#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "script.h"
#include "../vendor/quickjs/quickjs.h"
#include "dom_bootstrap.h"
#define JS_MEMORY_MAX (8*1024*1024)
#define SCRIPT_NODES_MAX 6000   /* pages with more DOM nodes are shown without their scripts */
#define COOKIES_MAX 8192        /* document.cookie as read */
#define POST_MAX (64*1024)      /* a form the scripts send */
typedef union { struct { size_t size; } h; long double align; void *pointer; } allocation;
static size_t usable(const void *p){return p?((const allocation *)p-1)->h.size:0;}
static void *allocate(JSMallocState *s,size_t size){if(!size||size>SIZE_MAX-sizeof(allocation)||size+sizeof(allocation)>s->malloc_limit-s->malloc_size)return NULL;allocation *p=malloc(size+sizeof(*p));if(!p)return NULL;p->h.size=size;s->malloc_count++;s->malloc_size+=size+sizeof(*p);return p+1;}
static void release(JSMallocState *s,void *p){if(p){s->malloc_size-=usable(p)+sizeof(allocation);s->malloc_count--;free((allocation *)p-1);}}
static void *resize(JSMallocState *s,void *p,size_t size){if(!p)return allocate(s,size);if(!size){release(s,p);return NULL;}size_t old=usable(p);if(size>SIZE_MAX-sizeof(allocation)||(size>old&&size-old>s->malloc_limit-s->malloc_size))return NULL;allocation *n=realloc((allocation *)p-1,size+sizeof(*n));if(!n)return NULL;n->h.size=size;s->malloc_size=s->malloc_size-old+size;return n+1;}
typedef struct {JSRuntime *runtime;JSContext *ctx;browser_document *doc;browser_fetch_fn fetch;void *fetch_ud;dom_cancel_fn cancel;void *cancel_ud;uint64_t start,network_wait;unsigned budget;int interrupted,fetches;size_t fetched;} script;
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int interrupt(JSRuntime *rt,void *ud){(void)rt;script *s=ud;int stop=(s->cancel&&s->cancel(s->cancel_ud))||(milliseconds()-s->start-s->network_wait>=s->budget);if(stop)s->interrupted=1;return stop;}
/* The browser scripts see: its user agent, its cookies, and how much of
   their memory a page whose scripts handle clicks may keep. */
static char agent[160]="Mozilla/5.0";
static browser_cookie_get_fn cookie_get;
static browser_cookie_set_fn cookie_set;
static size_t keep_max;
void browser_script_setup(const char *a,browser_cookie_get_fn get,browser_cookie_set_fn set,size_t keep){snprintf(agent,sizeof(agent),"%s",a&&*a?a:"Mozilla/5.0");cookie_get=get;cookie_set=set;keep_max=keep;}
/* document.cookie, read and set */
static JSValue cookie(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;(void)argc;(void)argv;script *s=JS_GetContextOpaque(ctx);char *out=malloc(COOKIES_MAX);if(!out)return JS_ThrowOutOfMemory(ctx);if(!cookie_get||cookie_get(s->doc->url,out,COOKIES_MAX)<0)out[0]=0;JSValue v=JS_NewString(ctx,out);free(out);return v;}
static JSValue set_cookie(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;script *s=JS_GetContextOpaque(ctx);const char *v=argc?JS_ToCString(ctx,argv[0]):NULL;if(!v)return JS_EXCEPTION;if(cookie_set)cookie_set(s->doc->url,v);JS_FreeCString(ctx,v);return JS_UNDEFINED;}
int browser_same_origin(const char *a,const char *b){char x[BROWSER_URL_MAX],y[BROWSER_URL_MAX];if(browser_url_resolve(NULL,a,x,sizeof(x))<0||browser_url_resolve(NULL,b,y,sizeof(y))<0)return 0;char *p=strstr(x,"://"),*q=strstr(y,"://");p=strchr(p+3,'/');q=strchr(q+3,'/');*p=*q=0;/* Normalize explicit default ports. */const char *dx=browser_url_secure(a)?":443":":80",*dy=browser_url_secure(b)?":443":":80";size_t n=strlen(x),m=strlen(dx);if(n>=m&&!strcmp(x+n-m,dx))x[n-m]=0;n=strlen(y);m=strlen(dy);if(n>=m&&!strcmp(y+n-m,dy))y[n-m]=0;return !strcmp(x,y);}
/* __resolve(address[, base]): an absolute address, against the page's or `base`. */
static JSValue resolve(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;script *s=JS_GetContextOpaque(ctx);const char *ref=argc?JS_ToCString(ctx,argv[0]):NULL;char target[BROWSER_URL_MAX],base[BROWSER_URL_MAX];if(!ref)return JS_EXCEPTION;
 int r=0;if(argc>1&&JS_IsString(argv[1])){const char *b=JS_ToCString(ctx,argv[1]);r=b?browser_url_resolve(s->doc->url,b,base,sizeof(base)):-1;if(b)JS_FreeCString(ctx,b);}else snprintf(base,sizeof(base),"%s",s->doc->url);
 if(r==0)r=browser_url_resolve(base,ref,target,sizeof(target));
 JS_FreeCString(ctx,ref);if(r<0)return JS_ThrowTypeError(ctx,"Unsupported address");return JS_NewString(ctx,target);}
/* __media(query): whether a media query matches the page view, as in CSS. */
static JSValue media(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;const char *q=argc?JS_ToCString(ctx,argv[0]):NULL;if(!q)return JS_EXCEPTION;int m=css_media_matches(q);JS_FreeCString(ctx,q);return JS_NewBool(ctx,m);}
static JSValue fetch(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){script *s=JS_GetContextOpaque(ctx);JSValue url=resolve(ctx,self,argc,argv);if(JS_IsException(url))return url;const char *u=JS_ToCString(ctx,url);if(!u){JS_FreeValue(ctx,url);return JS_EXCEPTION;}char error[256];int length=0;char *data=NULL;if(s->fetch&&browser_same_origin(s->doc->url,u)&&s->fetches++<8&&!interrupt(s->runtime,s)&&s->fetched<512*1024){int limit=256*1024;if((size_t)limit>512*1024-s->fetched)limit=(int)(512*1024-s->fetched);uint64_t waiting=milliseconds();data=s->fetch(s->fetch_ud,u,limit,&length,error,sizeof(error));s->network_wait+=milliseconds()-waiting;}JS_FreeCString(ctx,u);JS_FreeValue(ctx,url);if(!data)return JS_ThrowTypeError(ctx,"GET unavailable, cross-origin, cancelled or exceeds PSP limits");if(length<0||(size_t)length>256*1024||(size_t)length>512*1024-s->fetched){free(data);return JS_ThrowTypeError(ctx,"Response exceeds PSP limit");}s->fetched+=(size_t)length;JSValue v=JS_NewStringLen(ctx,data,(size_t)length);free(data);return v;}
static char *normalize_module(JSContext *ctx,const char *base,const char *name,void *ud)
{
    script *s=ud;char url[BROWSER_URL_MAX];
    if(browser_url_resolve(strstr(base,"://")?base:s->doc->url,name,url,sizeof(url))<0||!browser_same_origin(s->doc->url,url)) {
        JS_ThrowTypeError(ctx,"Unsupported or cross-origin module");return NULL;
    }
    return js_strdup(ctx,url);
}
static JSModuleDef *load_module(JSContext *ctx,const char *name,void *ud)
{
    script *s=ud;char err[256];int length=0;
    if(!s->fetch||s->fetches++>=8||s->fetched>=512*1024||interrupt(s->runtime,s)){JS_ThrowTypeError(ctx,"PSP module limit");return NULL;}
    int limit=256*1024;if((size_t)limit>512*1024-s->fetched)limit=(int)(512*1024-s->fetched);
    uint64_t waiting=milliseconds();char *code=s->fetch(s->fetch_ud,name,limit,&length,err,sizeof(err));s->network_wait+=milliseconds()-waiting;
    if(!code||length<0||length>limit){free(code);JS_ThrowTypeError(ctx,"Module unavailable");return NULL;}
    s->fetched+=(size_t)length;
    JSValue compiled=JS_Eval(ctx,code,(size_t)length,name,JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);free(code);
    if(JS_IsException(compiled))return NULL;
    JSModuleDef *module=JS_VALUE_GET_PTR(compiled);JS_FreeValue(ctx,compiled);return module;
}
static int evaluated(script *s,const char *code,size_t length,const char *name){JSValue v=JS_Eval(s->ctx,code,length,name,JS_EVAL_TYPE_GLOBAL);int fail=JS_IsException(v);JS_FreeValue(s->ctx,v);if(fail){v=JS_GetException(s->ctx);JS_FreeValue(s->ctx,v);}return fail?-1:0;}
static int jobs(script *s){JSContext *ctx;int n=0,r;while(JS_IsJobPending(s->runtime)){if(++n>2048||interrupt(s->runtime,s))return -1;r=JS_ExecutePendingJob(s->runtime,&ctx);if(r<0){JSValue v=JS_GetException(ctx);JS_FreeValue(ctx,v);return -1;}}return 0;}
/* Runs the timers set since the last round: how many they set in turn, -1 when stopped. */
static int timers(script *s){JSValue v=JS_Eval(s->ctx,"__timers()",10,"timers",JS_EVAL_TYPE_GLOBAL);int32_t n=-1;if(JS_IsException(v)){JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);}else if(JS_ToInt32(s->ctx,&n,v)<0)n=-1;JS_FreeValue(s->ctx,v);return jobs(s)<0?-1:n;}
/* Where the scripts sent the browser, if anywhere: a page address, and form data to send it. */
static void navigation(script *s,browser_redirect *to){
 JSValue n=JS_Eval(s->ctx,"__navigation()",14,"navigation",JS_EVAL_TYPE_GLOBAL);
 if(JS_IsException(n)){JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);return;}
 if(JS_IsObject(n)){JSValue url=JS_GetPropertyStr(s->ctx,n,"url"),post=JS_GetPropertyStr(s->ctx,n,"post"),replace=JS_GetPropertyStr(s->ctx,n,"replace");
  const char *u=JS_IsString(url)?JS_ToCString(s->ctx,url):NULL;size_t length=0;const char *p=JS_IsString(post)?JS_ToCStringLen(s->ctx,&length,post):NULL;
  if(u&&strlen(u)<sizeof(to->url)&&(!p||length<=POST_MAX)&&(!p||(to->post=malloc(length+1)))){strcpy(to->url,u);to->replace=JS_ToBool(s->ctx,replace)>0;if(p)memcpy(to->post,p,length+1);}
  if(u){JS_FreeCString(s->ctx,u);}if(p){JS_FreeCString(s->ctx,p);}JS_FreeValue(s->ctx,url);JS_FreeValue(s->ctx,post);JS_FreeValue(s->ctx,replace);}
 JS_FreeValue(s->ctx,n);}
/* The scripts' nodes a click does something on, a bit each; NULL for none. */
static unsigned char *clickables(script *s){
 JSValue list=JS_Eval(s->ctx,"__clickables()",14,"clickables",JS_EVAL_TYPE_GLOBAL);unsigned char *bits=NULL;int any=0;
 if(JS_IsException(list)){JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);return NULL;}
 int32_t length=0;if(JS_IsArray(s->ctx,list)>0){JSValue n=JS_GetPropertyStr(s->ctx,list,"length");if(JS_ToInt32(s->ctx,&length,n)<0)length=0;JS_FreeValue(s->ctx,n);}
 if(length>0&&(bits=calloc(DOM_NODES_MAX/8,1)))
  for(int32_t i=0;i<length;i++){JSValue v=JS_GetPropertyUint32(s->ctx,list,(uint32_t)i);int32_t id;if(JS_ToInt32(s->ctx,&id,v)==0&&id>=0&&id<DOM_NODES_MAX){bits[id>>3]|=(unsigned char)(1<<(id&7));any=1;}JS_FreeValue(s->ctx,v);}
 JS_FreeValue(s->ctx,list);if(!any){free(bits);bits=NULL;}return bits;}
/* The memory the scripts use. */
static size_t memory(script *s){JSMemoryUsage use;JS_ComputeMemoryUsage(s->runtime,&use);return use.malloc_size>0?(size_t)use.malloc_size:0;}
/* Marks them in the browser's DOM (data-flow-click), for the page view. */
static void mark(browser_dom *dom,const unsigned char *bits){for(int i=0;i<dom->count;i++){int id=dom->nodes[i].source_id;if(id>=0&&id<DOM_NODES_MAX&&bits[id>>3]&(1<<(id&7)))dom_set_attr(dom,i,"data-flow-click","");}}
/* The page as the scripts left it: 1 and a new DOM in *out, 0 when they
   didn't change it since the last time, -1 when it can't be read. With
   `keep`, the scripts keep their DOM for more clicks. */
static int snapshot(script *s,int keep,int shortened,browser_dom **out){
 const char *code=keep?"__snapshot(true)":"__snapshot(false)";*out=NULL;
 JSValue v=JS_Eval(s->ctx,code,strlen(code),"DOM snapshot",JS_EVAL_TYPE_GLOBAL);size_t length=0;const char *output=JS_IsException(v)?NULL:JS_ToCStringLen(s->ctx,&length,v);int result=-1;
 if(output&&!length)result=0;
 else if(output){browser_dom *next=calloc(1,sizeof(*next));if(next&&dom_from_json(next,output,length)>=0){next->shortened=shortened;*out=next;result=1;}else free(next);}
 if(output)JS_FreeCString(s->ctx,output);else if(JS_IsException(v)){JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);}
 JS_FreeValue(s->ctx,v);return result;}
void browser_script_move(browser_document *to,browser_document *from){script *s=from->script;from->script=NULL;to->script=s;if(s)s->doc=to;}
void browser_script_free(browser_document *doc){script *s=doc->script;if(!s)return;JS_FreeContext(s->ctx);JS_FreeRuntime(s->runtime);free(s);doc->script=NULL;}
/* Whether a node is a script that will run; `skipped` is set for one that
   won't because it exceeded the limits. (Another site's script, which isn't
   downloaded, has empty text and is passed over quietly.) */
static int will_run(const dom_node *n,int *skipped){
 if(strcmp(n->tag,"script"))return 0;
 const char *type=dom_attr(n,"type");
 if(*type&&strcmp(type,"module")&&strcmp(type,"text/javascript")&&strcmp(type,"application/javascript"))return 0;
 if((*dom_attr(n,"src")&&!n->text)||*dom_attr(n,"data-ark-omitted")){*skipped=1;return 0;}
 return n->text&&*n->text;}
int browser_scripts_present(const browser_document *doc){
 if(!doc->dom||!doc->dom->html)return 0;
 for(int i=0;i<doc->dom->count;i++){const dom_node *n=&doc->dom->nodes[i];if(strcmp(n->tag,"script"))continue;
  const char *type=dom_attr(n,"type"),*src=dom_attr(n,"src");char url[BROWSER_URL_MAX];
  if(*type&&strcmp(type,"module")&&strcmp(type,"text/javascript")&&strcmp(type,"application/javascript"))continue;
  if(*src?browser_url_resolve(doc->url,src,url,sizeof(url))==0&&browser_same_origin(doc->url,url):n->text&&*n->text)return 1;}
 return 0;}
int browser_script_run(browser_document *doc,browser_fetch_fn get,void *ud,dom_cancel_fn cancel,void *cancel_ud,unsigned budget_ms,char *err,size_t errlen){
 if(!doc->dom||!doc->dom->html)return 0;
 /* Nothing to run (no scripts, or only ones not downloaded): the page
    needn't make the round trip through JavaScript. */
 int runnable=0,missing=0;for(int i=0;i<doc->dom->count;i++){int skipped=0;runnable+=will_run(&doc->dom->nodes[i],&skipped);missing+=skipped;}
 if(!runnable){doc->scripts_failed+=missing;return 0;}
 /* A page this large wouldn't fit in the scripts' memory twice over (as
    objects, then as the snapshot): show it without them, quickly. */
 if(doc->dom->count>SCRIPT_NODES_MAX){doc->scripts_failed+=missing+runnable;snprintf(err,errlen,"This page is too large for its scripts; showing it without them.");return -1;}
 script *s=calloc(1,sizeof(*s));if(!s)return -1;const JSMallocFunctions mf={allocate,release,resize,usable};s->runtime=JS_NewRuntime2(&mf,NULL);if(!s->runtime){free(s);return -1;}JS_SetMemoryLimit(s->runtime,JS_MEMORY_MAX);JS_SetMaxStackSize(s->runtime,64*1024);s->ctx=JS_NewContext(s->runtime);if(!s->ctx){JS_FreeRuntime(s->runtime);free(s);return -1;}doc->script=s;s->doc=doc;s->fetch=get;s->fetch_ud=ud;s->cancel=cancel;s->cancel_ud=cancel_ud;s->start=milliseconds();s->budget=budget_ms?budget_ms:30000;JS_SetContextOpaque(s->ctx,s);JS_SetInterruptHandler(s->runtime,interrupt,s);JS_SetModuleLoaderFunc(s->runtime,normalize_module,load_module,s);
 char *json=dom_json(doc->dom);int result=-1,kept=0;if(!json)goto end;
 JSValue global=JS_GetGlobalObject(s->ctx),seed=JS_ParseJSON(s->ctx,json,strlen(json),"DOM");free(json);if(JS_IsException(seed)){JS_FreeValue(s->ctx,global);goto end;}
 JS_SetPropertyStr(s->ctx,global,"__seed",seed);JS_SetPropertyStr(s->ctx,global,"__url",JS_NewString(s->ctx,doc->url));JS_SetPropertyStr(s->ctx,global,"__resolve",JS_NewCFunction(s->ctx,resolve,"resolve",1));JS_SetPropertyStr(s->ctx,global,"__fetch",JS_NewCFunction(s->ctx,fetch,"fetch",1));JS_SetPropertyStr(s->ctx,global,"__media",JS_NewCFunction(s->ctx,media,"media",1));
 JS_SetPropertyStr(s->ctx,global,"__cookie",JS_NewCFunction(s->ctx,cookie,"cookie",0));JS_SetPropertyStr(s->ctx,global,"__set_cookie",JS_NewCFunction(s->ctx,set_cookie,"set_cookie",1));
 JS_SetPropertyStr(s->ctx,global,"__agent",JS_NewString(s->ctx,agent));JS_SetPropertyStr(s->ctx,global,"__cookies",JS_NewBool(s->ctx,cookie_get!=NULL));JS_FreeValue(s->ctx,global);
 if(evaluated(s,dom_bootstrap,sizeof(dom_bootstrap)-1,"psp-dom.js")<0)goto end;
 /* A fixed initial script list: inserted scripts are not executed. */
 for(int i=0;i<doc->dom->count&&!s->interrupted;i++){dom_node *n=&doc->dom->nodes[i];int skipped=0;if(!will_run(n,&skipped)){doc->scripts_failed+=skipped;continue;}int module=!strcmp(dom_attr(n,"type"),"module");doc->scripts_run++;int failed;
    if(module){char name[BROWSER_URL_MAX];if(browser_url_resolve(doc->url,dom_attr(n,"src"),name,sizeof(name))<0)strcpy(name,doc->url);JSValue v=JS_Eval(s->ctx,n->text,strlen(n->text),name,JS_EVAL_TYPE_MODULE);failed=JS_IsException(v);JS_FreeValue(s->ctx,v);if(failed){v=JS_GetException(s->ctx);JS_FreeValue(s->ctx,v);}}
    else failed=evaluated(s,n->text,strlen(n->text),*dom_attr(n,"src")?dom_attr(n,"src"):"inline.js")<0;
    if(failed||jobs(s)<0)doc->scripts_failed++;}
 if(s->interrupted)goto end;
 if(evaluated(s,"__finish()",10,"load")<0||jobs(s)<0){doc->scripts_failed++;if(s->interrupted)goto end;}
 /* The timers those timers set, a few rounds: a notice shown a moment
    after the page, a menu set up after a delay. */
 for(int round=0;round<3&&timers(s)>0;round++);
 if(s->interrupted)goto end;
 navigation(s,&doc->redirect);
 /* Scripts handling clicks stay with the page, when they fit in memory:
    their DOM stays theirs, and the browser's copy notes what they handle. */
 unsigned char *bits=!doc->redirect.url[0]&&keep_max?clickables(s):NULL;
 /* too big already: the browser's copy of the page needs the room */
 if(bits&&memory(s)>keep_max){free(bits);bits=NULL;}
 browser_dom *next;int changed=snapshot(s,bits!=NULL,doc->dom->shortened,&next);
 if(changed<0)doc->scripts_failed++;
 else if(!next)result=0;    /* the scripts left the page as it was: keep it */
 else{dom_free(doc->dom);free(doc->dom);doc->dom=next;result=browser_document_render(doc,err,errlen);}
 if(bits&&result==0&&!s->interrupted){JS_RunGC(s->runtime);if(memory(s)<=keep_max){mark(doc->dom,bits);kept=1;}}
 free(bits);
 end:if(result<0){doc->scripts_failed++;snprintf(err,errlen,"JavaScript stopped (error, time or memory limit); showing the readable page.");}if(!kept)browser_script_free(doc);return result;
}
int browser_script_click(browser_document *doc,int node,const char *values,browser_fetch_fn get,void *ud,dom_cancel_fn cancel,void *cancel_ud,
                         unsigned budget_ms,browser_dom **changed,browser_redirect *to,char *err,size_t errlen){
 script *s=doc->script;*changed=NULL;memset(to,0,sizeof(*to));
 if(!s){snprintf(err,errlen,"This page's scripts have stopped. Reload the page to use them.");return -1;}
 JS_UpdateStackTop(s->runtime);
 s->fetch=get;s->fetch_ud=ud;s->cancel=cancel;s->cancel_ud=cancel_ud;s->start=milliseconds();s->network_wait=0;s->budget=budget_ms?budget_ms:10000;s->interrupted=0;s->fetches=0;s->fetched=0;
 JSValue global=JS_GetGlobalObject(s->ctx),result;
 /* what was typed into the page's fields, then the click */
 if(values&&*values){JSValue list=JS_ParseJSON(s->ctx,values,strlen(values),"values"),f=JS_GetPropertyStr(s->ctx,global,"__set_values");
  result=JS_IsException(list)?JS_EXCEPTION:JS_Call(s->ctx,f,JS_UNDEFINED,1,(JSValueConst *)&list);
  if(JS_IsException(result)){JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);}JS_FreeValue(s->ctx,result);JS_FreeValue(s->ctx,f);JS_FreeValue(s->ctx,list);}
 JSValue f=JS_GetPropertyStr(s->ctx,global,"__click"),id=JS_NewInt32(s->ctx,node);
 result=JS_Call(s->ctx,f,JS_UNDEFINED,1,(JSValueConst *)&id);
 if(JS_IsException(result)){JSValue e=JS_GetException(s->ctx);JS_FreeValue(s->ctx,e);}
 JS_FreeValue(s->ctx,result);JS_FreeValue(s->ctx,f);JS_FreeValue(s->ctx,global);
 jobs(s);
 for(int round=0;round<5&&!s->interrupted&&timers(s)>0;round++);
 if(s->interrupted)goto stopped;
 navigation(s,to);
 if(to->url[0])return 0;
 unsigned char *bits=clickables(s);
 int r=snapshot(s,1,doc->dom->shortened,changed);
 if(r<0||s->interrupted){free(bits);if(*changed){dom_free(*changed);free(*changed);*changed=NULL;}goto stopped;}
 if(*changed&&bits)mark(*changed,bits);
 free(bits);
 return 0;
 stopped:snprintf(err,errlen,"The page's script stopped (error, time or memory limit).");browser_script_free(doc);return -1;
}

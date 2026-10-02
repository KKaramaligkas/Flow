#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "script.h"
#include "../vendor/quickjs/quickjs.h"
#include "dom_bootstrap.h"
#define JS_MEMORY_MAX (8*1024*1024)
typedef union { struct { size_t size; } h; long double align; void *pointer; } allocation;
static size_t usable(const void *p){return p?((const allocation *)p-1)->h.size:0;}
static void *allocate(JSMallocState *s,size_t size){if(!size||size>SIZE_MAX-sizeof(allocation)||size+sizeof(allocation)>s->malloc_limit-s->malloc_size)return NULL;allocation *p=malloc(size+sizeof(*p));if(!p)return NULL;p->h.size=size;s->malloc_count++;s->malloc_size+=size+sizeof(*p);return p+1;}
static void release(JSMallocState *s,void *p){if(p){s->malloc_size-=usable(p)+sizeof(allocation);s->malloc_count--;free((allocation *)p-1);}}
static void *resize(JSMallocState *s,void *p,size_t size){if(!p)return allocate(s,size);if(!size){release(s,p);return NULL;}size_t old=usable(p);if(size>SIZE_MAX-sizeof(allocation)||(size>old&&size-old>s->malloc_limit-s->malloc_size))return NULL;allocation *n=realloc((allocation *)p-1,size+sizeof(*n));if(!n)return NULL;n->h.size=size;s->malloc_size=s->malloc_size-old+size;return n+1;}
typedef struct {JSRuntime *runtime;JSContext *ctx;browser_document *doc;browser_fetch_fn fetch;void *fetch_ud;dom_cancel_fn cancel;void *cancel_ud;uint64_t start,network_wait;unsigned budget;int interrupted,fetches;size_t fetched;} script;
static uint64_t milliseconds(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)<0)return 0;return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
static int interrupt(JSRuntime *rt,void *ud){(void)rt;script *s=ud;int stop=(s->cancel&&s->cancel(s->cancel_ud))||(milliseconds()-s->start-s->network_wait>=s->budget);if(stop)s->interrupted=1;return stop;}
int browser_same_origin(const char *a,const char *b){char x[BROWSER_URL_MAX],y[BROWSER_URL_MAX];if(browser_url_resolve(NULL,a,x,sizeof(x))<0||browser_url_resolve(NULL,b,y,sizeof(y))<0)return 0;char *p=strstr(x,"://"),*q=strstr(y,"://");p=strchr(p+3,'/');q=strchr(q+3,'/');*p=*q=0;/* Normalize explicit default ports. */const char *dx=browser_url_secure(a)?":443":":80",*dy=browser_url_secure(b)?":443":":80";size_t n=strlen(x),m=strlen(dx);if(n>=m&&!strcmp(x+n-m,dx))x[n-m]=0;n=strlen(y);m=strlen(dy);if(n>=m&&!strcmp(y+n-m,dy))y[n-m]=0;return !strcmp(x,y);}
static JSValue resolve(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){(void)self;script *s=JS_GetContextOpaque(ctx);const char *ref=argc?JS_ToCString(ctx,argv[0]):NULL;char target[BROWSER_URL_MAX];if(!ref)return JS_EXCEPTION;int r=browser_url_resolve(s->doc->url,ref,target,sizeof(target));JS_FreeCString(ctx,ref);if(r<0)return JS_ThrowTypeError(ctx,"Unsupported address");return JS_NewString(ctx,target);}
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
void browser_script_free(browser_document *doc){script *s=doc->script;if(!s)return;JS_FreeContext(s->ctx);JS_FreeRuntime(s->runtime);free(s);doc->script=NULL;}
int browser_script_run(browser_document *doc,browser_fetch_fn get,void *ud,dom_cancel_fn cancel,void *cancel_ud,unsigned budget_ms,char *err,size_t errlen){
 if(!doc->dom||!doc->dom->html)return 0;
 int present=0;for(int i=0;i<doc->dom->count;i++)if(!strcmp(doc->dom->nodes[i].tag,"script"))present=1;if(!present)return 0;
 script *s=calloc(1,sizeof(*s));if(!s)return -1;const JSMallocFunctions mf={allocate,release,resize,usable};s->runtime=JS_NewRuntime2(&mf,NULL);if(!s->runtime){free(s);return -1;}JS_SetMemoryLimit(s->runtime,JS_MEMORY_MAX);JS_SetMaxStackSize(s->runtime,64*1024);s->ctx=JS_NewContext(s->runtime);if(!s->ctx){JS_FreeRuntime(s->runtime);free(s);return -1;}doc->script=s;s->doc=doc;s->fetch=get;s->fetch_ud=ud;s->cancel=cancel;s->cancel_ud=cancel_ud;s->start=milliseconds();s->budget=budget_ms?budget_ms:30000;JS_SetContextOpaque(s->ctx,s);JS_SetInterruptHandler(s->runtime,interrupt,s);JS_SetModuleLoaderFunc(s->runtime,normalize_module,load_module,s);
 char *json=dom_json(doc->dom);int result=-1;if(!json)goto end;
 JSValue global=JS_GetGlobalObject(s->ctx),seed=JS_ParseJSON(s->ctx,json,strlen(json),"DOM");free(json);if(JS_IsException(seed)){JS_FreeValue(s->ctx,global);goto end;}
 JS_SetPropertyStr(s->ctx,global,"__seed",seed);JS_SetPropertyStr(s->ctx,global,"__url",JS_NewString(s->ctx,doc->url));JS_SetPropertyStr(s->ctx,global,"__resolve",JS_NewCFunction(s->ctx,resolve,"resolve",1));JS_SetPropertyStr(s->ctx,global,"__fetch",JS_NewCFunction(s->ctx,fetch,"fetch",1));JS_FreeValue(s->ctx,global);
 if(evaluated(s,dom_bootstrap,sizeof(dom_bootstrap)-1,"psp-dom.js")<0)goto end;
 /* A fixed initial script list: inserted scripts are not executed. */
 for(int i=0;i<doc->dom->count&&!s->interrupted;i++){dom_node *n=&doc->dom->nodes[i];if(strcmp(n->tag,"script"))continue;const char *type=dom_attr(n,"type");int module=!strcmp(type,"module");if(*type&&!module&&strcmp(type,"text/javascript")&&strcmp(type,"application/javascript"))continue;if((*dom_attr(n,"src")&&!n->text)||*dom_attr(n,"data-ark-omitted")){doc->scripts_failed++;continue;}if(!n->text||!*n->text)continue;doc->scripts_run++;int failed;
    if(module){char name[BROWSER_URL_MAX];if(browser_url_resolve(doc->url,dom_attr(n,"src"),name,sizeof(name))<0)strcpy(name,doc->url);JSValue v=JS_Eval(s->ctx,n->text,strlen(n->text),name,JS_EVAL_TYPE_MODULE);failed=JS_IsException(v);JS_FreeValue(s->ctx,v);if(failed){v=JS_GetException(s->ctx);JS_FreeValue(s->ctx,v);}}
    else failed=evaluated(s,n->text,strlen(n->text),*dom_attr(n,"src")?dom_attr(n,"src"):"inline.js")<0;
    if(failed||jobs(s)<0)doc->scripts_failed++;}
 if(s->interrupted)goto end;
 if(evaluated(s,"__finish()",10,"load")<0||jobs(s)<0){doc->scripts_failed++;if(s->interrupted)goto end;}
 JSValue snapshot=JS_Eval(s->ctx,"__snapshot()",12,"DOM snapshot",JS_EVAL_TYPE_GLOBAL);size_t length=0;const char *output=JS_IsException(snapshot)?NULL:JS_ToCStringLen(s->ctx,&length,snapshot);
 browser_dom *next=calloc(1,sizeof(*next));if(output&&next&&dom_from_json(next,output,length)>=0){next->shortened=doc->dom->shortened;dom_free(doc->dom);free(doc->dom);doc->dom=next;result=browser_document_render(doc,err,errlen);}else {free(next);doc->scripts_failed++;}
 if(output)JS_FreeCString(s->ctx,output);
 JS_FreeValue(s->ctx,snapshot);
 end:if(result<0){doc->scripts_failed++;snprintf(err,errlen,"JavaScript stopped (error, time or memory limit); showing the readable page.");}browser_script_free(doc);return result;
}

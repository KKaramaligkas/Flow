#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cjson/cJSON.h>
#include "dom.h"

static int eq(const char *a, const char *b)
{
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
static char *copy(browser_dom *dom, const char *s)
{
    size_t n = strlen(s) + 1;
    if (n > DOM_BYTES_MAX - dom->bytes) { dom->shortened = 1; return NULL; }
    char *p = malloc(n);
    if (p) { memcpy(p, s, n); dom->bytes += n; }
    return p;
}
int dom_add(browser_dom *dom, int parent, const char *tag, const char *text)
{
    if (dom->count >= DOM_NODES_MAX || strlen(tag) >= sizeof(dom->nodes[0].tag)) { dom->shortened = 1; return -1; }
    if (dom->count >= dom->capacity) {
        /* Most pages need a fraction of the maximum. */
        int capacity = dom->capacity ? dom->capacity * 2 : 1024;
        if (capacity > DOM_NODES_MAX) capacity = DOM_NODES_MAX;
        dom_node *nodes = realloc(dom->nodes, (size_t)capacity * sizeof(*nodes));
        if (!nodes) { dom->shortened = 1; return -1; }
        dom->nodes = nodes; dom->capacity = capacity;
    }
    int i = dom->count;
    dom_node *n = &dom->nodes[i];
    memset(n, 0, sizeof(*n)); n->parent = parent; n->first = n->last = n->next = -1;
    strcpy(n->tag, tag); n->source_id=i;
    if (text && !(n->text = copy(dom, text))) return -1;
    dom->count++;
    if (parent >= 0) {
        if (dom->nodes[parent].last >= 0) dom->nodes[dom->nodes[parent].last].next = i;
        else dom->nodes[parent].first = i;
        dom->nodes[parent].last = i;
    }
    return i;
}
const char *dom_attr(const dom_node *n, const char *name)
{
    for (int i = 0; i < n->attribute_count; i++) if (eq(n->attributes[i].name, name)) return n->attributes[i].value;
    return "";
}
int dom_set_attr(browser_dom *dom, int index, const char *name, const char *value)
{
    dom_node *n = &dom->nodes[index];
    if (!*name || strlen(name) > 64 || strlen(value) > 4096 ) return -1;
    for (int i = 0; i < n->attribute_count; i++) if (eq(name, n->attributes[i].name)) {
        char *v = copy(dom, value); if (!v) return -1;
        dom->bytes -= strlen(n->attributes[i].value) + 1;
        free(n->attributes[i].value); n->attributes[i].value = v; return 0;
    }
    if(n->attribute_count>=32)return -1;
    char *key = copy(dom, name), *v = copy(dom, value);
    if (!key || !v) { if (key) { dom->bytes -= strlen(key)+1; free(key); } if(v) { dom->bytes-=strlen(v)+1;free(v); } return -1; }
    dom_attribute *attributes = realloc(n->attributes, (n->attribute_count + 1) * sizeof(*attributes));
    if (!attributes) { dom->bytes -= strlen(key)+strlen(v)+2;free(key); free(v); return -1; }
    n->attributes = attributes; attributes[n->attribute_count++] = (dom_attribute){key, v}; return 0;
}
void dom_free(browser_dom *dom)
{
    for (int i = 0; i < dom->count; i++) {
        dom_node *n = &dom->nodes[i]; free(n->text);
        for (int j = 0; j < n->attribute_count; j++) { free(n->attributes[j].name); free(n->attributes[j].value); }
        free(n->attributes);
    }
    free(dom->nodes); memset(dom, 0, sizeof(*dom));
}

typedef struct {
    dom_read_fn read; void *ud; dom_cancel_fn cancel; void *cancel_ud;
    char buffer[4096]; int used, size, failed, look;
    size_t bytes;
} reader;
static int get(reader *r)
{
    if (r->look != -2) { int c=r->look;r->look=-2;return c; }
    if (r->used == r->size) {
        if (r->cancel && r->cancel(r->cancel_ud)) { r->failed = 2; return -1; }
        r->size = r->read(r->ud, r->buffer, sizeof(r->buffer)); r->used = 0;
        if (r->size < 0 || r->size > (int)sizeof(r->buffer)) { r->failed=1;return -1; }
        if (!r->size) return -1;
    }
    unsigned char c = (unsigned char)r->buffer[r->used++];
    if (++r->bytes > DOM_SOURCE_MAX) { r->failed=3;return -1; }
    if (c < 9 || (c > 13 && c < 32)) { r->failed=4;return -1; }
    return c;
}
static int peek(reader *r) { if(r->look==-2)r->look=get(r);return r->look; }
static int whitespace(int c) { return c >= 0 && isspace((unsigned char)c); }
static int empty_tag(const char *tag)
{
    static const char *tags[]={"br","hr","img","input","meta","link","base","area","embed","source","wbr","param","col"};
    for(size_t i=0;i<sizeof(tags)/sizeof(*tags);i++)if(eq(tag,tags[i]))return 1;
    return 0;
}
static void attributes(browser_dom *dom, int index, const char *p)
{
    while (*p) {
        while (whitespace(*p) || *p=='/') p++;
        const char *start=p;
        while (*p && !whitespace(*p) && *p!='=' && *p!='/') p++;
        size_t n=(size_t)(p-start); if(!n){if(*p)p++;continue;}
        char name[65]; if(n>=sizeof(name))n=sizeof(name)-1;
        for(size_t i=0;i<n;i++)name[i]=(char)tolower((unsigned char)start[i]);
        name[n]=0;
        while(whitespace(*p))p++;
        const char *v=p;size_t length=0;
        if(*p=='=') {
            p++;while(whitespace(*p))p++;
            char quote=*p=='\''||*p=='"'?*p++:0;v=p;
            while(*p&&(quote?*p!=quote:!whitespace(*p)))p++;
            length=(size_t)(p-v);if(quote&&*p)p++;
        }
        if(length>4096)continue;
        char *value=browser_decode(v,length,1,0);
        if(value){dom_set_attr(dom,index,name,value);free(value);}
    }
}
static int pending_entity(const char *text,size_t length)
{
    for(size_t i=length;i>0&&length-i<32;i--){char c=text[i-1];if(c==';'||c=='<'||whitespace((unsigned char)c))return 0;if(c=='&')return 1;}
    return 0;
}
/* Appends to a raw text buffer that grows as needed, up to `maximum`. */
static int raw_add(char **out,size_t *used,size_t *capacity,size_t maximum,char c,int *shortened)
{
    if(*used>=maximum){*shortened=1;return 0;}
    if(*used==*capacity) {
        size_t grown=*capacity*2<maximum?*capacity*2:maximum;
        char *p=realloc(*out,grown+1);if(!p)return -1;
        *out=p;*capacity=grown;
    }
    (*out)[(*used)++]=c;return 0;
}
/* Raw script/style text may contain '<'. Only its matching end tag ends it.
   The buffer grows with the text: pages have dozens of small scripts. */
static char *raw(reader *r, const char *tag, size_t maximum, int *shortened)
{
    size_t used=0,capacity=maximum<256?maximum:256;
    char *out=malloc(capacity+1);if(!out)return NULL;
    int c,failed=0;
    while(!failed&&(c=get(r))>=0) {
        if(c=='<') {
            char candidate[64];size_t n=0;candidate[n++]='<';
            if(peek(r)=='/') {
                candidate[n++]=(char)get(r);
                while(n<sizeof(candidate)-2 && isalpha((unsigned char)peek(r)))candidate[n++]=(char)get(r);
                candidate[n]=0;
                if(eq(candidate+2,tag) && (peek(r)=='>' || peek(r)=='/' || whitespace(peek(r)))) {
                    while((c=get(r))>=0&&c!='>'){};break;
                }
            }
            for(size_t i=0;i<n&&!failed;i++)failed=raw_add(&out,&used,&capacity,maximum,candidate[i],shortened)<0;
        } else failed=raw_add(&out,&used,&capacity,maximum,(char)c,shortened)<0;
    }
    if(failed){free(out);return NULL;}
    out[used]=0;
    char *fitted=realloc(out,used+1);
    return fitted?fitted:out;
}
/* Skips an element's content up to its matching end tag. An SVG drawing's
   paths and a template's inert markup would take DOM nodes, often hundreds,
   for nothing the page view shows. */
static void skip_content(reader *r, const char *tag)
{
    int depth=1,c;
    while(depth>0&&(c=get(r))>=0) {
        if(c!='<')continue;
        int closing=peek(r)=='/';if(closing)get(r);
        char name[16];size_t n=0;
        while(n<sizeof(name)-1&&(isalnum((unsigned char)peek(r))||peek(r)=='-'))name[n++]=(char)tolower(get(r));
        name[n]=0;
        if(!eq(name,tag)||isalnum((unsigned char)peek(r)))continue;
        int previous=0;char quote=0,self=0;
        while((c=get(r))>=0){if(quote){if(c==quote)quote=0;}else if(c=='"'||c=='\'')quote=(char)c;else if(c=='>'){self=previous=='/';break;}previous=c;}
        if(closing)depth--;else if(!self)depth++;
    }
}
static int listed(const char *tag,const char *const *list){for(;*list;list++)if(eq(tag,*list))return 1;return 0;}
/* The depth after closing the open element in `targets` nearest the top of
   the stack, unless an element in `stops` comes first. */
static int close_open(const browser_dom *dom,const int *stack,int depth,const char *const *targets,const char *const *stops)
{
    for(int j=depth-1;j>0;j--){
        const char *open=dom->nodes[stack[j]].tag;
        if(listed(open,targets))return j;
        if(listed(open,stops))break;
    }
    return depth;
}
/* HTML's implied end tags: a block closes an open <p> (else everything
   after <p class=hidden> would be hidden with it), and items, cells and rows
   close the previous one. */
static int implied_end(const browser_dom *dom,const int *stack,int depth,const char *tag)
{
    static const char *const p_closers[]={"address","article","aside","blockquote","center","details","dialog","dir","div","dl","dd","dt",
        "fieldset","figcaption","figure","footer","form","h1","h2","h3","h4","h5","h6","header","hgroup","hr","li","listing","main","menu",
        "nav","ol","p","pre","search","section","table","ul","xmp",NULL};
    static const char *const p_scope[]={"button","table","td","th","caption","html","object","marquee","applet","template",NULL};
    static const char *const p[]={"p",NULL},*const li[]={"li",NULL},*const lists[]={"ul","ol","menu","table","td","th",NULL};
    static const char *const definitions[]={"dd","dt",NULL},*const dl[]={"dl","table",NULL};
    static const char *const cells[]={"td","th",NULL},*const row[]={"tr","table",NULL},*const tr[]={"tr",NULL};
    static const char *const sections[]={"tbody","thead","tfoot",NULL},*const table[]={"table",NULL};
    static const char *const table_parts[]={"table","tbody","thead","tfoot",NULL};
    static const char *const option[]={"option",NULL},*const optgroup[]={"optgroup",NULL};
    static const char *const select[]={"select","datalist","optgroup",NULL},*const select_only[]={"select","datalist",NULL};
    if(listed(tag,p_closers))depth=close_open(dom,stack,depth,p,p_scope);
    if(eq(tag,"li"))depth=close_open(dom,stack,depth,li,lists);
    if(eq(tag,"dd")||eq(tag,"dt"))depth=close_open(dom,stack,depth,definitions,dl);
    if(eq(tag,"td")||eq(tag,"th"))depth=close_open(dom,stack,depth,cells,row);
    if(eq(tag,"tr"))depth=close_open(dom,stack,depth,tr,table_parts);
    if(listed(tag,sections))depth=close_open(dom,stack,depth,sections,table);
    if(eq(tag,"option")||eq(tag,"optgroup"))depth=close_open(dom,stack,depth,option,select);
    if(eq(tag,"optgroup"))depth=close_open(dom,stack,depth,optgroup,select_only);
    return depth;
}
int dom_parse(browser_dom *dom, dom_read_fn read, void *ud, int html, int latin,
              dom_cancel_fn cancel, void *cancel_ud, char *err, size_t errlen)
{
    memset(dom,0,sizeof(*dom));dom->html=html;
    if(dom_add(dom,-1,"#document",NULL)<0){snprintf(err,errlen,"Not enough memory for the page.");return -1;}
    reader r={.read=read,.ud=ud,.cancel=cancel,.cancel_ud=cancel_ud,.look=-2};
    int stack[DOM_DEPTH_MAX]={0},depth=1,c;
    char token[DOM_TOKEN_MAX+1];
    while((c=get(&r))>=0 && !dom->shortened) {
        if(!html||c!='<') {
            size_t n=0;token[n++]=(char)c;
            while(n<DOM_TOKEN_MAX-64&&(c=peek(&r))>=0&&(!html||c!='<'))token[n++]=(char)get(&r);
            /* Continue a UTF-8 sequence/entity through a buffer boundary. */
            while(n<DOM_TOKEN_MAX&&(c=peek(&r))>=0&&(((unsigned)c&0xc0)==0x80 || (pending_entity(token,n)&&c!='<'&&!whitespace(c))))token[n++]=(char)get(&r);
            token[n]=0;char *text=browser_decode(token,n,html,latin);
            if(!text||dom_add(dom,stack[depth-1],"#text",text)<0){free(text);dom->shortened=1;break;}
            free(text);continue;
        }
        if(peek(&r)=='!') {
            get(&r);
            if(peek(&r)=='-') {
                get(&r);if(peek(&r)=='-')get(&r);
                int previous=0,before=0;
                while((c=get(&r))>=0){if(before=='-'&&previous=='-'&&c=='>')break;before=previous;previous=c;}
            }else while((c=get(&r))>=0&&c!='>'){}
            continue;
        }
        /* A quote starts a value only after '=', as in browsers: a stray
           quote in a tag (<div "="" class=x>) must not swallow the page. */
        size_t n=0;char quote=0;int oversized=0,value=0;
        while((c=get(&r))>=0) {
            if(quote){if(c==quote)quote=0;}
            else if((c=='\''||c=='"')&&value)quote=(char)c;
            else if(c=='>')break;
            if(!quote){if(c=='=')value=1;else if(!whitespace(c))value=0;}
            if(n<DOM_TOKEN_MAX)token[n++]=(char)c;else oversized=1;
        }
        token[n]=0;char *p=token;while(whitespace(*p))p++;
        int closing=*p=='/';if(closing)p++;
        char tag[32];size_t k=0;
        while(isalnum((unsigned char)*p)||*p=='-'||*p==':'){if(k<sizeof(tag)-1)tag[k++]=(char)tolower((unsigned char)*p);p++;}tag[k]=0;
        if(!*tag)continue;
        if(closing) {
            for(int j=depth-1;j>0;j--)if(eq(dom->nodes[stack[j]].tag,tag)){depth=j;break;}
            continue;
        }
        /* Recover common omitted HTML end tags. */
        if(eq(tag,"body"))for(int j=depth-1;j>0;j--)if(eq(dom->nodes[stack[j]].tag,"head")){depth=j;break;}
        if(html)depth=implied_end(dom,stack,depth,tag);
        int index=dom_add(dom,stack[depth-1],tag,NULL);
        if(index<0){dom->shortened=1;break;}
        if(!oversized)attributes(dom,index,p);
        if(eq(tag,"script")||eq(tag,"style")) {
            int cut=0;char *text=raw(&r,tag,eq(tag,"script")?512*1024:256*1024,&cut);
            if(!text){dom->shortened=1;break;}
            if(eq(tag,"script")) {
                size_t bytes=strlen(text)+1;
                if(!cut&&bytes<=1024*1024-dom->script_bytes){dom->nodes[index].text=text;dom->script_bytes+=bytes;text=NULL;}
                else cut=1;
            } else dom->nodes[index].text=copy(dom,text);
            free(text);
            if(cut)dom_set_attr(dom,index,"data-ark-omitted","1");
            continue;
        }
        if(!empty_tag(tag) && !(n&&token[n-1]=='/')) {
            if(html&&(eq(tag,"svg")||eq(tag,"template"))){skip_content(&r,tag);continue;}
            if(depth==DOM_DEPTH_MAX){dom->shortened=1;break;}
            stack[depth++]=index;
        }
    }
    dom->source_bytes=r.bytes;
    if(r.failed){snprintf(err,errlen,"%s",r.failed==2?"Cancelled":r.failed==3?"Page exceeds 8 MB.":r.failed==4?"Binary content. Select Download to save it.":"Could not read the page.");dom_free(dom);return -1;}
    return 0;
}

/* cJSON is used only by the browser worker. Bound temporary snapshot memory,
   including many tiny JSON values, independently of the JS heap limit. */
typedef union {size_t size;long double align;void *pointer;} json_allocation;
static size_t json_bytes;
static void *json_alloc(size_t size){if(size>4*1024*1024-json_bytes||size>SIZE_MAX-sizeof(json_allocation))return NULL;json_allocation *p=malloc(sizeof(*p)+size);if(!p)return NULL;p->size=size;json_bytes+=size;return p+1;}
static void json_release(void *ptr){if(ptr){json_allocation *p=(json_allocation *)ptr-1;json_bytes-=p->size;free(p);}}
static void json_begin(void){cJSON_Hooks hooks={json_alloc,json_release};json_bytes=0;cJSON_InitHooks(&hooks);}
static void json_end(void){cJSON_InitHooks(NULL);}
/* The DOM as JSON for scripts, written directly: a cJSON tree of a large
   page would take several times the memory. */
typedef struct { char *data; size_t used, capacity; int failed; } json_writer;
static void json_put(json_writer *w,const char *s,size_t n)
{
    if(w->failed)return;
    if(w->used+n+1>w->capacity){
        size_t capacity=w->capacity?w->capacity:64*1024;
        while(capacity<w->used+n+1)capacity*=2;
        char *data=capacity<=4*1024*1024?realloc(w->data,capacity):NULL;
        if(!data){w->failed=1;return;}
        w->data=data;w->capacity=capacity;
    }
    memcpy(w->data+w->used,s,n);w->used+=n;
}
static void json_string(json_writer *w,const char *s)
{
    json_put(w,"\"",1);
    for(const char *run=s;;s++){
        unsigned char c=(unsigned char)*s;
        if(c&&c!='"'&&c!='\\'&&c>=0x20)continue;
        json_put(w,run,(size_t)(s-run));
        if(!c)break;
        char escape[8];snprintf(escape,sizeof(escape),c=='"'||c=='\\'?"\\%c":"\\u%04x",c);json_put(w,escape,strlen(escape));
        run=s+1;
    }
    json_put(w,"\"",1);
}
char *dom_json(const browser_dom *dom)
{
    json_writer w={0};char number[16];
    json_put(&w,"[",1);
    for(int i=0;i<dom->count&&!w.failed;i++){
        const dom_node *n=&dom->nodes[i];
        json_put(&w,i?",{\"tag\":":"{\"tag\":",i?8:7);json_string(&w,n->tag);
        json_put(&w,",\"text\":",8);json_string(&w,eq(n->tag,"script")?"":n->text?n->text:"");
        json_put(&w,",\"attrs\":{",10);
        for(int j=0;j<n->attribute_count;j++){if(j)json_put(&w,",",1);json_string(&w,n->attributes[j].name);json_put(&w,":",1);json_string(&w,n->attributes[j].value);}
        json_put(&w,"},\"children\":[",14);
        for(int j=n->first;j>=0;j=dom->nodes[j].next){int k=snprintf(number,sizeof(number),j==n->first?"%d":",%d",j);json_put(&w,number,(size_t)k);}
        json_put(&w,"]}",2);
    }
    json_put(&w,"]",1);
    if(w.failed){free(w.data);return NULL;}
    w.data[w.used]=0;
    return w.data;
}
/* `items` indexes the array: cJSON arrays are lists, slow to index. */
static int import_node(browser_dom *dom,const cJSON *const *items,int count,int index,int parent,int depth,unsigned char *seen)
{
    if(index<0||index>=count||seen[index]||depth>=DOM_DEPTH_MAX)return -1;
    seen[index]=1;const cJSON *n=items[index],*tag=cJSON_GetObjectItemCaseSensitive(n,"tag"),*text=cJSON_GetObjectItemCaseSensitive(n,"text"),*a=cJSON_GetObjectItemCaseSensitive(n,"attrs"),*children=cJSON_GetObjectItemCaseSensitive(n,"children");
    if(!cJSON_IsString(tag)||!cJSON_IsString(text)||!cJSON_IsObject(a)||!cJSON_IsArray(children))return -1;
    int actual=dom_add(dom,parent,tag->valuestring,*text->valuestring?text->valuestring:NULL);if(actual<0)return -1;
    dom->nodes[actual].source_id=index;
    const cJSON *item;
    cJSON_ArrayForEach(item,a)if(!cJSON_IsString(item)||!item->string||dom_set_attr(dom,actual,item->string,item->valuestring)<0)return -1;
    cJSON_ArrayForEach(item,children)if(!cJSON_IsNumber(item)||item->valuedouble!=(double)item->valueint||import_node(dom,items,count,item->valueint,actual,depth+1,seen)<0)return -1;
    return 0;
}
int dom_from_json(browser_dom *dom,const char *json,size_t length)
{
    memset(dom,0,sizeof(*dom));if(length>2*DOM_BYTES_MAX)return -1;
    json_begin();cJSON *array=cJSON_ParseWithLength(json,length+1);
    if(!array||!cJSON_IsArray(array)||cJSON_GetArraySize(array)<1||cJSON_GetArraySize(array)>DOM_NODES_MAX){cJSON_Delete(array);json_end();return -1;}
    dom->html=1;dom->capacity=cJSON_GetArraySize(array);dom->nodes=calloc((size_t)dom->capacity,sizeof(*dom->nodes));
    unsigned char *seen=calloc((size_t)dom->capacity,1);const cJSON **items=malloc((size_t)dom->capacity*sizeof(*items));
    int result=-1,count=0;const cJSON *item;
    if(dom->nodes&&seen&&items){cJSON_ArrayForEach(item,array)items[count++]=item;result=import_node(dom,items,count,0,-1,0,seen);}
    free(items);free(seen);cJSON_Delete(array);json_end();if(result<0)dom_free(dom);return result;
}

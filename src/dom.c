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
/* Raw script/style text may contain '<'. Only its matching end tag ends it. */
static char *raw(reader *r, const char *tag, size_t maximum, int *shortened)
{
    char *out=malloc(maximum+1);if(!out)return NULL;
    size_t used=0;int c;
    while((c=get(r))>=0) {
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
            for(size_t i=0;i<n;i++)if(used<maximum)out[used++]=candidate[i];else *shortened=1;
        } else if(used<maximum)out[used++]=(char)c;else *shortened=1;
    }
    out[used]=0;return out;
}
int dom_parse(browser_dom *dom, dom_read_fn read, void *ud, int html, int latin,
              dom_cancel_fn cancel, void *cancel_ud, char *err, size_t errlen)
{
    memset(dom,0,sizeof(*dom));dom->html=html;
    dom->nodes=calloc(DOM_NODES_MAX,sizeof(*dom->nodes));
    if(!dom->nodes){snprintf(err,errlen,"Not enough memory for the page.");return -1;}
    dom_add(dom,-1,"#document",NULL);
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
        size_t n=0;char quote=0;int oversized=0;
        while((c=get(&r))>=0) {
            if(quote){if(c==quote)quote=0;}else if(c=='\''||c=='"')quote=(char)c;else if(c=='>')break;
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
        if((eq(tag,"p")||eq(tag,"li")||eq(tag,"tr")||eq(tag,"td")||eq(tag,"option"))&&eq(dom->nodes[stack[depth-1]].tag,tag)&&depth>1)depth--;
        int index=dom_add(dom,stack[depth-1],tag,NULL);
        if(index<0){dom->shortened=1;break;}
        if(!oversized)attributes(dom,index,p);
        if(eq(tag,"script")||eq(tag,"style")) {
            int cut=0;char *text=raw(&r,tag,eq(tag,"script")?512*1024:64*1024,&cut);
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
char *dom_json(const browser_dom *dom)
{
    json_begin();cJSON *array=cJSON_CreateArray();char *result=NULL;
    if(!array)goto end;
    for(int i=0;i<dom->count;i++) {
        const dom_node *n=&dom->nodes[i];cJSON *o=cJSON_CreateObject();
        if(!o)goto end;
        if(!cJSON_AddItemToArray(array,o)){cJSON_Delete(o);goto end;}
        if(!cJSON_AddStringToObject(o,"tag",n->tag)||!cJSON_AddStringToObject(o,"text",eq(n->tag,"script")?"":n->text?n->text:""))goto end;
        cJSON *a=cJSON_AddObjectToObject(o,"attrs"),*children=cJSON_AddArrayToObject(o,"children");
        if(!a||!children)goto end;
        for(int j=0;j<n->attribute_count;j++)if(!cJSON_AddStringToObject(a,n->attributes[j].name,n->attributes[j].value))goto end;
        for(int j=n->first;j>=0;j=dom->nodes[j].next){cJSON *number=cJSON_CreateNumber(j);if(!number)goto end;if(!cJSON_AddItemToArray(children,number)){cJSON_Delete(number);goto end;}}
    }
    char *json=cJSON_PrintUnformatted(array);
    if(json){size_t n=strlen(json)+1;result=malloc(n);if(result)memcpy(result,json,n);json_release(json);}
end:cJSON_Delete(array);json_end();return result;
}
static int import_node(browser_dom *dom,const cJSON *array,int index,int parent,int depth,unsigned char *seen)
{
    if(index<0||index>=cJSON_GetArraySize(array)||seen[index]||depth>=DOM_DEPTH_MAX)return -1;
    seen[index]=1;const cJSON *n=cJSON_GetArrayItem(array,index),*tag=cJSON_GetObjectItemCaseSensitive(n,"tag"),*text=cJSON_GetObjectItemCaseSensitive(n,"text"),*a=cJSON_GetObjectItemCaseSensitive(n,"attrs"),*children=cJSON_GetObjectItemCaseSensitive(n,"children");
    if(!cJSON_IsString(tag)||!cJSON_IsString(text)||!cJSON_IsObject(a)||!cJSON_IsArray(children))return -1;
    int actual=dom_add(dom,parent,tag->valuestring,*text->valuestring?text->valuestring:NULL);if(actual<0)return -1;
    dom->nodes[actual].source_id=index;
    const cJSON *item;
    cJSON_ArrayForEach(item,a)if(!cJSON_IsString(item)||!item->string||dom_set_attr(dom,actual,item->string,item->valuestring)<0)return -1;
    cJSON_ArrayForEach(item,children)if(!cJSON_IsNumber(item)||item->valuedouble!=(double)item->valueint||import_node(dom,array,item->valueint,actual,depth+1,seen)<0)return -1;
    return 0;
}
int dom_from_json(browser_dom *dom,const char *json,size_t length)
{
    memset(dom,0,sizeof(*dom));if(length>2*DOM_BYTES_MAX)return -1;
    json_begin();cJSON *array=cJSON_ParseWithLength(json,length+1);
    if(!array||!cJSON_IsArray(array)||cJSON_GetArraySize(array)<1||cJSON_GetArraySize(array)>DOM_NODES_MAX){cJSON_Delete(array);json_end();return -1;}
    dom->html=1;dom->nodes=calloc(DOM_NODES_MAX,sizeof(*dom->nodes));unsigned char seen[DOM_NODES_MAX]={0};
    int result=dom->nodes?import_node(dom,array,0,-1,0,seen):-1;
    cJSON_Delete(array);json_end();if(result<0)dom_free(dom);return result;
}

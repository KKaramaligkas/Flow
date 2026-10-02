#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "document.h"

typedef struct { browser_document *doc; size_t used, capacity; int html, pre, latin; browser_style style; } writer;

static int equal(const char *a, const char *b)
{
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
static int starts(const char *a, const char *b)
{
    while (*b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return 1;
}
static const char *find_case(const char *text, const char *wanted)
{
    for (; *text; text++) if (starts(text, wanted)) return text;
    return NULL;
}
static size_t utf8(uint32_t c, char bytes[4])
{
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || !c) c = 0xfffd;
    if (c < 0x80) { bytes[0] = (char)c; return 1; }
    if (c < 0x800) { bytes[0] = 0xc0 | (c >> 6); bytes[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { bytes[0] = 0xe0 | (c >> 12); bytes[1] = 0x80 | ((c >> 6) & 63); bytes[2] = 0x80 | (c & 63); return 3; }
    bytes[0] = 0xf0 | (c >> 18); bytes[1] = 0x80 | ((c >> 12) & 63);
    bytes[2] = 0x80 | ((c >> 6) & 63); bytes[3] = 0x80 | (c & 63); return 4;
}
static size_t character(const char *p, size_t remaining, int latin, uint32_t *value)
{
    const unsigned char *s = (const unsigned char *)p;
    static const uint16_t windows[32] = {
        0x20ac,0xfffd,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0xfffd,0x017d,0xfffd,
        0xfffd,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x02dc,0x2122,0x0161,0x203a,0x0153,0xfffd,0x017e,0x0178};
    if (*s < 128 || latin) { *value = latin && *s >= 128 && *s < 160 ? windows[*s - 128] : *s; return 1; }
    size_t n = *s >= 0xc2 && *s <= 0xdf ? 2 : *s >= 0xe0 && *s <= 0xef ? 3 : *s >= 0xf0 && *s <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) { *value = 0xfffd; return 1; }
    uint32_t c = *s & (n == 2 ? 31 : n == 3 ? 15 : 7);
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *value = 0xfffd; return 1; }
        c = (c << 6) | (s[i] & 63);
    }
    if (c < (n == 2 ? 128u : n == 3 ? 2048u : 65536u) || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) { *value = 0xfffd; return 1; }
    *value = c; return n;
}
static size_t entity(const char *p, size_t remaining, uint32_t *value)
{
    if (*p != '&') return 0;
    size_t n = 1;
    while (n < remaining && n < 32 && p[n] != ';' && !isspace((unsigned char)p[n]) && p[n] != '<') n++;
    if (n >= remaining || p[n] != ';') return 0;
    char name[32]; memcpy(name, p + 1, n - 1); name[n - 1] = 0;
    static const struct { const char *name; uint32_t code; } names[] = {
        {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",32},
        {"copy",0xa9},{"reg",0xae},{"hellip",0x2026},{"ndash",0x2013},{"mdash",0x2014},{"euro",0x20ac}};
    for (size_t i = 0; i < sizeof(names)/sizeof(*names); i++) if (!strcmp(name, names[i].name)) { *value = names[i].code; return n + 1; }
    if (name[0] == '#') {
        const char *digits = name + 1; unsigned base = 10;
        if (*digits == 'x' || *digits == 'X') { digits++; base = 16; }
        if (!*digits) return 0;
        uint32_t code = 0;
        for (; *digits; digits++) {
            int digit = isdigit((unsigned char)*digits) ? *digits - '0' :
                tolower((unsigned char)*digits) >= 'a' && tolower((unsigned char)*digits) <= 'f' ? tolower((unsigned char)*digits) - 'a' + 10 : -1;
            if (digit < 0 || (unsigned)digit >= base || code > (0x10ffffu - digit) / base) { *value = 0xfffd; return n + 1; }
            code = code * base + digit;
        }
        *value = code; return n + 1;
    }
    return 0;
}
static void emit(writer *w, uint32_t c)
{
    if (c == 0xa0 || c == '\r' || c == '\t' || (c < 32 && c != '\n') || c == 127) c = ' ';
    if (w->html && !w->pre && (c == ' ' || c == '\n')) {
        if (!w->used || w->doc->text[w->used - 1] == ' ' || w->doc->text[w->used - 1] == '\n') return;
        c = ' ';
    }
    char bytes[4]; size_t n = utf8(c, bytes);
    if (w->used + n >= (w->capacity ? w->capacity : BROWSER_TEXT_MAX - 64)) { w->doc->shortened = 1; return; }
    if (w->doc->spans && (w->doc->span_count == 0 || memcmp(&w->doc->spans[w->doc->span_count-1].style, &w->style, sizeof(w->style)))) {
        if (w->doc->span_count < CSS_SPANS_MAX) w->doc->spans[w->doc->span_count++] = (browser_span){w->used,w->style};
        else w->doc->css_omitted=1;
    }
    memcpy(w->doc->text + w->used, bytes, n); w->used += n; w->doc->text[w->used] = 0;
}
static void line(writer *w)
{
    while (w->used && w->doc->text[w->used - 1] == ' ') w->used--;
    if (w->used && w->doc->text[w->used - 1] != '\n' && w->used < BROWSER_TEXT_MAX - 64)
        w->doc->text[w->used++] = '\n';
    w->doc->text[w->used] = 0;
}
static void decoded(writer *w, const char *text, size_t length)
{
    for (size_t pos = 0; pos < length && !w->doc->shortened;) {
        uint32_t c; size_t n = entity(text + pos, length - pos, &c);
        if (!n) n = character(text + pos, length - pos, w->latin, &c);
        emit(w, c); pos += n;
    }
}

char *browser_decode(const char *data,size_t length,int entities,int latin)
{
    if(length>DOM_SOURCE_MAX||length>(SIZE_MAX-1)/3)return NULL;
    char *out=malloc(length*3+1);if(!out)return NULL;size_t used=0;
    for(size_t pos=0;pos<length;){uint32_t c;size_t n=entities?entity(data+pos,length-pos,&c):0;
        if(!n)n=character(data+pos,length-pos,latin,&c);
        char bytes[4];size_t count=utf8(c,bytes);memcpy(out+used,bytes,count);used+=count;pos+=n;
    }out[used]=0;return out;
}
static void title_text(const browser_dom *dom,int index,char *out,size_t capacity)
{
    const dom_node *node=&dom->nodes[index];size_t used=strlen(out);
    if(node->text&&used<capacity-1){size_t n=strlen(node->text);if(n>capacity-used-1)n=capacity-used-1;
        while(n&&((unsigned char)node->text[n]&0xc0)==0x80)n--;
        memcpy(out+used,node->text,n);out[used+n]=0;
    }
    for(int i=node->first;i>=0;i=dom->nodes[i].next)title_text(dom,i,out,capacity);
}
static void marker(writer *w,int number){char value[24];snprintf(value,sizeof(value)," [%d]",number);decoded(w,value,strlen(value));}
static void render_node(writer *w,int index,browser_style parent,const char *base,int depth)
{
    browser_document *doc=w->doc;const browser_dom *dom=doc->dom;const dom_node *n=&dom->nodes[index];
    if(depth>=DOM_DEPTH_MAX)return;
    browser_style style=css_compute(&doc->css,dom,index,parent);
    if(style.hidden||equal(n->tag,"head")||equal(n->tag,"script")||equal(n->tag,"style")||equal(n->tag,"template")||equal(n->tag,"title")||equal(n->tag,"link"))return;
    if(equal(n->tag,"body")&&style.background)doc->paper=style.background;
    w->style=style;w->pre=style.pre;
    if(style.block)line(w);
    const char *id=dom_attr(n,"id");if(!*id&&equal(n->tag,"a"))id=dom_attr(n,"name");
    if(*id&&strlen(id)<sizeof(doc->anchors[0].id)&&doc->anchor_count<BROWSER_ANCHORS_MAX){browser_anchor *a=&doc->anchors[doc->anchor_count++];strcpy(a->id,id);a->offset=w->used;}
    int active=-1;
    if(equal(n->tag,"a")) {
        char target[BROWSER_URL_MAX];const char *href=dom_attr(n,"href");
        if(*href&&browser_url_resolve(base,href,target,sizeof(target))==0){
            if(doc->count==BROWSER_LINKS_MAX)doc->links_omitted=1;
            else{active=doc->count++;browser_link *l=&doc->links[active];strcpy(l->url,target);l->offset=w->used;l->node=n->source_id;}
        }
    }
    if(equal(n->tag,"li")){emit(w,'*');emit(w,' ');}
    if(equal(n->tag,"td")||equal(n->tag,"th"))emit(w,' ');
    if(equal(n->tag,"img")){const char *alt=dom_attr(n,"alt");for(size_t p=0,len=strlen(alt);p<len;){uint32_t c;size_t n=character(alt+p,len-p,0,&c);emit(w,c);p+=n;}}
    if(n->text&&strcmp(n->tag,"#text")==0){
        for(size_t pos=0,length=strlen(n->text);pos<length&&!doc->shortened;){uint32_t c;size_t count=character(n->text+pos,length-pos,0,&c);emit(w,c);pos+=count;}
    }
    for(int i=n->first;i>=0&&!doc->shortened;i=dom->nodes[i].next)render_node(w,i,style,base,depth+1);
    w->style=style;w->pre=style.pre;
    if(active>=0){browser_link *l=&doc->links[active];size_t length=w->used-l->offset;if(length>=sizeof(l->label))length=sizeof(l->label)-1;
        while(length&&((unsigned char)doc->text[l->offset+length]&0xc0)==0x80)length--;
        memcpy(l->label,doc->text+l->offset,length);l->label[length]=0;
        for(char *p=l->label;*p;p++)if(*p=='\n')*p=' ';
        if(!length)snprintf(l->label,sizeof(l->label),"Link %d",active+1);marker(w,active+1);
    }
    if(style.block)line(w);
}
int browser_document_render(browser_document *doc,char *err,size_t errlen)
{
    char *text=calloc(1,BROWSER_TEXT_MAX);browser_span *spans=calloc(CSS_SPANS_MAX,sizeof(*spans));
    if(!text||!spans){free(text);free(spans);snprintf(err,errlen,"Not enough memory to display the page.");return -1;}
    free(doc->text);free(doc->spans);css_free(&doc->css);doc->text=text;doc->spans=spans;
    doc->count=doc->anchor_count=doc->span_count=doc->links_omitted=doc->css_omitted=0;
    int previously_shortened=doc->dom->shortened;doc->shortened=0;
    doc->paper=0xff1f140c;snprintf(doc->title,sizeof(doc->title),"Web page");
    char base[BROWSER_URL_MAX];strcpy(base,doc->url);int base_set=0;
    for(int i=0;i<doc->dom->count;i++){
        const dom_node *n=&doc->dom->nodes[i];
        if(equal(n->tag,"title")){doc->title[0]=0;title_text(doc->dom,i,doc->title,sizeof(doc->title));}
        if(equal(n->tag,"base")&&!base_set){char resolved[BROWSER_URL_MAX];if(*dom_attr(n,"href")&&browser_url_resolve(base,dom_attr(n,"href"),resolved,sizeof(resolved))==0){strcpy(base,resolved);base_set=1;}}
        if((equal(n->tag,"style")||equal(n->tag,"link")&&equal(dom_attr(n,"rel"),"stylesheet"))&&n->text)css_add(&doc->css,n->text,strlen(n->text));
    }
    browser_style initial={.color=0xfff2e8e0,.scale=0.64f,.pre=!doc->dom->html};
    writer w={.doc=doc,.html=doc->dom->html,.style=initial,.pre=initial.pre};
    render_node(&w,0,initial,base,0);
    doc->shortened|=previously_shortened;doc->css_omitted|=doc->css.omitted;
    if(doc->shortened){const char *notice="\n[Page shortened to fit PSP memory.]\n";memcpy(doc->text+w.used,notice,strlen(notice)+1);}
    if(!*doc->text)strcpy(doc->text,"This page has no readable text. Try enabling JavaScript or downloading the file.");
    return 0;
}
browser_style browser_style_at(const browser_document *doc,size_t offset)
{
    browser_style style={.color=0xfff2e8e0,.scale=0.64f};
    int low=0,high=doc->span_count;while(low<high){int mid=low+(high-low)/2;if(doc->spans[mid].offset<=offset)low=mid+1;else high=mid;}
    if(low)style=doc->spans[low-1].style;
    return style;
}
void browser_script_free(browser_document *);
void browser_document_free(browser_document *doc)
{
    browser_script_free(doc);free(doc->text);free(doc->spans);css_free(&doc->css);
    if(doc->dom){dom_free(doc->dom);free(doc->dom);}memset(doc,0,sizeof(*doc));
}
int browser_document_read(browser_document *doc,dom_read_fn read,void *ud,const char *url,const char *type,
                          dom_cancel_fn cancel,void *cancel_ud,char *err,size_t errlen)
{
    memset(doc,0,sizeof(*doc));
    if(!type)type="";
    int html=starts(type,"text/html")||starts(type,"application/xhtml+xml")||!*type;
    if(browser_url_resolve(NULL,url,doc->url,sizeof(doc->url))<0||!html&&!starts(type,"text/")&&!starts(type,"application/json")){snprintf(err,errlen,"This is a file. Select Download to save it.");return -1;}
    doc->dom=calloc(1,sizeof(*doc->dom));if(!doc->dom){snprintf(err,errlen,"Not enough memory for the page.");return -1;}
    int latin=find_case(type,"iso-8859-1")||find_case(type,"windows-1252");
    if(dom_parse(doc->dom,read,ud,html,latin,cancel,cancel_ud,err,errlen)<0){browser_document_free(doc);return -1;}
    doc->source_bytes=doc->dom->source_bytes;
    int result=browser_document_render(doc,err,errlen);
    if(result<0)browser_document_free(doc);
    return result;
}
typedef struct{const char *data;size_t length,pos;} memory_reader;
static int memory_read(void *ud,char *out,size_t size)
{
    memory_reader *r=ud;size_t n=r->length-r->pos;if(n>size)n=size;memcpy(out,r->data+r->pos,n);r->pos+=n;return (int)n;
}
int browser_document_parse(browser_document *doc,const char *data,size_t length,const char *url,const char *type,char *err,size_t errlen)
{
    memset(doc,0,sizeof(*doc));
    if(!data||length>BROWSER_PAGE_MAX||memchr(data,0,length)){snprintf(err,errlen,"Invalid or oversized page.");return -1;}
    memory_reader r={data,length,0};
    if(!type||!*type){size_t i=0;while(i<length&&isspace((unsigned char)data[i]))i++;type=i<length&&data[i]=='<'?"text/html":"text/plain";}
    return browser_document_read(doc,memory_read,&r,url,type,NULL,NULL,err,errlen);
}

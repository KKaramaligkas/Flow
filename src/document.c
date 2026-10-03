#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "document.h"
#include "script.h"
#include "view.h"

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
/* Named character references: all of HTML 4's (Latin-1, punctuation,
   symbols, Greek, arrows, maths) and HTML 5's common ones. */
static const char *const latin1_names[96] = {
    "nbsp","iexcl","cent","pound","curren","yen","brvbar","sect","uml","copy","ordf","laquo","not","shy","reg","macr",
    "deg","plusmn","sup2","sup3","acute","micro","para","middot","cedil","sup1","ordm","raquo","frac14","frac12","frac34","iquest",
    "Agrave","Aacute","Acirc","Atilde","Auml","Aring","AElig","Ccedil","Egrave","Eacute","Ecirc","Euml","Igrave","Iacute","Icirc","Iuml",
    "ETH","Ntilde","Ograve","Oacute","Ocirc","Otilde","Ouml","times","Oslash","Ugrave","Uacute","Ucirc","Uuml","Yacute","THORN","szlig",
    "agrave","aacute","acirc","atilde","auml","aring","aelig","ccedil","egrave","eacute","ecirc","euml","igrave","iacute","icirc","iuml",
    "eth","ntilde","ograve","oacute","ocirc","otilde","ouml","divide","oslash","ugrave","uacute","ucirc","uuml","yacute","thorn","yuml"};
static const char *const greek_names[25] = {    /* U+0391 to U+03A9, and in lower case from U+03B1 */
    "alpha","beta","gamma","delta","epsilon","zeta","eta","theta","iota","kappa","lambda","mu","nu","xi","omicron","pi",
    "rho","sigmaf","sigma","tau","upsilon","phi","chi","psi","omega"};
static const struct { const char *name; uint32_t code; } entity_names[] = {
    {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",32},
    {"ndash",0x2013},{"mdash",0x2014},{"lsquo",0x2018},{"rsquo",0x2019},{"sbquo",0x201a},{"ldquo",0x201c},{"rdquo",0x201d},
    {"bdquo",0x201e},{"hellip",0x2026},{"bull",0x2022},{"lsaquo",0x2039},{"rsaquo",0x203a},{"euro",0x20ac},{"trade",0x2122},
    {"ensp",0x2002},{"emsp",0x2003},{"thinsp",0x2009},{"zwnj",0x200c},{"zwj",0x200d},{"lrm",0x200e},{"rlm",0x200f},
    {"dagger",0x2020},{"Dagger",0x2021},{"permil",0x2030},{"prime",0x2032},{"Prime",0x2033},{"oline",0x203e},{"frasl",0x2044},
    {"OElig",0x152},{"oelig",0x153},{"Scaron",0x160},{"scaron",0x161},{"Yuml",0x178},{"fnof",0x192},{"circ",0x2c6},{"tilde",0x2dc},
    {"thetasym",0x3d1},{"upsih",0x3d2},{"piv",0x3d6},{"image",0x2111},{"weierp",0x2118},{"real",0x211c},{"alefsym",0x2135},
    {"larr",0x2190},{"uarr",0x2191},{"rarr",0x2192},{"darr",0x2193},{"harr",0x2194},{"crarr",0x21b5},
    {"lArr",0x21d0},{"uArr",0x21d1},{"rArr",0x21d2},{"dArr",0x21d3},{"hArr",0x21d4},
    {"forall",0x2200},{"part",0x2202},{"exist",0x2203},{"empty",0x2205},{"nabla",0x2207},{"isin",0x2208},{"notin",0x2209},
    {"ni",0x220b},{"prod",0x220f},{"sum",0x2211},{"minus",0x2212},{"lowast",0x2217},{"radic",0x221a},{"prop",0x221d},
    {"infin",0x221e},{"ang",0x2220},{"and",0x2227},{"or",0x2228},{"cap",0x2229},{"cup",0x222a},{"int",0x222b},{"there4",0x2234},
    {"sim",0x223c},{"cong",0x2245},{"asymp",0x2248},{"ne",0x2260},{"equiv",0x2261},{"le",0x2264},{"ge",0x2265},{"sub",0x2282},
    {"sup",0x2283},{"nsub",0x2284},{"sube",0x2286},{"supe",0x2287},{"oplus",0x2295},{"otimes",0x2297},{"perp",0x22a5},
    {"sdot",0x22c5},{"lceil",0x2308},{"rceil",0x2309},{"lfloor",0x230a},{"rfloor",0x230b},{"lang",0x27e8},{"rang",0x27e9},
    {"loz",0x25ca},{"spades",0x2660},{"clubs",0x2663},{"hearts",0x2665},{"diams",0x2666},
    {"check",0x2713},{"cross",0x2717},{"star",0x2606},{"starf",0x2605},{"phone",0x260e},{"hyphen",0x2010},{"dash",0x2010},
    {"horbar",0x2015},{"centerdot",0xb7},{"half",0xbd},{"Tab",9},{"NewLine",10},{"excl",33},{"num",35},{"dollar",36},
    {"percnt",37},{"lpar",40},{"rpar",41},{"ast",42},{"plus",43},{"comma",44},{"period",46},{"sol",47},{"colon",58},
    {"semi",59},{"equals",61},{"quest",63},{"commat",64},{"lsqb",91},{"lbrack",91},{"bsol",92},{"rsqb",93},{"rbrack",93},
    {"Hat",94},{"lowbar",95},{"grave",96},{"lcub",123},{"lbrace",123},{"verbar",124},{"vert",124},{"rcub",125},{"rbrace",125}};
static size_t entity(const char *p, size_t remaining, uint32_t *value)
{
    if (*p != '&') return 0;
    size_t n = 1;
    while (n < remaining && n < 32 && p[n] != ';' && !isspace((unsigned char)p[n]) && p[n] != '<') n++;
    if (n >= remaining || p[n] != ';') return 0;
    char name[32]; memcpy(name, p + 1, n - 1); name[n - 1] = 0;
    for (size_t i = 0; i < sizeof(entity_names)/sizeof(*entity_names); i++) if (!strcmp(name, entity_names[i].name)) { *value = entity_names[i].code; return n + 1; }
    for (size_t i = 0; i < 96; i++) if (!strcmp(name, latin1_names[i])) { *value = 0xa0 + (uint32_t)i; return n + 1; }
    for (size_t i = 0; i < 25; i++) {
        if (!strcmp(name, greek_names[i])) { *value = 0x3b1 + (uint32_t)i; return n + 1; }
        /* Capital letters: the same names capitalised, with no final sigma. */
        if (i != 17 && name[0] == toupper((unsigned char)greek_names[i][0]) && !strcmp(name + 1, greek_names[i] + 1)) { *value = 0x391 + (uint32_t)i; return n + 1; }
    }
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
/* Soft hyphens, zero-width spaces and joiners, direction marks and byte
   order marks take no room and have no glyph. */
static int invisible(uint32_t c)
{
    return c == 0xad || (c >= 0x200b && c <= 0x200f) || c == 0x2060 || c == 0xfeff;
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
    while(w->doc->span_count&&w->doc->spans[w->doc->span_count-1].offset>=w->used)w->doc->span_count--;
    if (w->used && w->doc->text[w->used - 1] != '\n' && w->used < BROWSER_TEXT_MAX - 64)
        w->doc->text[w->used++] = '\n';
    w->doc->text[w->used] = 0;
}
static void decoded(writer *w, const char *text, size_t length)
{
    for (size_t pos = 0; pos < length && !w->doc->shortened;) {
        uint32_t c; size_t n = entity(text + pos, length - pos, &c);
        if (!n) n = character(text + pos, length - pos, w->latin, &c);
        if (!invisible(c)) emit(w, c);
        pos += n;
    }
}

char *browser_decode(const char *data,size_t length,int entities,int latin)
{
    if(length>DOM_SOURCE_MAX||length>(SIZE_MAX-1)/3)return NULL;
    char *out=malloc(length*3+1);if(!out)return NULL;size_t used=0;
    for(size_t pos=0;pos<length;){uint32_t c;size_t n=entities?entity(data+pos,length-pos,&c):0;
        if(!n)n=character(data+pos,length-pos,latin,&c);
        pos+=n;if(invisible(c))continue;
        char bytes[4];size_t count=utf8(c,bytes);memcpy(out+used,bytes,count);used+=count;
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
    if(doc->scripting&&equal(n->tag,"noscript"))return;
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
        if(!length)snprintf(l->label,sizeof(l->label),"Link %d",active+1);
        marker(w,active+1);
    }
    if(style.block)line(w);
}
/* A word of a space-separated attribute ("rel"), any case. */
static int has_word(const char *list,const char *name)
{
    size_t n=strlen(name);
    for(const char *p=list;*p;){
        while(isspace((unsigned char)*p))p++;
        const char *end=p;while(*end&&!isspace((unsigned char)*end))end++;
        if((size_t)(end-p)==n){size_t i=0;while(i<n&&tolower((unsigned char)p[i])==tolower((unsigned char)name[i]))i++;if(i==n)return 1;}
        p=end;
    }
    return 0;
}
/* <style> and <link rel=stylesheet> for media that the page view has. A
   preloaded stylesheet counts too: pages switch it on with a script. */
static int stylesheet(const dom_node *n)
{
    const char *rel=dom_attr(n,"rel");
    int sheet=equal(n->tag,"style")||(equal(n->tag,"link")&&((has_word(rel,"stylesheet")&&!has_word(rel,"alternate"))||
              (has_word(rel,"preload")&&equal(dom_attr(n,"as"),"style"))));
    return sheet&&css_media_matches(dom_attr(n,"media"));
}
static int inside(const browser_dom *dom,int index,const char *tag)
{
    for(int i=dom->nodes[index].parent,depth=0;i>=0&&depth<DOM_DEPTH_MAX;i=dom->nodes[i].parent,depth++)if(equal(dom->nodes[i].tag,tag))return 1;
    return 0;
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
        if(stylesheet(n)&&n->text&&!(doc->scripting&&inside(doc->dom,i,"noscript"))){doc->css.dom=doc->dom;css_add(&doc->css,n->text,strlen(n->text));}
    }
    browser_style initial={.color=0xfff2e8e0,.link=0xffe6bc52,.scale=0.64f,.pre=!doc->dom->html};
    writer w={.doc=doc,.html=doc->dom->html,.style=initial,.pre=initial.pre};
    render_node(&w,0,initial,base,0);
    doc->shortened|=previously_shortened;doc->css_omitted|=doc->css.omitted;
    if(doc->shortened){const char *notice="\n[Page shortened to fit PSP memory.]\n";memcpy(doc->text+w.used,notice,strlen(notice)+1);}
    if(!*doc->text)strcpy(doc->text,"This page has no readable text. Try enabling JavaScript or downloading the file.");
    doc->rendered=doc->dom;
    return 0;
}
browser_style browser_style_at(const browser_document *doc,size_t offset)
{
    browser_style style={.color=0xfff2e8e0,.link=0xffe6bc52,.scale=0.64f};
    int low=0,high=doc->span_count;while(low<high){int mid=low+(high-low)/2;if(doc->spans[mid].offset<=offset)low=mid+1;else high=mid;}
    if(low)style=doc->spans[low-1].style;
    return style;
}
void browser_script_free(browser_document *);
void browser_document_free(browser_document *doc)
{
    if(doc->view){browser_view_free(doc->view);free(doc->view);}
    browser_script_free(doc);free(doc->text);free(doc->spans);css_free(&doc->css);
    if(doc->dom){dom_free(doc->dom);free(doc->dom);}memset(doc,0,sizeof(*doc));
}
int browser_document_read(browser_document *doc,dom_read_fn read,void *ud,const char *url,const char *type,
                          dom_cancel_fn cancel,void *cancel_ud,char *err,size_t errlen)
{
    if(browser_document_load(doc,read,ud,url,type,cancel,cancel_ud,err,errlen)<0)return -1;
    int result=browser_document_render(doc,err,errlen);
    if(result<0)browser_document_free(doc);
    return result;
}
int browser_document_load(browser_document *doc,dom_read_fn read,void *ud,const char *url,const char *type,
                          dom_cancel_fn cancel,void *cancel_ud,char *err,size_t errlen)
{
    memset(doc,0,sizeof(*doc));
    if(!type)type="";
    int html=starts(type,"text/html")||starts(type,"application/xhtml+xml")||!*type;
    if(browser_url_resolve(NULL,url,doc->url,sizeof(doc->url))<0||(!html&&!starts(type,"text/")&&!starts(type,"application/json"))){snprintf(err,errlen,"This is a file. Select Download to save it.");return -1;}
    doc->dom=calloc(1,sizeof(*doc->dom));if(!doc->dom){snprintf(err,errlen,"Not enough memory for the page.");return -1;}
    int latin=find_case(type,"iso-8859-1")||find_case(type,"windows-1252");
    if(dom_parse(doc->dom,read,ud,html,latin,cancel,cancel_ud,err,errlen)<0){browser_document_free(doc);return -1;}
    doc->source_bytes=doc->dom->source_bytes;
    return 0;
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
void browser_assets(browser_document *doc,int javascript,browser_asset_fn fetch,void *ud,dom_cancel_fn cancel,void *cancel_ud)
{
    int requests=0; size_t script_bytes=doc->dom->script_bytes, css_bytes=0;
    char base[BROWSER_URL_MAX]; strcpy(base,doc->url);
    for(int i=0;i<doc->dom->count;i++) if(!strcmp(doc->dom->nodes[i].tag,"base")) {
        char resolved[BROWSER_URL_MAX];
        if(browser_url_resolve(base,dom_attr(&doc->dom->nodes[i],"href"),resolved,sizeof(resolved))==0) strcpy(base,resolved);
        break;
    }
    size_t downloaded=0;
    for(int i=0;i<doc->dom->count&&!(cancel&&cancel(cancel_ud));i++) {
        dom_node *n=&doc->dom->nodes[i]; int script=!strcmp(n->tag,"script");
        int css=!script&&stylesheet(n);
        if(!script&&!css)continue;
        if(css&&!strcmp(n->tag,"style")) {
            /* Inline styles too keep only what can match. */
            if(!n->text)continue;
            size_t before=strlen(n->text)+1,length=0;
            char *compact=css_compact(n->text,before-1,doc->dom,&length);
            if(!compact)continue;
            free(n->text);n->text=compact;
            doc->dom->bytes=doc->dom->bytes-before+length+1;css_bytes+=length;
            continue;
        }
        const char *ref=dom_attr(n,script?"src":"href"); if(!*ref||(script&&!javascript))continue;
        if(css&&javascript&&inside(doc->dom,i,"noscript"))continue;     /* not shown with JavaScript on */
        char target[BROWSER_URL_MAX],final[BROWSER_URL_MAX]; int limit=script?512*1024:CSS_SHEET_MAX;
        size_t left=script?1024*1024-script_bytes:css_bytes<CSS_TEXT_MAX&&downloaded<2*CSS_SHEET_MAX?2*CSS_SHEET_MAX-downloaded:0;
        if(requests++>=16||!left||browser_url_resolve(base,ref,target,sizeof(target))<0||
           (browser_url_secure(doc->url)&&!browser_url_secure(target))||(script&&!browser_same_origin(doc->url,target))) { doc->assets_omitted++; continue; }
        if((size_t)limit>left)limit=(int)left;
        int length=0; final[0]=0;
        char *data=fetch(ud,target,limit,&length,final);
        if(data&&((browser_url_secure(doc->url)&&!browser_url_secure(final))||(script&&!browser_same_origin(doc->url,final)))) { free(data); data=NULL; }
        if(!data||memchr(data,0,(size_t)length)) {free(data);doc->assets_omitted++;continue;}
        if(css) {
            /* A stylesheet is downloaded whole, then reduced to the rules
               that can match this page: the rest of a framework's CSS isn't kept. */
            downloaded+=(size_t)length;
            size_t kept=0;char *compact=css_compact(data,(size_t)length,doc->dom,&kept);
            free(data);data=compact;length=(int)kept;
            if(!data||(size_t)length+1>DOM_BYTES_MAX-doc->dom->bytes) {free(data);doc->assets_omitted++;continue;}
        }
        if(script)script_bytes+=(size_t)length;else{css_bytes+=(size_t)length;doc->dom->bytes+=(size_t)length+1;}
        free(n->text); n->text=data;
        if(script)dom_set_attr(doc->dom,i,"src",final);
    }
}

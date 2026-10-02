#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"
static int eq(const char *a,const char *b){while(*a&&*b)if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0;return !*a&&!*b;}
static char *trim(char *s){while(isspace((unsigned char)*s))s++;size_t n=strlen(s);while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;return s;}
static int word(const char *list,const char *name){size_t n=strlen(name);for(const char *p=list;*p;){while(isspace((unsigned char)*p))p++;const char *end=p;while(*end&&!isspace((unsigned char)*end))end++;if((size_t)(end-p)==n&&!memcmp(p,name,n))return 1;p=end;}return 0;}
static int simple(const dom_node *n,const char *selector)
{
    const char *p=selector;char token[96];
    if(*p=='*')p++;else if(isalpha((unsigned char)*p)){
        size_t k=0;while(isalnum((unsigned char)*p)||*p=='-'||*p=='_'){if(k<sizeof(token)-1)token[k++]=*p;p++;}token[k]=0;if(!eq(n->tag,token))return 0;
    }
    while(*p){char type=*p++;size_t k=0;while(isalnum((unsigned char)*p)||*p=='-'||*p=='_'){if(k<sizeof(token)-1)token[k++]=*p;p++;}token[k]=0;
        if(type=='#'){if(strcmp(dom_attr(n,"id"),token))return 0;}else if(type=='.'){if(!word(dom_attr(n,"class"),token))return 0;}else if(type==':'&&eq(token,"root")){if(!eq(n->tag,"html")&&strcmp(n->tag,"#document"))return 0;}else return 0;
    }return 1;
}
int css_matches(const browser_dom *dom,int index,const char *selector)
{
    if(index<0||index>=dom->count||strlen(selector)>=192)return 0;
    char copy[192];strcpy(copy,selector);char *s=trim(copy),*end=s+strlen(s);
    while(end>s){char *start=end;while(start>s&&!isspace((unsigned char)start[-1])&&start[-1]!='>')start--;
        char saved=*end;*end=0;if(!simple(&dom->nodes[index],start))return 0;*end=saved;
        end=start;while(end>s&&isspace((unsigned char)end[-1]))end--;
        if(end==s)return 1;
        int direct=end[-1]=='>';if(direct){end--;while(end>s&&isspace((unsigned char)end[-1]))end--;}
        char *previous=end;while(previous>s&&!isspace((unsigned char)previous[-1])&&previous[-1]!='>')previous--;
        saved=*end;*end=0;
        index=dom->nodes[index].parent;
        if(!direct)while(index>=0&&!simple(&dom->nodes[index],previous))index=dom->nodes[index].parent;
        *end=saved;if(index<0)return 0;
    }return 0;
}
void css_free(browser_css *css){free(css->rules);memset(css,0,sizeof(*css));}
static int parse_rules(browser_css *css,const char *text,size_t length,int nesting)
{
    if(nesting>8){css->omitted=1;return -1;}
    if(!css->rules){css->rules=calloc(CSS_RULES_MAX,sizeof(*css->rules));if(!css->rules){css->omitted=1;return -1;}}
    size_t pos=0;
    while(pos<length){while(pos<length&&isspace((unsigned char)text[pos]))pos++;
        if(pos+1<length&&text[pos]=='/'&&text[pos+1]=='*'){pos+=2;while(pos+1<length&&(text[pos]!='*'||text[pos+1]!='/'))pos++;pos+=2;continue;}
        size_t begin=pos;while(pos<length&&text[pos]!='{'&&text[pos]!=';')pos++;
        if(pos==length)break;
        if(text[pos]==';'){pos++;continue;}
        size_t selector_length=pos-begin;pos++;size_t body=pos;int depth=1;char quote=0;
        while(pos<length&&depth){char c=text[pos];if(quote){if(c==quote&&(!pos||text[pos-1]!='\\'))quote=0;}else if(c=='\''||c=='"')quote=c;else if(c=='{')depth++;else if(c=='}')depth--;if(depth)pos++;}
        size_t body_length=pos-body;if(pos<length)pos++;
        if(text[begin]=='@'){
            /* Reader styles use screen media; ignore unsupported nested rules. */
            if(selector_length>=6&&!memcmp(text+begin,"@media",6)&&selector_length<96){char media[96];memcpy(media,text+begin,selector_length);media[selector_length]=0;if(!strstr(media,"print"))parse_rules(css,text+body,body_length,nesting+1);}
            continue;
        }
        if(selector_length>=192||body_length>=512){css->omitted=1;continue;}
        char selectors[192];memcpy(selectors,text+begin,selector_length);selectors[selector_length]=0;
        for(char *p=selectors;*p;){char *next=strchr(p,',');if(next)*next++=0;char *selector=trim(p);
            if(css->count==CSS_RULES_MAX){css->omitted=1;break;}
            css_rule *r=&css->rules[css->count];snprintf(r->selector,sizeof(r->selector),"%s",selector);memcpy(r->declarations,text+body,body_length);r->declarations[body_length]=0;r->order=css->count++;
            for(const char *q=selector;*q;q++)if(*q=='#')r->specificity+=100;else if(*q=='.'||*q==':')r->specificity+=10;else if(isalpha((unsigned char)*q)&&(q==selector||q[-1]==' '||q[-1]=='>'))r->specificity++;
            if(!next)break;
            p=next;
        }
    }return 0;
}
int css_add(browser_css *css,const char *text,size_t length)
{
    if(length>64*1024||css->bytes+length>128*1024){css->omitted=1;return -1;}
    css->bytes+=length;return parse_rules(css,text,length,0);
}
static int color(const char *v,uint32_t *out)
{
    unsigned r,g,b;
    if(*v=='#'&&strlen(v)==7&&sscanf(v+1,"%2x%2x%2x",&r,&g,&b)==3){*out=0xff000000u|r|(g<<8)|(b<<16);return 1;}
    if(*v=='#'&&strlen(v)==4&&sscanf(v+1,"%1x%1x%1x",&r,&g,&b)==3){*out=0xff000000u|(r*17)|((g*17)<<8)|((b*17)<<16);return 1;}
    if(sscanf(v,"rgb(%u,%u,%u)",&r,&g,&b)==3&&r<=255&&g<=255&&b<=255){*out=0xff000000u|r|(g<<8)|(b<<16);return 1;}
    static const struct{const char *name;uint32_t value;} names[]={{"black",0xff000000},{"white",0xffffffff},{"red",0xff0000ff},{"green",0xff008000},{"blue",0xffff0000},{"gray",0xff808080},{"grey",0xff808080},{"yellow",0xff00ffff},{"navy",0xff800000},{"purple",0xff800080},{"orange",0xff00a5ff},{"transparent",0}};
    for(size_t i=0;i<sizeof(names)/sizeof(*names);i++)if(eq(v,names[i].name)){*out=names[i].value;return 1;}
    return 0;
}
static void declarations(browser_style *s,const char *text,int priority)
{
    if(strlen(text)>=4096)return;
    char copy[4096];strcpy(copy,text);char *p=copy;
    while(*p){char *next=strchr(p,';');if(next)*next++=0;char *colon=strchr(p,':');if(colon){*colon++=0;char *key=trim(p),*v=trim(colon),*important=strchr(v,'!');int is_important=important&&eq(trim(important+1),"important");if(important)*important=0;v=trim(v);if(is_important!=priority){if(!next)break;p=next;continue;}
        if(eq(key,"color"))color(v,&s->color);
        else if(eq(key,"background-color")||eq(key,"background"))color(v,&s->background);
        else if(eq(key,"display")){if(eq(v,"none"))s->hidden=1;else if(eq(v,"block")||eq(v,"flex")||eq(v,"grid")||eq(v,"table")||eq(v,"list-item")){s->block=1;s->hidden=0;}else if(eq(v,"inline")||eq(v,"inline-block")){s->block=0;s->hidden=0;}}
        else if(eq(key,"visibility")&&eq(v,"hidden"))s->hidden=1;
        else if(eq(key,"font-weight")){if(eq(v,"bold")||eq(v,"bolder")||atoi(v)>=600)s->flags|=CSS_BOLD;else if(eq(v,"normal"))s->flags&=~CSS_BOLD;}
        else if(eq(key,"font-style")){if(eq(v,"italic")||eq(v,"oblique"))s->flags|=CSS_ITALIC;else if(eq(v,"normal"))s->flags&=~CSS_ITALIC;}
        else if(eq(key,"text-decoration")&&strstr(v,"underline"))s->flags|=CSS_UNDERLINE;
        else if(eq(key,"white-space"))s->pre=eq(v,"pre")||eq(v,"pre-wrap")||eq(v,"break-spaces");
        else if(eq(key,"text-align"))s->align=eq(v,"center")?1:eq(v,"right")?2:0;
        else if(eq(key,"font-size")){char *end;float value=strtof(v,&end);if(value>0){if(eq(end,"px"))value*=0.04f;else if(eq(end,"em")||eq(end,"rem"))value*=0.64f;else if(*end=='%')value*=0.0064f;else value=s->scale;if(value<0.45f)value=0.45f;if(value>1.1f)value=1.1f;s->scale=value;}else if(eq(v,"small"))s->scale=0.5f;else if(eq(v,"large"))s->scale=0.8f;}
    }if(!next)break;p=next;}
}
browser_style css_compute(const browser_css *css,const browser_dom *dom,int index,browser_style parent)
{
    browser_style s=parent;const dom_node *n=&dom->nodes[index];s.block=0;
    if(!strcmp(n->tag,"#text"))return s;
    s.hidden=0;
    static const char *blocks[]={"p","div","article","section","header","footer","main","nav","aside","h1","h2","h3","h4","h5","h6","li","ul","ol","tr","table","blockquote","pre","hr","br","form"};
    for(size_t i=0;i<sizeof(blocks)/sizeof(*blocks);i++)if(eq(n->tag,blocks[i]))s.block=1;
    if(eq(n->tag,"pre"))s.pre=1;
    if(eq(n->tag,"b")||eq(n->tag,"strong")||eq(n->tag,"th")||(n->tag[0]=='h'&&n->tag[1]>='1'&&n->tag[1]<='6'&&!n->tag[2]))s.flags|=CSS_BOLD;
    if(eq(n->tag,"i")||eq(n->tag,"em"))s.flags|=CSS_ITALIC;
    if(eq(n->tag,"h1"))s.scale=0.95f;else if(eq(n->tag,"h2"))s.scale=0.82f;
    if(eq(n->tag,"a")){s.color=0xffe6bc52;s.flags|=CSS_UNDERLINE;}
    int indices[CSS_RULES_MAX],count=0;
    for(int i=0;i<css->count;i++)if(css_matches(dom,index,css->rules[i].selector)){int j=count;while(j>0&&css->rules[indices[j-1]].specificity>css->rules[i].specificity){indices[j]=indices[j-1];j--;}indices[j]=i;count++;}
    for(int priority=0;priority<2;priority++){
        for(int i=0;i<count;i++)declarations(&s,css->rules[indices[i]].declarations,priority);
        declarations(&s,dom_attr(n,"style"),priority);
    }s.hidden|=parent.hidden;if(*dom_attr(n,"hidden"))s.hidden=1;
    /* The hidden attribute is also present when its value is empty. */
    for(int i=0;i<n->attribute_count;i++)if(eq(n->attributes[i].name,"hidden"))s.hidden=1;
    return s;
}

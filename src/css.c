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
/* Colors are 0xAABBGGRR for the PSP's GU. Transparent and unknown values
   (inherit, var(), gradients) leave the color unchanged. */
static int color(const char *v,uint32_t *out)
{
    unsigned r,g,b,a=255;float alpha=1;size_t n=strlen(v);
    if(*v=='#'&&(n==7||n==9)&&sscanf(v+1,"%2x%2x%2x",&r,&g,&b)==3){if(n==9&&sscanf(v+7,"%2x",&a)!=1)return 0;}
    else if(*v=='#'&&(n==4||n==5)&&sscanf(v+1,"%1x%1x%1x",&r,&g,&b)==3){r*=17;g*=17;b*=17;if(n==5){if(sscanf(v+4,"%1x",&a)!=1)return 0;a*=17;}}
    else if(sscanf(v,"rgba(%u,%u,%u,%f)",&r,&g,&b,&alpha)==4||sscanf(v,"rgb(%u,%u,%u,%f)",&r,&g,&b,&alpha)==4||sscanf(v,"rgb(%u,%u,%u)",&r,&g,&b)==3||sscanf(v,"rgb(%u %u %u",&r,&g,&b)==3){a=alpha<0.3f?0:255;if(r>255||g>255||b>255)return 0;}
    else {
        static const struct{const char *name;uint32_t rgb;} names[]={
            {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},{"blue",0x0000ff},{"gray",0x808080},{"grey",0x808080},
            {"yellow",0xffff00},{"navy",0x000080},{"purple",0x800080},{"orange",0xffa500},{"silver",0xc0c0c0},{"maroon",0x800000},
            {"olive",0x808000},{"lime",0x00ff00},{"aqua",0x00ffff},{"cyan",0x00ffff},{"teal",0x008080},{"fuchsia",0xff00ff},
            {"magenta",0xff00ff},{"lightgray",0xd3d3d3},{"lightgrey",0xd3d3d3},{"darkgray",0xa9a9a9},{"darkgrey",0xa9a9a9},
            {"dimgray",0x696969},{"whitesmoke",0xf5f5f5},{"gainsboro",0xdcdcdc},{"darkblue",0x00008b},{"darkred",0x8b0000},
            {"darkgreen",0x006400},{"lightblue",0xadd8e6},{"steelblue",0x4682b4},{"royalblue",0x4169e1},{"dodgerblue",0x1e90ff},
            {"skyblue",0x87ceeb},{"brown",0xa52a2a},{"pink",0xffc0cb},{"gold",0xffd700},{"beige",0xf5f5dc},{"ivory",0xfffff0},
            {"lightyellow",0xffffe0},{"crimson",0xdc143c},{"tomato",0xff6347},{"coral",0xff7f50},{"indigo",0x4b0082},
            {"violet",0xee82ee},{"tan",0xd2b48c},{"khaki",0xf0e68c},{"salmon",0xfa8072},{"lightgreen",0x90ee90},
            {"darkorange",0xff8c00},{"slategray",0x708090},{"aliceblue",0xf0f8ff},{"snow",0xfffafa},{"linen",0xfaf0e6},
            {"lavender",0xe6e6fa},{"midnightblue",0x191970},{"darkslategray",0x2f4f4f},{"orangered",0xff4500},
            {"seagreen",0x2e8b57},{"forestgreen",0x228b22},{"chocolate",0xd2691e},{"firebrick",0xb22222}};
        if(eq(v,"transparent")){*out=0;return 1;}
        for(size_t i=0;i<sizeof(names)/sizeof(*names);i++)if(eq(v,names[i].name)){uint32_t c=names[i].rgb;r=c>>16;g=(c>>8)&255;b=c&255;goto found;}
        return 0;
    }
found:
    *out=a<0x40?0:0xff000000u|r|(g<<8)|(b<<16);return 1;
}
int css_color(const char *v,uint32_t *out){char copy[64];if(strlen(v)>=sizeof(copy))return 0;strcpy(copy,v);return color(trim(copy),out);}
/* The first token of a shorthand that is a color ("1px solid #ccc", "#fff url(a.png)"). */
static int shorthand_color(const char *v,uint32_t *out)
{
    char token[64];
    for(const char *p=v;*p;){
        while(isspace((unsigned char)*p))p++;
        size_t n=0;int depth=0;
        while(*p&&(depth||!isspace((unsigned char)*p))){if(*p=='(')depth++;else if(*p==')')depth--;if(n<sizeof(token)-1)token[n++]=*p;p++;}
        token[n]=0;if(n&&color(token,out))return 1;
    }
    return 0;
}

#define CSS_PX 0.6f
#define HIDE_CLIP 1
#define HIDE_HEIGHT0 2
#define HIDE_OVERFLOW 4
#define HIDE_OFFSCREEN 8
#define HIDE_TINY_W 16
#define HIDE_TINY_H 32
/* A CSS length in layout pixels. Percentages give -percent when `percent`
   is set, else a share of the PSP screen; "auto" gives BOX_AUTO. */
static int length(const char *v,float scale,int percent,int *out,float *css_px)
{
    char *end;float x=strtof(v,&end),px;
    if(end==v){if(eq(v,"auto")){*out=BOX_AUTO;return 1;}if(eq(v,"thin")){x=1;end=(char *)"px";}else if(eq(v,"medium")){x=3;end=(char *)"px";}else if(eq(v,"thick")){x=5;end=(char *)"px";}else return 0;}
    if(eq(end,"px")||!*end)px=x;
    else if(eq(end,"em"))px=x*16*scale/0.64f;
    else if(eq(end,"rem"))px=x*16;
    else if(eq(end,"pt"))px=x*4/3;
    else if(eq(end,"vw"))px=x*7.87f;
    else if(eq(end,"vh"))px=x*4;
    else if(*end=='%'){if(percent){*out=x<=0?0:-(int)(x>100?100:x+0.5f);if(css_px)*css_px=-1;return 1;}px=x*7.87f;}
    else return 0;
    if(css_px)*css_px=px;
    float l=px*CSS_PX;*out=(int)(l<0?l-0.5f:l+0.5f);return 1;
}
static int clamp(int v,int low,int high){return v<low?low:v>high?high:v;}
/* "a b c d" into top/right/bottom/left with the usual repetition. */
static int sides(const char *v,float scale,int out[4])
{
    char copy[128];if(strlen(v)>=sizeof(copy))return 0;strcpy(copy,v);
    int value[4],n=0;
    for(char *p=strtok(copy," \t");p&&n<4;p=strtok(NULL," \t"))if(length(p,scale,0,&value[n],NULL))n++;else return 0;
    if(!n)return 0;
    out[0]=value[0];out[1]=n>1?value[1]:value[0];out[2]=n>2?value[2]:value[0];out[3]=n>3?value[3]:out[1];return 1;
}
static void border(browser_box *b,int side,const char *v,float scale)
{
    char copy[128];if(strlen(v)>=sizeof(copy))return;strcpy(copy,v);
    int width=-1;uint32_t c=0;int has_color=0,none=0;
    for(char *p=strtok(copy," \t");p;p=strtok(NULL," \t")){
        int w;if(eq(p,"none")||eq(p,"hidden")||eq(p,"0"))none=1;
        else if(length(p,scale,0,&w,NULL)&&w!=BOX_AUTO)width=w;
        else if(color(p,&c))has_color=1;
    }
    for(int i=0;i<4;i++)if(side<0||side==i){
        if(none)b->border[i]=0;else{b->border[i]=(unsigned char)clamp(width<0?1:width<1?1:width,0,6);if(has_color)b->border_color[i]=c;}
    }
}
static void declarations(browser_style *s,browser_box *b,const char *text,int priority)
{
    if(strlen(text)>=4096)return;
    char copy[4096];strcpy(copy,text);char *p=copy;
    while(*p){char *next=strchr(p,';');if(next)*next++=0;char *colon=strchr(p,':');if(colon){*colon++=0;char *key=trim(p),*v=trim(colon),*important=strchr(v,'!');int is_important=important&&eq(trim(important+1),"important");if(important)*important=0;v=trim(v);if(is_important!=priority){if(!next)break;p=next;continue;}
        if(eq(key,"color"))color(v,&s->color);
        else if(eq(key,"background-color"))color(v,&s->background);
        else if(eq(key,"background")){if(!shorthand_color(v,&s->background)&&eq(v,"none"))s->background=0;}
        else if(eq(key,"display")){
            int d=eq(v,"none")?DISPLAY_NONE:eq(v,"block")||eq(v,"grid")||eq(v,"flow-root")?DISPLAY_BLOCK:eq(v,"flex")?DISPLAY_FLEX:
                  eq(v,"list-item")?DISPLAY_LIST_ITEM:eq(v,"table")?DISPLAY_TABLE:eq(v,"table-row")?DISPLAY_ROW:eq(v,"table-cell")?DISPLAY_CELL:
                  eq(v,"inline")||eq(v,"contents")?DISPLAY_INLINE:eq(v,"inline-block")||eq(v,"inline-flex")||eq(v,"inline-grid")?DISPLAY_INLINE_BLOCK:-1;
            if(d==DISPLAY_NONE)s->hidden=1;else if(d>=0){s->hidden=0;s->block=d!=DISPLAY_INLINE&&d!=DISPLAY_INLINE_BLOCK;}
            if(b&&d>=0)b->display=(unsigned char)d;
        }
        else if(eq(key,"visibility")&&(eq(v,"hidden")||eq(v,"collapse")))s->hidden=1;
        else if(eq(key,"font-weight")){if(eq(v,"bold")||eq(v,"bolder")||atoi(v)>=600)s->flags|=CSS_BOLD;else if(eq(v,"normal")||eq(v,"lighter")||(atoi(v)>0&&atoi(v)<600))s->flags&=~CSS_BOLD;}
        else if(eq(key,"font-style")){if(eq(v,"italic")||eq(v,"oblique"))s->flags|=CSS_ITALIC;else if(eq(v,"normal"))s->flags&=~CSS_ITALIC;}
        else if(eq(key,"text-decoration")||eq(key,"text-decoration-line")){
            if(eq(v,"none"))s->flags&=~(CSS_UNDERLINE|CSS_STRIKE);
            else{if(strstr(v,"underline"))s->flags|=CSS_UNDERLINE;if(strstr(v,"line-through"))s->flags|=CSS_STRIKE;}
        }
        else if(eq(key,"text-transform")){s->flags&=~(CSS_UPPERCASE|CSS_LOWERCASE);if(eq(v,"uppercase"))s->flags|=CSS_UPPERCASE;else if(eq(v,"lowercase"))s->flags|=CSS_LOWERCASE;}
        else if(eq(key,"white-space"))s->pre=eq(v,"pre")||eq(v,"pre-wrap")||eq(v,"break-spaces")||eq(v,"pre-line");
        else if(eq(key,"text-align"))s->align=eq(v,"center")||eq(v,"-webkit-center")?1:eq(v,"right")||eq(v,"end")?2:0;
        else if(eq(key,"list-style")||eq(key,"list-style-type"))s->list_none=strstr(v,"none")!=NULL;
        else if(eq(key,"font-size")||eq(key,"font")){
            char size[64]="";
            if(eq(key,"font")){char tmp[256];if(strlen(v)<sizeof(tmp)){strcpy(tmp,v);for(char *t=strtok(tmp," \t");t;t=strtok(NULL," \t")){if(eq(t,"bold"))s->flags|=CSS_BOLD;else if(eq(t,"italic"))s->flags|=CSS_ITALIC;else if(isdigit((unsigned char)*t)||*t=='.'){char *slash=strchr(t,'/');if(slash)*slash=0;if(strlen(t)<sizeof(size))strcpy(size,t);if(strtof(t,NULL)>=100&&!strpbrk(t,"pe%r"))size[0]=0;}}}}
            else if(strlen(v)<sizeof(size))strcpy(size,v);
            if(*size){char *end;float value=strtof(size,&end);
                if(end!=size&&value>=0){
                    if(eq(end,"px"))value*=0.04f;else if(eq(end,"pt"))value*=0.0533f;else if(eq(end,"em"))value*=s->scale;else if(eq(end,"rem"))value*=0.64f;else if(*end=='%')value*=s->scale/100;else value=s->scale;
                    if(value<=0.01f){if(b)b->hide|=HIDE_CLIP;value=s->scale;}
                    if(value<0.45f)value=0.45f;
                    if(value>1.1f)value=1.1f;
                    s->scale=value;
                }
                else{static const struct{const char *name;float scale;} k[]={{"xx-small",0.45f},{"x-small",0.48f},{"small",0.54f},{"medium",0.64f},{"large",0.76f},{"x-large",0.9f},{"xx-large",1.05f},{"smaller",0},{"larger",-1}};
                    for(size_t i=0;i<sizeof(k)/sizeof(*k);i++)if(eq(size,k[i].name)){s->scale=k[i].scale>0?k[i].scale:k[i].scale==0?s->scale*0.85f:s->scale*1.2f;if(s->scale<0.45f)s->scale=0.45f;if(s->scale>1.1f)s->scale=1.1f;}}
            }
        }
        else if(b){
            int value,four[4];float px;
            if(eq(key,"margin")){if(sides(v,s->scale,four))for(int i=0;i<4;i++){b->margin[i]=(short)(four[i]==BOX_AUTO?BOX_AUTO:clamp(four[i],-40,120));b->set|=1<<i;}}
            else if(!strncmp(key,"margin-",7)){int i=eq(key+7,"top")?0:eq(key+7,"right")?1:eq(key+7,"bottom")?2:eq(key+7,"left")?3:-1;if(i>=0&&length(v,s->scale,0,&value,NULL)){b->margin[i]=(short)(value==BOX_AUTO?BOX_AUTO:clamp(value,-40,120));b->set|=1<<i;}}
            else if(eq(key,"padding")){if(sides(v,s->scale,four))for(int i=0;i<4;i++){b->padding[i]=(short)clamp(four[i]==BOX_AUTO?0:four[i],0,60);b->set|=16<<i;}}
            else if(!strncmp(key,"padding-",8)){int i=eq(key+8,"top")?0:eq(key+8,"right")?1:eq(key+8,"bottom")?2:eq(key+8,"left")?3:-1;if(i>=0&&length(v,s->scale,0,&value,NULL)){b->padding[i]=(short)clamp(value==BOX_AUTO?0:value,0,60);b->set|=16<<i;}}
            else if(eq(key,"border"))border(b,-1,v,s->scale);
            else if(!strncmp(key,"border-",7)&&!strchr(key+7,'-')){int i=eq(key+7,"top")?0:eq(key+7,"right")?1:eq(key+7,"bottom")?2:eq(key+7,"left")?3:-1;if(i>=0)border(b,i,v,s->scale);}
            else if(eq(key,"border-width")){if(sides(v,s->scale,four))for(int i=0;i<4;i++)b->border[i]=(unsigned char)clamp(four[i]==BOX_AUTO?0:four[i]<1&&four[i]>0?1:four[i],0,6);}
            else if(eq(key,"border-color")){uint32_t c;if(color(v,&c))for(int i=0;i<4;i++)b->border_color[i]=c;}
            else if(eq(key,"border-style")&&(eq(v,"none")||eq(v,"hidden")))memset(b->border,0,sizeof(b->border));
            else if(eq(key,"width")||eq(key,"max-width")||eq(key,"height")){
                if(length(v,s->scale,1,&value,&px)){
                    if(value==BOX_AUTO)value=0;
                    if(eq(key,"width")){b->width=(short)(value>2000?2000:value);if(px>=0&&px<=1)b->hide|=HIDE_TINY_W;}
                    else if(eq(key,"max-width"))b->max_width=(short)(value>2000?2000:value);
                    else{b->height=(short)(value>2000?2000:value);if(px==0)b->hide|=HIDE_HEIGHT0;if(px>0&&px<=1)b->hide|=HIDE_TINY_H;}
                }
                else if(eq(v,"none"))b->max_width=0;
            }
            else if(eq(key,"overflow")&&(eq(v,"hidden")||eq(v,"clip")))b->hide|=HIDE_OVERFLOW;
            else if(eq(key,"justify-content"))b->justify=eq(v,"center")||eq(v,"space-around")||eq(v,"space-evenly")?2:eq(v,"flex-end")||eq(v,"end")||eq(v,"right")?3:eq(v,"flex-start")||eq(v,"start")||eq(v,"left")?1:0;
            else if((eq(key,"clip")&&strstr(v,"rect"))||(eq(key,"clip-path")&&strstr(v,"inset(50%")))b->hide|=HIDE_CLIP;
            else if((eq(key,"left")||eq(key,"top")||eq(key,"text-indent"))&&length(v,s->scale,0,&value,&px)&&px<=-500)b->hide|=HIDE_OFFSCREEN;
        }
    }if(!next)break;p=next;}
}
static browser_style compute(const browser_css *css,const browser_dom *dom,int index,browser_style parent,browser_box *box)
{
    browser_style s=parent;const dom_node *n=&dom->nodes[index];s.block=0;
    if(box)memset(box,0,sizeof(*box));
    if(!strcmp(n->tag,"#text"))return s;
    s.hidden=0;s.background=0;
    static const char *blocks[]={"p","div","article","section","header","footer","main","nav","aside","h1","h2","h3","h4","h5","h6","li","ul","ol","tr","table","blockquote","pre","hr","br","form",
        "dl","dt","dd","figure","figcaption","fieldset","address","center","details","summary","menu","dir","hgroup","legend","caption","tbody","thead","tfoot","body","html"};
    for(size_t i=0;i<sizeof(blocks)/sizeof(*blocks);i++)if(eq(n->tag,blocks[i]))s.block=1;
    if(eq(n->tag,"pre")||eq(n->tag,"textarea")||eq(n->tag,"listing")||eq(n->tag,"xmp"))s.pre=1;
    if(eq(n->tag,"b")||eq(n->tag,"strong")||eq(n->tag,"th")||(n->tag[0]=='h'&&n->tag[1]>='1'&&n->tag[1]<='6'&&!n->tag[2]))s.flags|=CSS_BOLD;
    if(eq(n->tag,"i")||eq(n->tag,"em")||eq(n->tag,"cite")||eq(n->tag,"var")||eq(n->tag,"address"))s.flags|=CSS_ITALIC;
    if(eq(n->tag,"u")||eq(n->tag,"ins"))s.flags|=CSS_UNDERLINE;
    if(eq(n->tag,"s")||eq(n->tag,"strike")||eq(n->tag,"del"))s.flags|=CSS_STRIKE;
    if(eq(n->tag,"h1"))s.scale=0.95f;else if(eq(n->tag,"h2"))s.scale=0.82f;else if(eq(n->tag,"h3"))s.scale=0.74f;
    else if(eq(n->tag,"small")||eq(n->tag,"sub")||eq(n->tag,"sup"))s.scale=s.scale*0.85f<0.45f?0.45f:s.scale*0.85f;
    else if(eq(n->tag,"big"))s.scale=s.scale*1.2f>1.1f?1.1f:s.scale*1.2f;
    if(eq(n->tag,"center"))s.align=1;
    if(eq(n->tag,"td")||eq(n->tag,"th")){if(eq(n->tag,"th"))s.align=1;}
    {const char *align=dom_attr(n,"align");if(s.block||eq(n->tag,"td")||eq(n->tag,"th")){if(eq(align,"center")||eq(align,"middle"))s.align=1;else if(eq(align,"right"))s.align=2;else if(eq(align,"left"))s.align=0;}}
    /* Presentational attributes, which old and minimal pages still use. */
    uint32_t c;
    if((eq(n->tag,"font")||eq(n->tag,"basefont"))&&*dom_attr(n,"color")&&color(dom_attr(n,"color"),&c))s.color=c;
    if(eq(n->tag,"font")&&*dom_attr(n,"size")){int k=atoi(dom_attr(n,"size"));const char *v=dom_attr(n,"size");if(*v=='+'||*v=='-')k=3+k;static const float sizes[]={0.48f,0.54f,0.64f,0.74f,0.86f,1.0f,1.1f};if(k<1)k=1;if(k>7)k=7;s.scale=sizes[k-1];}
    if(*dom_attr(n,"bgcolor")&&color(dom_attr(n,"bgcolor"),&c))s.background=c;
    if(eq(n->tag,"body")&&*dom_attr(n,"text")&&color(dom_attr(n,"text"),&c))s.color=c;
    if(eq(n->tag,"body")&&*dom_attr(n,"link")&&color(dom_attr(n,"link"),&c))s.link=c;
    if(eq(n->tag,"a")&&*dom_attr(n,"href")){s.color=s.link?s.link:0xffe6bc52;s.flags|=CSS_UNDERLINE;}
    if(box){
        if(eq(n->tag,"table")&&*dom_attr(n,"border")&&atoi(dom_attr(n,"border"))>0)for(int i=0;i<4;i++){box->border[i]=1;box->border_color[i]=0xff808080;}
        const char *w=dom_attr(n,"width");
        if(*w&&(eq(n->tag,"table")||eq(n->tag,"td")||eq(n->tag,"th")||eq(n->tag,"img")||eq(n->tag,"col")||eq(n->tag,"hr")||eq(n->tag,"iframe"))){char *end;float x=strtof(w,&end);if(x>0)box->width=(short)(*end=='%'?-(x>100?100:x):x*CSS_PX+0.5f);}
        const char *h=dom_attr(n,"height");
        if(*h&&(eq(n->tag,"img")||eq(n->tag,"iframe"))){float x=strtof(h,NULL);if(x>0)box->height=(short)(x*CSS_PX+0.5f);}
    }
    int indices[CSS_RULES_MAX],count=0;
    for(int i=0;i<css->count;i++)if(css_matches(dom,index,css->rules[i].selector)){int j=count;while(j>0&&css->rules[indices[j-1]].specificity>css->rules[i].specificity){indices[j]=indices[j-1];j--;}indices[j]=i;count++;}
    for(int priority=0;priority<2;priority++){
        for(int i=0;i<count;i++)declarations(&s,box,css->rules[indices[i]].declarations,priority);
        declarations(&s,box,dom_attr(n,"style"),priority);
    }s.hidden|=parent.hidden;if(*dom_attr(n,"hidden"))s.hidden=1;
    /* The hidden attribute is also present when its value is empty. */
    for(int i=0;i<n->attribute_count;i++)if(eq(n->attributes[i].name,"hidden"))s.hidden=1;
    if(box){
        int h=box->hide;
        /* Screen-reader-only text and collapsed menus: clipped, off-screen,
           1x1 or zero-height boxes with hidden overflow. */
        box->hide=(h&HIDE_CLIP)||(h&HIDE_OFFSCREEN)||((h&HIDE_TINY_W)&&(h&HIDE_TINY_H))||((h&HIDE_HEIGHT0)&&(h&HIDE_OVERFLOW));
        if(eq(dom_attr(n,"aria-hidden"),"true")&&(eq(n->tag,"svg")||eq(n->tag,"span")||eq(n->tag,"i")))box->hide=1;
    }
    return s;
}
browser_style css_compute(const browser_css *css,const browser_dom *dom,int index,browser_style parent){return compute(css,dom,index,parent,NULL);}
browser_style css_compute_box(const browser_css *css,const browser_dom *dom,int index,browser_style parent,browser_box *box){return compute(css,dom,index,parent,box);}

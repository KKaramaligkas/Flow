/* ARK Browser CSS: the cascade of the rules a page can use, sized for the PSP.
   Stylesheets are reduced to the rules whose subject occurs in the page
   (css_compact), @media queries see the page view's width, rules are indexed
   by their subject's id, class or tag, and var() resolves per element. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "css.h"

enum { KEY_ANY, KEY_TAG, KEY_CLASS, KEY_ID };

static int eq(const char *a,const char *b){while(*a&&*b)if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0;return !*a&&!*b;}
/* Case-insensitive comparison of n bytes. */
static int same(const char *a,const char *b,size_t n){for(size_t i=0;i<n;i++)if(tolower((unsigned char)a[i])!=tolower((unsigned char)b[i]))return 0;return 1;}
static char *trim(char *s){while(isspace((unsigned char)*s))s++;size_t n=strlen(s);while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;return s;}
static int word(const char *list,const char *name){size_t n=strlen(name);for(const char *p=list;*p;){while(isspace((unsigned char)*p))p++;const char *end=p;while(*end&&!isspace((unsigned char)*end))end++;if((size_t)(end-p)==n&&!memcmp(p,name,n))return 1;p=end;}return 0;}
static unsigned hash(const char *s,size_t n,int fold){unsigned h=2166136261u;for(size_t i=0;i<n;i++){unsigned char c=(unsigned char)s[i];if(fold)c=(unsigned char)tolower(c);h=(h^c)*16777619u;}return h;}

/* ---- selectors ---- */

static int utf8_put(unsigned cp,char *out)
{
    if(cp<0x80){out[0]=(char)cp;return 1;}
    if(cp<0x800){out[0]=(char)(0xC0|(cp>>6));out[1]=(char)(0x80|(cp&63));return 2;}
    if(cp<0x10000){out[0]=(char)(0xE0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&63));out[2]=(char)(0x80|(cp&63));return 3;}
    out[0]=(char)(0xF0|(cp>>18));out[1]=(char)(0x80|((cp>>12)&63));out[2]=(char)(0x80|((cp>>6)&63));out[3]=(char)(0x80|(cp&63));return 4;
}
static int ident_start(const char *s){unsigned char c=(unsigned char)*s;return isalpha(c)||c=='_'||c=='-'||c>=0x80||(c=='\\'&&s[1]);}
/* Reads an identifier with its escapes: Tailwind's ".md\:flex" is class "md:flex". */
static size_t ident(const char **p,char *out,size_t size)
{
    const char *s=*p;size_t n=0;
    while(*s){
        unsigned char c=(unsigned char)*s;
        if(c=='\\'&&s[1]&&s[1]!='\n'){
            s++;
            if(isxdigit((unsigned char)*s)){
                unsigned cp=0;int k=0;
                while(k<6&&isxdigit((unsigned char)*s)){cp=cp*16+(unsigned)(isdigit((unsigned char)*s)?*s-'0':tolower((unsigned char)*s)-'a'+10);s++;k++;}
                if(*s==' '||*s=='\t'||*s=='\n')s++;
                char b[4];int m=utf8_put(cp&&cp<=0x10FFFF?cp:0xFFFD,b);
                for(int i=0;i<m;i++)if(n+1<size)out[n++]=b[i];
            }else{if(n+1<size)out[n++]=*s;s++;}
            continue;
        }
        if(isalnum(c)||c=='-'||c=='_'||c>=0x80){if(n+1<size)out[n++]=(char)c;s++;continue;}
        break;
    }
    out[n]=0;*p=s;return n;
}
/* Past a group starting at s ('[' or '('), with its quotes and escapes. */
static const char *group_end(const char *s)
{
    char open=*s,close=open=='['?']':')',quote=0;int depth=0;
    for(;*s;s++){
        if(*s=='\\'&&s[1]){s++;continue;}
        if(quote){if(*s==quote)quote=0;continue;}
        if(*s=='"'||*s=='\'')quote=*s;
        else if(*s==open)depth++;
        else if(*s==close&&--depth==0)return s+1;
    }
    return s;
}
static const char *compound_end(const char *s)
{
    while(*s){
        if(*s=='\\'&&s[1]){s+=2;continue;}
        if(*s=='['||*s=='('){s=group_end(s);continue;}
        if(isspace((unsigned char)*s)||*s=='>'||*s=='+'||*s=='~'||*s==',')break;
        s++;
    }
    return s;
}
/* A complex selector as compounds; combinator: how a compound relates to
   the one before it (' ', '>', '+', '~'). */
typedef struct { const char *start,*end; char combinator; } part;
#define PARTS_MAX 16
static int split(const char *s,part *parts)
{
    int n=0;char combinator=0;
    while(*s){
        if(isspace((unsigned char)*s)){if(n&&!combinator)combinator=' ';s++;continue;}
        if(*s=='>'||*s=='+'||*s=='~'){combinator=*s++;continue;}
        const char *end=compound_end(s);
        if(end==s||n==PARTS_MAX)return -1;
        parts[n].start=s;parts[n].end=end;parts[n].combinator=n?(combinator?combinator:' '):0;n++;combinator=0;
        s=end;
    }
    return n;
}
static int element(const dom_node *n){return n->tag[0]!='#';}
static int previous_element(const browser_dom *dom,int index)
{
    int parent=dom->nodes[index].parent,last=-1;if(parent<0)return -1;
    for(int c=dom->nodes[parent].first;c>=0&&c!=index;c=dom->nodes[c].next)if(element(&dom->nodes[c]))last=c;
    return last;
}
static int next_element(const browser_dom *dom,int index){for(int c=dom->nodes[index].next;c>=0;c=dom->nodes[c].next)if(element(&dom->nodes[c]))return c;return -1;}
static int has_attribute(const dom_node *n,const char *name){for(int i=0;i<n->attribute_count;i++)if(eq(n->attributes[i].name,name))return 1;return 0;}
/* [name], [name=v], [name~=v], [name|=v], [name^=v], [name$=v], [name*=v], with " i" for any case. */
static int attribute(const dom_node *n,const char *s,const char *e)
{
    char name[64],want[256];const char *p;
    s++;while(s<e&&isspace((unsigned char)*s))s++;
    p=s;ident(&p,name,sizeof(name));s=p;
    if(*s=='|'&&s[1]!='='){s++;p=s;ident(&p,name,sizeof(name));s=p;}
    while(s<e&&isspace((unsigned char)*s))s++;
    const char *value=NULL;
    for(int i=0;i<n->attribute_count;i++)if(eq(n->attributes[i].name,name)){value=n->attributes[i].value;break;}
    if(!value)return 0;
    if(*s==']')return 1;
    char op=*s;if(op!='=')s++;
    if(*s!='=')return 0;
    s++;while(s<e&&isspace((unsigned char)*s))s++;
    size_t k=0;
    if(*s=='"'||*s=='\''){char q=*s++;while(s<e&&*s!=q){if(*s=='\\'&&s[1])s++;if(k+1<sizeof(want))want[k++]=*s;s++;}if(*s==q)s++;want[k]=0;}
    else{p=s;k=ident(&p,want,sizeof(want));s=p;}
    while(s<e&&isspace((unsigned char)*s))s++;
    int fold=*s=='i'||*s=='I';
    size_t vl=strlen(value);
    #define SAME(a,b,len) (fold?same(a,b,len):!memcmp(a,b,len))
    switch(op){
    case '=':return vl==k&&SAME(value,want,k);
    case '~':for(const char *w=value;*w;){while(isspace((unsigned char)*w))w++;const char *end=w;while(*end&&!isspace((unsigned char)*end))end++;if((size_t)(end-w)==k&&k&&SAME(w,want,k))return 1;w=end;}return 0;
    case '|':return vl>=k&&SAME(value,want,k)&&(vl==k||value[k]=='-');
    case '^':return k&&vl>=k&&SAME(value,want,k);
    case '$':return k&&vl>=k&&SAME(value+vl-k,want,k);
    case '*':if(!k)return 0;for(size_t i=0;i+k<=vl;i++)if(SAME(value+i,want,k))return 1;return 0;
    }
    #undef SAME
    return 0;
}
/* an+b ("odd", "even", "3", "2n+1", "-n+3") against a 1-based position. */
static int nth(const char *s,const char *e,int position)
{
    char text[32];size_t n=0;
    for(;s<e&&n<sizeof(text)-1;s++)if(!isspace((unsigned char)*s))text[n++]=(char)tolower((unsigned char)*s);
    text[n]=0;
    char *of=strstr(text,"of");if(of)*of=0;     /* "of S" isn't supported: an+b alone */
    int a=0,b=0;
    if(!strcmp(text,"odd")){a=2;b=1;}
    else if(!strcmp(text,"even")){a=2;b=0;}
    else{
        char *nn=strchr(text,'n');
        if(!nn){char *end;b=(int)strtol(text,&end,10);if(*end)return 0;}
        else{
            *nn=0;a=!*text||!strcmp(text,"+")?1:!strcmp(text,"-")?-1:atoi(text);
            if(nn[1]){char *end;b=(int)strtol(nn+1,&end,10);if(*end)return 0;}
        }
    }
    if(!a)return position==b;
    return (position-b)/a>=0&&(position-b)%a==0;
}
static int position(const browser_dom *dom,int index,int from_end,int same_type)
{
    int count=1,parent=dom->nodes[index].parent;if(parent<0)return 1;
    const char *tag=dom->nodes[index].tag;int seen=0;
    for(int c=dom->nodes[parent].first;c>=0;c=dom->nodes[c].next){
        const dom_node *k=&dom->nodes[c];
        if(c==index){seen=1;continue;}
        if(!element(k)||(same_type&&!eq(k->tag,tag)))continue;
        if(from_end?seen:!seen)count++;
    }
    return count;
}
static int matches(const browser_dom *dom,int index,const char *selector,int *budget);
/* Whether any selector of a comma-separated list matches. */
static int any_matches(const browser_dom *dom,int index,const char *s,const char *e,int *budget)
{
    while(s<e){
        const char *end=s;
        while(end<e&&*end!=','){if(*end=='\\'&&end+1<e){end+=2;continue;}if(*end=='('||*end=='['){end=group_end(end);continue;}end++;}
        char one[256];size_t n=(size_t)(end-s);
        if(n<sizeof(one)){memcpy(one,s,n);one[n]=0;if(matches(dom,index,trim(one),budget))return 1;}
        s=end<e?end+1:e;
    }
    return 0;
}
static int pseudo_element(const char *name)
{
    return eq(name,"before")||eq(name,"after")||eq(name,"first-line")||eq(name,"first-letter")||eq(name,"marker")||eq(name,"placeholder")||
           eq(name,"selection")||eq(name,"backdrop")||!strncmp(name,"-webkit-",8)||!strncmp(name,"-moz-",5)||!strncmp(name,"-ms-",4);
}
static int form_field(const dom_node *n){return eq(n->tag,"input")||eq(n->tag,"button")||eq(n->tag,"select")||eq(n->tag,"textarea")||eq(n->tag,"option")||eq(n->tag,"fieldset");}
/* Pseudo-classes. States the page view doesn't have (hover, focus, visited,
   target) never match, so rules apply as to a page nobody has touched. */
static int pseudo(const browser_dom *dom,int index,const char *name,const char *args,const char *args_end,int *budget)
{
    const dom_node *n=&dom->nodes[index];
    if(eq(name,"root")||eq(name,"scope"))return eq(n->tag,"html");
    if(eq(name,"first-child"))return previous_element(dom,index)<0;
    if(eq(name,"last-child"))return next_element(dom,index)<0;
    if(eq(name,"only-child"))return previous_element(dom,index)<0&&next_element(dom,index)<0;
    if(eq(name,"first-of-type"))return position(dom,index,0,1)==1;
    if(eq(name,"last-of-type"))return position(dom,index,1,1)==1;
    if(eq(name,"only-of-type"))return position(dom,index,0,1)==1&&position(dom,index,1,1)==1;
    if(args&&eq(name,"nth-child"))return nth(args,args_end,position(dom,index,0,0));
    if(args&&eq(name,"nth-last-child"))return nth(args,args_end,position(dom,index,1,0));
    if(args&&eq(name,"nth-of-type"))return nth(args,args_end,position(dom,index,0,1));
    if(args&&eq(name,"nth-last-of-type"))return nth(args,args_end,position(dom,index,1,1));
    if(eq(name,"empty")){for(int c=n->first;c>=0;c=dom->nodes[c].next)if(element(&dom->nodes[c])||(dom->nodes[c].text&&*dom->nodes[c].text))return 0;return 1;}
    if(args&&eq(name,"not"))return !any_matches(dom,index,args,args_end,budget);
    if(args&&(eq(name,"is")||eq(name,"where")||eq(name,"matches")||eq(name,"any")))return any_matches(dom,index,args,args_end,budget);
    if(eq(name,"link")||eq(name,"any-link"))return (eq(n->tag,"a")||eq(n->tag,"area"))&&has_attribute(n,"href");
    if(eq(name,"checked"))return (eq(n->tag,"input")&&has_attribute(n,"checked"))||(eq(n->tag,"option")&&has_attribute(n,"selected"));
    if(eq(name,"disabled"))return form_field(n)&&has_attribute(n,"disabled");
    if(eq(name,"enabled"))return form_field(n)&&!has_attribute(n,"disabled");
    if(eq(name,"required"))return form_field(n)&&has_attribute(n,"required");
    if(eq(name,"optional"))return form_field(n)&&!has_attribute(n,"required");
    if(eq(name,"read-only"))return !form_field(n)||has_attribute(n,"readonly");
    if(eq(name,"read-write"))return form_field(n)&&!has_attribute(n,"readonly");
    if(eq(name,"placeholder-shown"))return has_attribute(n,"placeholder")&&!*dom_attr(n,"value");
    if(eq(name,"open"))return has_attribute(n,"open");
    if(eq(name,"defined"))return 1;
    if(args&&eq(name,"lang")){
        char want[32];size_t k=0;for(const char *p=args;p<args_end&&k<sizeof(want)-1;p++)if(!isspace((unsigned char)*p)&&*p!='"'&&*p!='\'')want[k++]=*p;want[k]=0;
        for(int i=index;i>=0;i=dom->nodes[i].parent){const char *lang=dom_attr(&dom->nodes[i],"lang");if(*lang)return same(lang,want,k)&&(!lang[k]||lang[k]=='-');}
        return 0;
    }
    if(args&&eq(name,"dir"))return args_end>args&&same(args,"ltr",3);
    return 0;
}
static int compound(const browser_dom *dom,int index,const char *s,const char *e,int *budget)
{
    const dom_node *n=&dom->nodes[index];
    if(!element(n))return 0;
    char name[128];const char *p;
    if(*s=='*'){s++;if(*s=='|'){s++;if(*s=='*')s++;else{p=s;ident(&p,name,sizeof(name));s=p;if(!eq(n->tag,name))return 0;}}}
    else if(ident_start(s)){
        p=s;ident(&p,name,sizeof(name));s=p;
        if(*s=='|'){s++;if(*s=='*')s++;else{p=s;ident(&p,name,sizeof(name));s=p;if(!eq(n->tag,name))return 0;}}
        else if(!eq(n->tag,name))return 0;
    }
    while(s<e){
        char c=*s++;
        if(c=='#'){p=s;ident(&p,name,sizeof(name));s=p;if(strcmp(dom_attr(n,"id"),name))return 0;}
        else if(c=='.'){p=s;ident(&p,name,sizeof(name));s=p;if(!word(dom_attr(n,"class"),name))return 0;}
        else if(c=='['){const char *end=group_end(s-1);if(!attribute(n,s-1,end))return 0;s=end;}
        else if(c==':'){
            int pe=*s==':';if(pe)s++;
            p=s;ident(&p,name,sizeof(name));s=p;
            const char *args=NULL,*args_end=NULL;
            if(*s=='('){const char *end=group_end(s);args=s+1;args_end=end>args?end-1:args;s=end;}
            if(pe||pseudo_element(name))return 0;
            if(!pseudo(dom,index,name,args,args_end,budget))return 0;
        }
        else return 0;
    }
    return 1;
}
static int match_from(const browser_dom *dom,int index,const part *parts,int k,int *budget)
{
    if(--*budget<0||!compound(dom,index,parts[k].start,parts[k].end,budget))return 0;
    if(!k)return 1;
    switch(parts[k].combinator){
    case '>':{int p=dom->nodes[index].parent;return p>=0&&match_from(dom,p,parts,k-1,budget);}
    case '+':{int s=previous_element(dom,index);return s>=0&&match_from(dom,s,parts,k-1,budget);}
    case '~':for(int s=previous_element(dom,index);s>=0;s=previous_element(dom,s))if(match_from(dom,s,parts,k-1,budget))return 1;return 0;
    default:for(int p=dom->nodes[index].parent;p>=0;p=dom->nodes[p].parent)if(match_from(dom,p,parts,k-1,budget))return 1;return 0;
    }
}
static int matches(const browser_dom *dom,int index,const char *selector,int *budget)
{
    part parts[PARTS_MAX];int n=split(selector,parts);
    return n>0&&match_from(dom,index,parts,n-1,budget);
}
int css_matches(const browser_dom *dom,int index,const char *selector)
{
    if(index<0||index>=dom->count)return 0;
    int budget=2048;     /* bounds descendant backtracking on deep pages */
    return matches(dom,index,selector,&budget);
}
/* Specificity as a*10000 + b*100 + c. */
static int specificity(const char *s,const char *e)
{
    int a=0,b=0,c=0,extra=0;char name[64];const char *p;
    while(s<e&&*s){
        if(*s=='\\'&&s[1]){s+=2;continue;}
        if(*s=='#'){a++;s++;p=s;ident(&p,name,sizeof(name));s=p;continue;}
        if(*s=='.'){b++;s++;p=s;ident(&p,name,sizeof(name));s=p;continue;}
        if(*s=='['){b++;s=group_end(s);continue;}
        if(*s==':'){
            int pe=s[1]==':';s+=pe?2:1;p=s;ident(&p,name,sizeof(name));s=p;
            const char *args=NULL,*args_end=NULL;
            if(*s=='('){const char *end=group_end(s);args=s+1;args_end=end>args?end-1:args;s=end;}
            if(pe||pseudo_element(name)){c++;continue;}
            if(eq(name,"where"))continue;
            if(args&&(eq(name,"not")||eq(name,"is")||eq(name,"matches")||eq(name,"has")||eq(name,"any"))){
                int best=0;
                for(const char *q=args;q<args_end;){
                    const char *end=q;
                    while(end<args_end&&*end!=','){if(*end=='('||*end=='['){end=group_end(end);continue;}end++;}
                    int value=specificity(q,end);if(value>best)best=value;
                    q=end<args_end?end+1:args_end;
                }
                extra+=best;continue;
            }
            b++;continue;
        }
        if(ident_start(s)&&*s!='-'){c++;p=s;ident(&p,name,sizeof(name));s=p;continue;}
        s++;
    }
    if(b>99)b=99;
    if(c>99)c=99;
    return a*10000+b*100+c+extra;
}

/* ---- the page's ids, classes and tags ---- */

/* The page's keys (sorted, for dropping rules), and with `nodes` each
   element's tag, id and class hashes (for matching compiled selectors). */
struct css_features { unsigned *keys; int count, nodes; unsigned *tag, *id, *classes; int *class_first; unsigned char *class_count; };
static unsigned feature(unsigned kind,unsigned h){return (h&~3u)|kind;}
static int compare_keys(const void *a,const void *b){unsigned x=*(const unsigned *)a,y=*(const unsigned *)b;return x<y?-1:x>y;}
static void features_free(css_features *f);
static css_features *features_build(const browser_dom *dom,int per_node)
{
    css_features *f=calloc(1,sizeof(*f));if(!f)return NULL;
    int capacity=dom->count*2+16,class_total=0;
    f->keys=malloc((size_t)capacity*sizeof(unsigned));if(!f->keys){free(f);return NULL;}
    if(per_node){
        f->nodes=dom->count;
        f->tag=calloc((size_t)dom->count,sizeof(unsigned));f->id=calloc((size_t)dom->count,sizeof(unsigned));
        f->class_first=calloc((size_t)dom->count,sizeof(int));f->class_count=calloc((size_t)dom->count,1);
        f->classes=malloc((size_t)capacity*sizeof(unsigned));
        if(!f->tag||!f->id||!f->class_first||!f->class_count||!f->classes){features_free(f);return NULL;}
    }
    for(int i=0;i<dom->count;i++){
        const dom_node *n=&dom->nodes[i];if(!element(n))continue;
        unsigned keys[40];int k=0;
        unsigned tag=hash(n->tag,strlen(n->tag),1);keys[k++]=feature(KEY_TAG,tag);
        const char *id=n->attribute_count?dom_attr(n,"id"):"";unsigned id_key=*id?hash(id,strlen(id),0):0;
        if(*id)keys[k++]=feature(KEY_ID,id_key);
        int first_class=k;
        for(const char *c=n->attribute_count?dom_attr(n,"class"):"";*c&&k<40;){while(isspace((unsigned char)*c))c++;const char *end=c;while(*end&&!isspace((unsigned char)*end))end++;if(end>c)keys[k++]=feature(KEY_CLASS,hash(c,(size_t)(end-c),0));c=end;}
        if(f->count+k>capacity||class_total+k>capacity){
            capacity=capacity*2+k;
            unsigned *grown=realloc(f->keys,(size_t)capacity*sizeof(unsigned));if(!grown){features_free(f);return NULL;}f->keys=grown;
            if(per_node){grown=realloc(f->classes,(size_t)capacity*sizeof(unsigned));if(!grown){features_free(f);return NULL;}f->classes=grown;}
        }
        memcpy(f->keys+f->count,keys,(size_t)k*sizeof(unsigned));f->count+=k;
        if(per_node){
            f->tag[i]=tag;f->id[i]=id_key;f->class_first[i]=class_total;
            /* class keys without their kind bits, as compiled compounds hash them */
            for(int j=first_class;j<k;j++)f->classes[class_total++]=keys[j]&~3u;
            f->class_count[i]=(unsigned char)(k-first_class);
        }
    }
    qsort(f->keys,(size_t)f->count,sizeof(unsigned),compare_keys);
    int unique=0;for(int i=0;i<f->count;i++)if(!unique||f->keys[unique-1]!=f->keys[i])f->keys[unique++]=f->keys[i];
    f->count=unique;
    return f;
}
static void features_free(css_features *f){if(f){free(f->keys);free(f->tag);free(f->id);free(f->classes);free(f->class_first);free(f->class_count);free(f);}}
static int features_have(const css_features *f,unsigned kind,unsigned h)
{
    unsigned key=feature(kind,h);int low=0,high=f->count;
    while(low<high){int mid=low+(high-low)/2;if(f->keys[mid]<key)low=mid+1;else high=mid;}
    return low<f->count&&f->keys[low]==key;
}
/* The subject compound's index key and, with features, whether every id,
   class and tag it needs is in the page. */
/* A selector the page view never matches: its subject is a pseudo-element
   (::before), or it needs a state no element has (:hover, :focus...). */
static int never(const char *s)
{
    static const char *const states[]={"hover","focus","active","visited","focus-visible","focus-within","target","target-within",
                                       "user-invalid","user-valid","autofill","-webkit-autofill","fullscreen","modal","popover-open",NULL};
    char name[32];const char *p;
    while(*s){
        if(*s=='\\'&&s[1]){s+=2;continue;}
        if(*s=='['){s=group_end(s);continue;}
        if(*s=='"'||*s=='\''){char q=*s++;while(*s&&*s!=q){if(*s=='\\'&&s[1])s++;s++;}if(*s)s++;continue;}
        if(*s==':'){
            int pe=s[1]==':';s+=pe?2:1;p=s;ident(&p,name,sizeof(name));s=p;
            if(pe||pseudo_element(name))return 1;
            for(int i=0;states[i];i++)if(eq(name,states[i]))return 1;
            if(*s=='(')s=group_end(s);   /* :not(:hover) can match */
            continue;
        }
        s++;
    }
    return 0;
}
static int present(const char *s,const char *e,const css_features *f,int depth);
/* Whether any selector of a :where(), :is() or :matches() list can match. */
static int any_present(const char *s,const char *e,const css_features *f,int depth)
{
    while(s<e){
        const char *end=s;
        while(end<e&&*end!=','){if(*end=='\\'&&end+1<e){end+=2;continue;}if(*end=='('||*end=='['){end=group_end(end);continue;}end++;}
        char one[256];size_t n=(size_t)(end-s);
        if(n>=sizeof(one))return 1;
        memcpy(one,s,n);one[n]=0;
        part parts[PARTS_MAX];int count=split(trim(one),parts),all=count>0;
        for(int k=0;k<count&&all;k++)all=present(parts[k].start,parts[k].end,f,depth+1);
        if(all||count<0)return 1;
        s=end<e?end+1:e;
    }
    return 0;
}
/* Whether the page has every id, class and tag a compound needs. */
static int present(const char *s,const char *e,const css_features *f,int depth)
{
    char name[128];const char *p;
    if(!f)return 1;
    if(*s=='*')s++;
    else if(ident_start(s)){p=s;size_t k=ident(&p,name,sizeof(name));s=p;if(*s!='|'&&!features_have(f,KEY_TAG,hash(name,k,1)))return 0;}
    while(s<e){
        if(*s=='#'||*s=='.'){int is_id=*s=='#';s++;p=s;size_t k=ident(&p,name,sizeof(name));s=p;if(!features_have(f,is_id?KEY_ID:KEY_CLASS,hash(name,k,0)))return 0;}
        else if(*s=='[')s=group_end(s);
        else if(*s==':'){
            s++;if(*s==':')s++;p=s;ident(&p,name,sizeof(name));s=p;
            if(*s=='('){
                const char *end=group_end(s);
                if(depth<4&&(eq(name,"where")||eq(name,"is")||eq(name,"matches"))&&!any_present(s+1,end-1,f,depth))return 0;
                s=end;
            }
        }
        else s++;
    }
    return 1;
}
/* The index key of a compound: its id, else a class, else its tag; failing
   those, the key of the one selector in a :where(), :is() or :matches(). */
static void compound_key(const char *s,const char *e,const css_features *f,unsigned char *kind,unsigned *key,int depth)
{
    char name[128];const char *p,*single=NULL,*single_end=NULL;
    unsigned tag=0,class_key=0,id=0;
    *kind=KEY_ANY;*key=0;
    if(*s=='*')s++;
    else if(ident_start(s)){p=s;size_t k=ident(&p,name,sizeof(name));s=p;if(*s!='|')tag=hash(name,k,1);}
    while(s<e){
        if(*s=='#'||*s=='.'){int is_id=*s=='#';s++;p=s;size_t k=ident(&p,name,sizeof(name));s=p;unsigned h=hash(name,k,0);if(is_id&&!id)id=h;else if(!is_id&&!class_key)class_key=h;}
        else if(*s=='[')s=group_end(s);
        else if(*s==':'){
            s++;if(*s==':')s++;p=s;ident(&p,name,sizeof(name));s=p;
            if(*s=='('){
                const char *end=group_end(s);
                if(!single&&(eq(name,"where")||eq(name,"is")||eq(name,"matches"))){
                    int comma=0;for(const char *q=s+1;q<end-1;){if(*q=='('||*q=='['){q=group_end(q);continue;}if(*q==','){comma=1;break;}q++;}
                    if(!comma){single=s+1;single_end=end-1;}
                }
                s=end;
            }
        }
        else s++;
    }
    if(id){*kind=KEY_ID;*key=id;}else if(class_key){*kind=KEY_CLASS;*key=class_key;}else if(tag){*kind=KEY_TAG;*key=tag;}
    else if(single&&depth<4){
        char inner[256];size_t n=(size_t)(single_end-single);
        if(n<sizeof(inner)){
            memcpy(inner,single,n);inner[n]=0;
            part parts[PARTS_MAX];int count=split(trim(inner),parts);
            if(count>0)compound_key(parts[count-1].start,parts[count-1].end,f,kind,key,depth+1);
        }
    }
}
/* A selector's index key; 0 when it can't match the page (`f`): a
   compound needs an id, class or tag the page doesn't have, or never() holds. */
static int subject(const char *selector,const css_features *f,unsigned char *kind,unsigned *key)
{
    part parts[PARTS_MAX];int n=split(selector,parts);
    *kind=KEY_ANY;*key=0;
    if(n<=0||never(selector))return 0;
    for(int k=0;k<n;k++)if(!present(parts[k].start,parts[k].end,f,0))return 0;
    compound_key(parts[n-1].start,parts[n-1].end,f,kind,key,0);
    if(f&&*kind!=KEY_ANY&&!features_have(f,*kind,*key))return 0;
    return 1;
}

/* ---- @media and @supports ---- */

static float media_length(const char *v)
{
    char *end;float x=strtof(v,&end);while(isspace((unsigned char)*end))end++;
    if(same(end,"rem",3)||same(end,"em",2))return x*16;
    return x;
}
static float media_ratio(const char *v){char *end;float x=strtof(v,&end);while(isspace((unsigned char)*end)||*end=='/')end++;float y=strtof(end,NULL);return y>0?x/y:x;}
static float media_resolution(const char *v)
{
    char *end;float x=strtof(v,&end);while(isspace((unsigned char)*end))end++;
    if(same(end,"dpi",3))return x/96;
    if(same(end,"dpcm",4))return x*2.54f/96;
    return x;
}
static int media_value(const char *name,float *out)
{
    if(eq(name,"width")||eq(name,"device-width")){*out=CSS_VIEWPORT_WIDTH;return 1;}
    if(eq(name,"height")||eq(name,"device-height")){*out=CSS_VIEWPORT_HEIGHT;return 1;}
    if(eq(name,"aspect-ratio")||eq(name,"device-aspect-ratio")){*out=(float)CSS_VIEWPORT_WIDTH/CSS_VIEWPORT_HEIGHT;return 1;}
    if(eq(name,"resolution")){*out=1;return 1;}
    return 0;
}
/* A value compared with a feature, in the feature's unit. */
static float feature_number(const char *name,const char *v)
{
    if(strstr(name,"ratio"))return media_ratio(v);
    if(strstr(name,"resolution"))return media_resolution(v);
    return media_length(v);
}
static int compare(float a,const char *op,float b)
{
    if(!strcmp(op,"<"))return a<b;
    if(!strcmp(op,"<="))return a<=b;
    if(!strcmp(op,">"))return a>b;
    if(!strcmp(op,">="))return a>=b;
    return fabsf(a-b)<0.5f;
}
/* One parenthesized feature: "min-width: 768px", "width >= 40rem",
   "400px <= width < 900px", "prefers-color-scheme: dark", "hover". */
static int media_feature(const char *s,size_t n)
{
    char text[160];if(n>=sizeof(text))return 0;
    for(size_t i=0;i<n;i++)text[i]=(char)tolower((unsigned char)s[i]);
    text[n]=0;
    char *f=trim(text);
    if(!strncmp(f,"not ",4))return !media_feature(f+4,strlen(f+4));
    if(*f=='(')return 0;
    char *colon=strchr(f,':'),*op=strpbrk(f,"<>=");
    if(colon&&(!op||colon<op)){
        *colon=0;char *name=trim(f),*v=trim(colon+1);float x;
        if(!strncmp(name,"min-",4)&&media_value(name+4,&x))return x>=feature_number(name,v);
        if(!strncmp(name,"max-",4)&&media_value(name+4,&x))return x<=feature_number(name,v);
        if(media_value(name,&x))return fabsf(x-feature_number(name,v))<0.01f*(x>2?50:1);
        if(eq(name,"orientation"))return eq(v,"landscape");
        if(eq(name,"prefers-color-scheme"))return eq(v,"light");
        if(eq(name,"prefers-reduced-motion")||eq(name,"prefers-reduced-transparency")||eq(name,"prefers-reduced-data"))return eq(v,"reduce");
        if(eq(name,"prefers-contrast")||eq(name,"forced-colors")||eq(name,"inverted-colors"))return eq(v,"no-preference")||eq(v,"none");
        if(eq(name,"hover")||eq(name,"any-hover"))return eq(v,"hover");
        if(eq(name,"pointer")||eq(name,"any-pointer"))return eq(v,"fine");
        if(strstr(name,"resolution")||strstr(name,"pixel-ratio")){float r=media_resolution(v);return !strncmp(name,"max",3)||strstr(name,"-max")?r>=1:!strncmp(name,"min",3)||strstr(name,"-min")?r<=1:fabsf(r-1)<0.01f;}
        if(eq(name,"display-mode"))return eq(v,"browser");
        if(eq(name,"scripting"))return eq(v,"enabled");
        if(eq(name,"color-gamut"))return eq(v,"srgb");
        if(eq(name,"update"))return eq(v,"fast");
        if(eq(name,"color"))return atoi(v)<=8;
        return 0;
    }
    if(!op){
        return eq(f,"color")||eq(f,"hover")||eq(f,"any-hover")||eq(f,"pointer")||eq(f,"any-pointer")||eq(f,"width")||eq(f,"height");
    }
    /* Range syntax: "width >= 40rem", "400px <= width < 900px". */
    char parts[5][48];int k=0;
    for(char *q=f;*q&&k<5;){
        while(isspace((unsigned char)*q))q++;
        if(!*q)break;
        size_t m=0;
        if(*q=='<'||*q=='>'||*q=='='){parts[k][m++]=*q++;if(*q=='=')parts[k][m++]=*q++;}
        else while(*q&&*q!='<'&&*q!='>'&&*q!='='&&m<sizeof(parts[0])-1)parts[k][m++]=*q++;
        while(m&&isspace((unsigned char)parts[k][m-1]))m--;
        parts[k][m]=0;k++;
    }
    if(k!=3&&k!=5)return 0;
    int result=1;
    for(int i=0;i+2<k;i+=2){
        float left,right;const char *o=parts[i+1];
        if(media_value(parts[i],&left))right=feature_number(parts[i],parts[i+2]);
        else if(media_value(parts[i+2],&right))left=feature_number(parts[i+2],parts[i]);
        else return 0;
        result&=compare(left,o,right);
    }
    return result;
}
/* One query: [not|only] [type] [and (feature)]..., or features joined by and/or. */
static int media_query(const char *s,size_t n)
{
    const char *e=s+n;int negate=0,result=1,first=1,or=0,any=0;
    while(s<e){
        while(s<e&&isspace((unsigned char)*s))s++;
        if(s>=e)break;
        if(*s=='('){
            const char *end=s;int depth=0;
            for(;end<e;end++){if(*end=='(')depth++;else if(*end==')'&&--depth==0)break;}
            int value=media_feature(s+1,(size_t)(end-s-1));
            if(or)result=result||value;else result=result&&value;
            any=1;first=0;s=end<e?end+1:e;continue;
        }
        const char *w=s;while(s<e&&!isspace((unsigned char)*s)&&*s!='(')s++;
        size_t k=(size_t)(s-w);
        if(k==3&&same(w,"not",3)&&first){negate=1;continue;}
        if(k==4&&same(w,"only",4))continue;
        if(k==3&&same(w,"and",3)){or=0;continue;}
        if(k==2&&same(w,"or",2)){or=1;continue;}
        int type=(k==3&&same(w,"all",3))||(k==6&&same(w,"screen",6));
        result=result&&type;any=1;first=0;
    }
    if(!any)return !negate;
    return negate?!result:result;
}
int css_media_matches(const char *list)
{
    if(!list||!*list)return 1;
    const char *s=list;
    for(;;){
        const char *end=s;int depth=0;
        while(*end&&(depth||*end!=',')){if(*end=='(')depth++;else if(*end==')')depth--;end++;}
        size_t n=(size_t)(end-s);
        while(n&&isspace((unsigned char)s[0])){s++;n--;}
        if(n&&media_query(s,n))return 1;
        if(!*end)return 0;
        s=end+1;
    }
}
/* @supports: everything but "not (...)" is taken as supported. */
static int supports(const char *s,size_t n){while(n&&isspace((unsigned char)*s)){s++;n--;}return !(n>=3&&same(s,"not",3)&&(n==3||isspace((unsigned char)s[3])||s[3]=='('));}

/* ---- stylesheets ---- */

typedef struct walker walker;
struct walker {
    void (*rule)(walker *,const char *selectors,size_t selectors_length,const char *body,size_t body_length);
    /* @property --name {initial-value: ...}: a custom property's default */
    void (*property)(walker *,const char *name,size_t name_length,const char *body,size_t body_length);
    int omitted;
};
static size_t skip_comment(const char *text,size_t length,size_t pos)
{
    pos+=2;while(pos+1<length&&(text[pos]!='*'||text[pos+1]!='/'))pos++;
    return pos+2<length?pos+2:length;
}
/* Walks the style rules that apply: @media blocks that match the page view,
   @supports and @layer blocks. Other at-rules (@font-face, @keyframes,
   @import, @container...) don't affect the page view. */
static void walk(walker *w,const char *text,size_t length,int nesting)
{
    if(nesting>8){w->omitted=1;return;}
    size_t pos=0;
    while(pos<length){
        while(pos<length&&isspace((unsigned char)text[pos]))pos++;
        if(pos+1<length&&text[pos]=='/'&&text[pos+1]=='*'){pos=skip_comment(text,length,pos);continue;}
        if(length-pos>=4&&!memcmp(text+pos,"<!--",4)){pos+=4;continue;}
        if(length-pos>=3&&!memcmp(text+pos,"-->",3)){pos+=3;continue;}
        if(pos>=length)break;
        size_t begin=pos;char quote=0;int paren=0;
        while(pos<length){
            char c=text[pos];
            if(quote){if(c=='\\'){pos+=2;continue;}if(c==quote)quote=0;}
            else if(c=='"'||c=='\'')quote=c;
            else if(c=='(')paren++;
            else if(c==')')paren--;
            else if(c=='/'&&pos+1<length&&text[pos+1]=='*'){pos=skip_comment(text,length,pos);continue;}
            else if(paren<=0&&(c=='{'||c==';'||c=='}'))break;
            pos++;
        }
        if(pos>=length)break;
        if(text[pos]==';'||text[pos]=='}'){pos++;continue;}
        size_t prelude_end=pos;pos++;
        size_t body=pos;int depth=1;quote=0;
        while(pos<length&&depth){
            char c=text[pos];
            if(quote){if(c=='\\'){pos+=2;continue;}if(c==quote)quote=0;}
            else if(c=='"'||c=='\'')quote=c;
            else if(c=='/'&&pos+1<length&&text[pos+1]=='*'){pos=skip_comment(text,length,pos);continue;}
            else if(c=='{')depth++;
            else if(c=='}')depth--;
            if(depth)pos++;
        }
        size_t body_length=(pos<length?pos:length)-body;
        if(pos<length)pos++;
        const char *prelude=text+begin;size_t prelude_length=prelude_end-begin;
        while(prelude_length&&isspace((unsigned char)prelude[prelude_length-1]))prelude_length--;
        if(*prelude=='@'){
            const char *name=prelude+1,*rest=name;
            while(rest<prelude+prelude_length&&(isalnum((unsigned char)*rest)||*rest=='-'))rest++;
            size_t name_length=(size_t)(rest-name),rest_length=(size_t)(prelude+prelude_length-rest);
            if(name_length==5&&same(name,"media",5)){
                char query[512];
                if(rest_length<sizeof(query)){memcpy(query,rest,rest_length);query[rest_length]=0;if(css_media_matches(query))walk(w,text+body,body_length,nesting+1);}
                else w->omitted=1;
            }
            else if(name_length==8&&same(name,"supports",8)){if(supports(rest,rest_length))walk(w,text+body,body_length,nesting+1);}
            else if(name_length==5&&same(name,"layer",5))walk(w,text+body,body_length,nesting+1);
            else if(name_length==8&&same(name,"property",8)&&w->property){
                while(rest_length&&isspace((unsigned char)*rest)){rest++;rest_length--;}
                w->property(w,rest,rest_length,text+body,body_length);
            }
            continue;
        }
        w->rule(w,prelude,prelude_length,text+body,body_length);
    }
}
/* Calls `each` for every selector of a list, split at top-level commas. */
static void each_selector(const char *s,size_t n,void (*each)(void *,const char *,size_t),void *ud)
{
    const char *e=s+n;
    while(s<e){
        const char *end=s;char quote=0;int depth=0;
        for(;end<e;end++){
            if(*end=='\\'&&end+1<e){end++;continue;}
            if(quote){if(*end==quote)quote=0;continue;}
            if(*end=='"'||*end=='\'')quote=*end;
            else if(*end=='('||*end=='[')depth++;
            else if(*end==')'||*end==']')depth--;
            else if(*end==','&&depth<=0)break;
        }
        const char *a=s,*b=end;
        while(a<b&&isspace((unsigned char)*a))a++;
        while(b>a&&isspace((unsigned char)b[-1]))b--;
        if(b>a)each(ud,a,(size_t)(b-a));
        s=end<e?end+1:e;
    }
}

/* Text of kept rules. */
static int store(browser_css *css,const char *s,size_t n)
{
    if(css->used+n+1>CSS_TEXT_MAX){css->omitted=1;return -1;}
    if(css->used+n+1>css->size){
        size_t size=css->size?css->size:16*1024;
        while(size<css->used+n+1)size*=2;
        if(size>CSS_TEXT_MAX)size=CSS_TEXT_MAX;
        char *text=realloc(css->text,size);if(!text){css->omitted=1;return -1;}
        css->text=text;css->size=size;
    }
    int at=(int)css->used;memcpy(css->text+at,s,n);css->text[at+n]=0;css->used+=n+1;
    return at;
}
/* Custom properties of rules for the root or body element: the values
   var() falls back to when an element's own rules don't define them. */
struct css_variable { unsigned key; int name, value; };
static int root_rule(const browser_css *css,const char *selector)
{
    int roots=0;
    for(int i=0;css->dom&&i<css->dom->count&&i<64;i++){
        const dom_node *n=&css->dom->nodes[i];
        if(!eq(n->tag,"html")&&!eq(n->tag,"body"))continue;
        roots++;
        if(css_matches(css->dom,i,selector))return 1;
    }
    /* A fragment without <html> or <body>: by the selector's text. */
    return !roots&&(eq(selector,":root")||eq(selector,"html")||eq(selector,"body")||eq(selector,":host")||eq(selector,"*"));
}
/* Calls `each` with every "name: value" declaration of a body. */
static void each_declaration(const char *s,size_t n,void (*each)(void *,const char *,size_t,const char *,size_t),void *ud)
{
    const char *e=s+n;
    while(s<e){
        const char *end=s;char quote=0;int depth=0;
        for(;end<e;end++){
            if(quote){if(*end=='\\'&&end+1<e){end++;continue;}if(*end==quote)quote=0;continue;}
            if(*end=='"'||*end=='\'')quote=*end;
            else if(*end=='('||*end=='['||*end=='{')depth++;
            else if(*end==')'||*end==']'||*end=='}')depth--;
            else if(*end==';'&&depth<=0)break;
        }
        const char *colon=s;while(colon<end&&*colon!=':')colon++;
        if(colon<end){
            const char *k=s,*ke=colon,*v=colon+1,*ve=end;
            while(k<ke&&isspace((unsigned char)*k))k++;
            while(ke>k&&isspace((unsigned char)ke[-1]))ke--;
            while(v<ve&&isspace((unsigned char)*v))v++;
            while(ve>v&&isspace((unsigned char)ve[-1]))ve--;
            if(ke>k)each(ud,k,(size_t)(ke-k),v,(size_t)(ve-v));
        }
        s=end<e?end+1:e;
    }
}
static void add_variable(void *ud,const char *k,size_t kn,const char *v,size_t vn)
{
    browser_css *css=ud;
    if(kn<3||k[0]!='-'||k[1]!='-'||vn>=1024)return;
    unsigned key=hash(k,kn,0);
    for(int i=css->variable_count-1;i>=0;i--)
        if(css->variables[i].key==key&&strlen(css->text+css->variables[i].name)==kn&&!memcmp(css->text+css->variables[i].name,k,kn)){
            int value=store(css,v,vn);if(value>=0)css->variables[i].value=value;return;
        }
    if(css->variable_count%256==0){css_variable *grown=realloc(css->variables,(size_t)(css->variable_count+256)*sizeof(*grown));if(!grown)return;css->variables=grown;}
    int name=store(css,k,kn),value=store(css,v,vn);
    if(name<0||value<0)return;
    css->variables[css->variable_count++]=(css_variable){key,name,value};
}
/* Compiles a stored selector's compounds; returns the first one's index. */
static int compile(browser_css *css,const char *selector)
{
    part parts[PARTS_MAX];int n=split(selector,parts);
    if(n<=0)return -1;
    if(css->compound_count+n>css->compound_capacity){
        int capacity=css->compound_capacity?css->compound_capacity*2:512;
        while(capacity<css->compound_count+n)capacity*=2;
        css_compound *grown=realloc(css->compounds,(size_t)capacity*sizeof(*grown));if(!grown){css->omitted=1;return -1;}
        css->compounds=grown;css->compound_capacity=capacity;
    }
    int first=css->compound_count;
    for(int k=0;k<n;k++){
        css_compound *c=&css->compounds[first+k];memset(c,0,sizeof(*c));
        c->start=(short)(parts[k].start-selector);c->end=(short)(parts[k].end-selector);c->combinator=(unsigned char)parts[k].combinator;
        const char *s=parts[k].start,*e=parts[k].end,*p;char name[128];
        if(*s=='*')s++;
        else if(ident_start(s)){p=s;size_t m=ident(&p,name,sizeof(name));s=p;if(*s=='|')c->rest=1;else c->tag=hash(name,m,1);}
        while(s<e&&!c->rest){
            if(*s=='#'&&!c->id){s++;p=s;size_t m=ident(&p,name,sizeof(name));s=p;c->id=hash(name,m,0);}
            else if(*s=='.'&&c->class_count<3){s++;p=s;size_t m=ident(&p,name,sizeof(name));s=p;c->classes[c->class_count++]=hash(name,m,0)&~3u;}
            else c->rest=1;
        }
    }
    css->compound_count+=n;
    return first;
}
typedef struct { browser_css *css; int declarations, specificity_base, variables; const char *body; size_t body_length; } adding;
static void add_selector(void *ud,const char *s,size_t n)
{
    adding *a=ud;browser_css *css=a->css;
    char selector[512];if(n>=sizeof(selector)){css->omitted=1;return;}
    memcpy(selector,s,n);selector[n]=0;
    unsigned char kind;unsigned key;
    if(!subject(selector,css->features,&kind,&key))return;
    if(css->count>=CSS_RULES_MAX){css->omitted=1;return;}
    if(css->count==css->capacity){
        int capacity=css->capacity?css->capacity*2:256;
        css_rule *rules=realloc(css->rules,(size_t)capacity*sizeof(*rules));if(!rules){css->omitted=1;return;}
        css->rules=rules;css->capacity=capacity;
    }
    if(a->declarations<0&&(a->declarations=store(css,a->body,a->body_length))<0)return;
    int at=store(css,selector,n);if(at<0)return;
    int compound=compile(css,css->text+at);if(compound<0)return;
    css_rule *r=&css->rules[css->count];
    r->selector=at;r->declarations=a->declarations;r->specificity=specificity(selector,selector+n);r->order=css->count;
    r->kind=kind;r->key=key;r->variables=(unsigned char)a->variables;
    r->compound=compound;r->compounds=(unsigned char)(css->compound_count-compound);
    css->count++;
    if(a->variables&&root_rule(css,selector))each_declaration(a->body,a->body_length,add_variable,css);
}
static int defines_variables(const char *s,size_t n)
{
    for(size_t i=0;i+2<n;i++)if(s[i]=='-'&&s[i+1]=='-'&&(i==0||s[i-1]==';'||s[i-1]=='{'||isspace((unsigned char)s[i-1])))return 1;
    return 0;
}
/* An @property's initial value, unless a rule already defined the variable. */
typedef struct { browser_css *css; const char *name; size_t name_length; } property_default;
static void initial_value(void *ud,const char *k,size_t kn,const char *v,size_t vn)
{
    property_default *d=ud;browser_css *css=d->css;
    if(kn!=13||!same(k,"initial-value",13))return;
    unsigned key=hash(d->name,d->name_length,0);
    for(int i=0;i<css->variable_count;i++)
        if(css->variables[i].key==key&&strlen(css->text+css->variables[i].name)==d->name_length&&!memcmp(css->text+css->variables[i].name,d->name,d->name_length))return;
    add_variable(css,d->name,d->name_length,v,vn);
}
typedef struct { walker base; browser_css *css; } store_walker;
static void store_property(walker *w,const char *name,size_t name_length,const char *body,size_t body_length)
{
    property_default d={((store_walker *)w)->css,name,name_length};
    if(name_length>=3&&name[0]=='-'&&name[1]=='-'&&body_length<1024)each_declaration(body,body_length,initial_value,&d);
}
static void store_rule(walker *w,const char *selectors,size_t selectors_length,const char *body,size_t body_length)
{
    browser_css *css=((store_walker *)w)->css;
    if(selectors_length>=8192||body_length>=32*1024){css->omitted=1;return;}
    adding a={css,-1,0,defines_variables(body,body_length),body,body_length};
    each_selector(selectors,selectors_length,add_selector,&a);
}
typedef struct { unsigned key; int kind, rule; } index_entry;
static int by_key(const void *x,const void *y)
{
    const index_entry *a=x,*b=y;
    if(a->kind!=b->kind)return a->kind<b->kind?-1:1;
    if(a->key!=b->key)return a->key<b->key?-1:1;
    return a->rule-b->rule;
}
/* Rule numbers sorted by (kind, key), then source order. */
static int index_rules(browser_css *css)
{
    free(css->index);css->index=NULL;
    if(!css->count)return 0;
    index_entry *entries=malloc((size_t)css->count*sizeof(*entries));
    css->index=malloc((size_t)css->count*sizeof(int));
    if(!entries||!css->index){free(entries);free(css->index);css->index=NULL;css->omitted=1;return -1;}
    for(int i=0;i<css->count;i++)entries[i]=(index_entry){css->rules[i].key,css->rules[i].kind,i};
    qsort(entries,(size_t)css->count,sizeof(*entries),by_key);
    for(int i=0;i<css->count;i++)css->index[i]=entries[i].rule;
    free(entries);
    return 0;
}
void css_free(browser_css *css)
{
    free(css->rules);free(css->text);free(css->index);free(css->variables);free(css->compounds);features_free(css->features);
    const browser_dom *dom=css->dom;memset(css,0,sizeof(*css));css->dom=dom;
}
int css_add(browser_css *css,const char *text,size_t length)
{
    if(length>CSS_SHEET_MAX){css->omitted=1;return -1;}
    if(css->dom&&!css->features)css->features=features_build(css->dom,1);
    css->bytes+=length;
    store_walker w={{store_rule,store_property,0},css};
    walk(&w.base,text,length,0);
    if(w.base.omitted)css->omitted=1;
    return index_rules(css);
}

/* Compacting: the rules whose selectors can match the page, as CSS text. */
typedef struct { walker base; css_features *features; char *out; size_t used, size; int failed, kept; } compact_walker;
static int append(compact_walker *c,const char *s,size_t n)
{
    if(c->used+n+1>CSS_TEXT_MAX){c->base.omitted=1;return -1;}
    if(c->used+n+1>c->size){
        size_t size=c->size?c->size:4096;while(size<c->used+n+1)size*=2;if(size>CSS_TEXT_MAX)size=CSS_TEXT_MAX;
        char *out=realloc(c->out,size);if(!out){c->failed=1;return -1;}
        c->out=out;c->size=size;
    }
    memcpy(c->out+c->used,s,n);c->used+=n;c->out[c->used]=0;return 0;
}
static void keep_selector(void *ud,const char *s,size_t n)
{
    compact_walker *c=ud;char selector[512];
    if(n>=sizeof(selector)||c->failed)return;
    memcpy(selector,s,n);selector[n]=0;
    unsigned char kind;unsigned key;
    if(!subject(selector,c->features,&kind,&key))return;
    if(c->kept)append(c,",",1);
    append(c,s,n);c->kept++;
}
static void compact_rule(walker *w,const char *selectors,size_t selectors_length,const char *body,size_t body_length)
{
    compact_walker *c=(compact_walker *)w;
    if(selectors_length>=8192||body_length>=32*1024||c->failed){c->base.omitted=1;return;}
    size_t mark=c->used;c->kept=0;
    each_selector(selectors,selectors_length,keep_selector,c);
    if(!c->kept){c->used=mark;if(c->out)c->out[mark]=0;return;}
    if(append(c,"{",1)<0||append(c,body,body_length)<0||append(c,"}\n",2)<0){c->used=mark;if(c->out)c->out[mark]=0;}
}
static void compact_property(walker *w,const char *name,size_t name_length,const char *body,size_t body_length)
{
    compact_walker *c=(compact_walker *)w;
    if(c->failed||name_length>=128||body_length>=1024)return;
    size_t mark=c->used;
    if(append(c,"@property ",10)<0||append(c,name,name_length)<0||append(c,"{",1)<0||append(c,body,body_length)<0||append(c,"}\n",2)<0){c->used=mark;if(c->out)c->out[mark]=0;}
}
char *css_compact(const char *text,size_t length,const browser_dom *dom,size_t *out_length)
{
    compact_walker c;memset(&c,0,sizeof(c));c.base.rule=compact_rule;c.base.property=compact_property;
    c.features=dom?features_build(dom,0):NULL;
    walk(&c.base,text,length,0);
    features_free(c.features);
    if(c.failed){free(c.out);return NULL;}
    if(!c.out&&!(c.out=calloc(1,1)))return NULL;
    *out_length=c.used;return c.out;
}

/* ---- values ---- */

/* Colors are 0xAABBGGRR for the PSP's GU. Mostly transparent colors give 0
   (no color); unknown values (gradients, color-mix()) leave it unchanged. */
static unsigned channel(float v){return v<=0?0:v>=255?255:(unsigned)(v+0.5f);}
static float srgb(float linear){float c=linear<=0.0031308f?12.92f*linear:1.055f*powf(linear,1/2.4f)-0.055f;return c*255;}
static int function_color(const char *v,unsigned *r,unsigned *g,unsigned *b,float *alpha)
{
    char name[16];size_t k=0;
    while(*v&&*v!='('&&k<sizeof(name)-1)name[k++]=(char)tolower((unsigned char)*v++);
    name[k]=0;if(*v!='(')return 0;v++;
    float x[4];char unit[4][6];int n=0;
    while(*v&&*v!=')'&&n<4){
        while(isspace((unsigned char)*v)||*v==','||*v=='/')v++;
        if(*v==')'||!*v)break;
        char *end;x[n]=strtof(v,&end);
        if(end==v){if(same(v,"none",4)){x[n]=0;end=(char *)v+4;}else return 0;}
        size_t u=0;while(isalpha((unsigned char)*end)||*end=='%'){if(u<5)unit[n][u++]=(char)tolower((unsigned char)*end);end++;}
        unit[n][u]=0;n++;v=end;
    }
    if(n<3)return 0;
    *alpha=n>3?(unit[3][0]=='%'?x[3]/100:x[3]):1;
    if(!strcmp(name,"rgb")||!strcmp(name,"rgba")){
        for(int i=0;i<3;i++)if(unit[i][0]=='%')x[i]*=2.55f;
        *r=channel(x[0]);*g=channel(x[1]);*b=channel(x[2]);return 1;
    }
    if(!strcmp(name,"hsl")||!strcmp(name,"hsla")){
        float h=x[0];if(!strcmp(unit[0],"turn"))h*=360;else if(!strcmp(unit[0],"rad"))h*=57.2958f;else if(!strcmp(unit[0],"grad"))h*=0.9f;
        float s=x[1]/100,l=x[2]/100;if(s<0)s=0;if(s>1)s=1;if(l<0)l=0;if(l>1)l=1;
        h=fmodf(h,360);if(h<0)h+=360;
        float c=(1-fabsf(2*l-1))*s,hp=h/60,xx=c*(1-fabsf(fmodf(hp,2)-1)),m=l-c/2,rr=0,gg=0,bb=0;
        if(hp<1){rr=c;gg=xx;}else if(hp<2){rr=xx;gg=c;}else if(hp<3){gg=c;bb=xx;}else if(hp<4){gg=xx;bb=c;}else if(hp<5){rr=xx;bb=c;}else{rr=c;bb=xx;}
        *r=channel((rr+m)*255);*g=channel((gg+m)*255);*b=channel((bb+m)*255);return 1;
    }
    if(!strcmp(name,"oklch")||!strcmp(name,"oklab")){
        float L=unit[0][0]=='%'?x[0]/100:x[0],A,B;
        if(!strcmp(name,"oklch")){
            float C=unit[1][0]=='%'?x[1]*0.004f:x[1],h=x[2];
            if(!strcmp(unit[2],"turn"))h*=360;else if(!strcmp(unit[2],"rad"))h*=57.2958f;
            A=C*cosf(h*3.14159265f/180);B=C*sinf(h*3.14159265f/180);
        }else{A=unit[1][0]=='%'?x[1]*0.004f:x[1];B=unit[2][0]=='%'?x[2]*0.004f:x[2];}
        float l_=L+0.3963377774f*A+0.2158037573f*B,m_=L-0.1055613458f*A-0.0638541728f*B,s_=L-0.0894841775f*A-1.2914855480f*B;
        float l3=l_*l_*l_,m3=m_*m_*m_,s3=s_*s_*s_;
        *r=channel(srgb(4.0767416621f*l3-3.3077115913f*m3+0.2309699292f*s3));
        *g=channel(srgb(-1.2684380046f*l3+2.6097574011f*m3-0.3413193965f*s3));
        *b=channel(srgb(-0.0041960863f*l3-0.7034186147f*m3+1.7076147010f*s3));
        return 1;
    }
    return 0;
}
static int color(const char *v,uint32_t *out)
{
    unsigned r,g,b,a=255;float alpha=1;size_t n=strlen(v);
    if(*v=='#'&&(n==7||n==9)&&sscanf(v+1,"%2x%2x%2x",&r,&g,&b)==3){if(n==9&&sscanf(v+7,"%2x",&a)!=1)return 0;}
    else if(*v=='#'&&(n==4||n==5)&&sscanf(v+1,"%1x%1x%1x",&r,&g,&b)==3){r*=17;g*=17;b*=17;if(n==5){if(sscanf(v+4,"%1x",&a)!=1)return 0;a*=17;}}
    else if(strchr(v,'(')){if(!function_color(v,&r,&g,&b,&alpha))return 0;a=alpha<0.3f?0:255;}
    else {
        static const struct{const char *name;uint32_t rgb;} names[]={
            {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},{"blue",0x0000ff},{"gray",0x808080},{"grey",0x808080},
            {"yellow",0xffff00},{"navy",0x000080},{"purple",0x800080},{"orange",0xffa500},{"silver",0xc0c0c0},{"maroon",0x800000},
            {"olive",0x808000},{"lime",0x00ff00},{"aqua",0x00ffff},{"cyan",0x00ffff},{"teal",0x008080},{"fuchsia",0xff00ff},
            {"magenta",0xff00ff},{"lightgray",0xd3d3d3},{"lightgrey",0xd3d3d3},{"darkgray",0xa9a9a9},{"darkgrey",0xa9a9a9},
            {"dimgray",0x696969},{"dimgrey",0x696969},{"whitesmoke",0xf5f5f5},{"gainsboro",0xdcdcdc},{"darkblue",0x00008b},{"darkred",0x8b0000},
            {"darkgreen",0x006400},{"lightblue",0xadd8e6},{"steelblue",0x4682b4},{"royalblue",0x4169e1},{"dodgerblue",0x1e90ff},
            {"skyblue",0x87ceeb},{"brown",0xa52a2a},{"pink",0xffc0cb},{"gold",0xffd700},{"beige",0xf5f5dc},{"ivory",0xfffff0},
            {"lightyellow",0xffffe0},{"crimson",0xdc143c},{"tomato",0xff6347},{"coral",0xff7f50},{"indigo",0x4b0082},
            {"violet",0xee82ee},{"tan",0xd2b48c},{"khaki",0xf0e68c},{"salmon",0xfa8072},{"lightgreen",0x90ee90},
            {"darkorange",0xff8c00},{"slategray",0x708090},{"slategrey",0x708090},{"aliceblue",0xf0f8ff},{"snow",0xfffafa},{"linen",0xfaf0e6},
            {"lavender",0xe6e6fa},{"midnightblue",0x191970},{"darkslategray",0x2f4f4f},{"darkslategrey",0x2f4f4f},{"orangered",0xff4500},
            {"seagreen",0x2e8b57},{"forestgreen",0x228b22},{"chocolate",0xd2691e},{"firebrick",0xb22222},{"rebeccapurple",0x663399},
            {"cornflowerblue",0x6495ed},{"deepskyblue",0x00bfff},{"lightskyblue",0x87cefa},{"lightsteelblue",0xb0c4de},{"powderblue",0xb0e0e6},
            {"darkcyan",0x008b8b},{"cadetblue",0x5f9ea0},{"lightcyan",0xe0ffff},{"turquoise",0x40e0d0},{"mediumseagreen",0x3cb371},
            {"limegreen",0x32cd32},{"olivedrab",0x6b8e23},{"darkolivegreen",0x556b2f},{"yellowgreen",0x9acd32},{"palegreen",0x98fb98},
            {"honeydew",0xf0fff0},{"mintcream",0xf5fffa},{"azure",0xf0ffff},{"ghostwhite",0xf8f8ff},{"floralwhite",0xfffaf0},
            {"oldlace",0xfdf5e6},{"seashell",0xfff5ee},{"cornsilk",0xfff8dc},{"lemonchiffon",0xfffacd},{"wheat",0xf5deb3},
            {"goldenrod",0xdaa520},{"darkgoldenrod",0xb8860b},{"peru",0xcd853f},{"sienna",0xa0522d},{"saddlebrown",0x8b4513},
            {"indianred",0xcd5c5c},{"lightcoral",0xf08080},{"hotpink",0xff69b4},{"deeppink",0xff1493},{"lightpink",0xffb6c1},
            {"mistyrose",0xffe4e1},{"orchid",0xda70d6},{"plum",0xdda0dd},{"thistle",0xd8bfd8},{"darkviolet",0x9400d3},
            {"blueviolet",0x8a2be2},{"mediumpurple",0x9370db},{"slateblue",0x6a5acd},{"darkslateblue",0x483d8b},{"mediumblue",0x0000cd},
            {"lightslategray",0x778899},{"lightslategrey",0x778899},{"darkkhaki",0xbdb76b},{"aquamarine",0x7fffd4},{"darkturquoise",0x00ced1},
            {"lightseagreen",0x20b2aa},{"papayawhip",0xffefd5},{"antiquewhite",0xfaebd7},{"bisque",0xffe4c4},{"peachpuff",0xffdab9}};
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
    char token[96];
    for(const char *p=v;*p;){
        while(isspace((unsigned char)*p)||*p==',')p++;
        size_t n=0;int depth=0;
        while(*p&&(depth||(!isspace((unsigned char)*p)&&*p!=','))){if(*p=='(')depth++;else if(*p==')')depth--;if(n<sizeof(token)-1)token[n++]=*p;p++;}
        token[n]=0;if(n&&color(token,out))return 1;
    }
    return 0;
}
/* A gradient drawn as one colour: the average of its stops that show
   (a dark banner stays dark behind its white text). */
static int gradient_color(const char *v,uint32_t *out)
{
    const char *p=strstr(v,"gradient(");
    if(!p)return 0;
    p+=9;
    unsigned r=0,g=0,b=0,count=0;char stop[160];
    while(*p&&*p!=')'){
        size_t n=0;int depth=0;
        while(*p&&(depth||(*p!=','&&*p!=')'))){if(*p=='(')depth++;else if(*p==')')depth--;if(n<sizeof(stop)-1)stop[n++]=*p;p++;}
        stop[n]=0;if(*p==',')p++;
        uint32_t c;
        if(shorthand_color(stop,&c)&&c){r+=c&255;g+=(c>>8)&255;b+=(c>>16)&255;count++;}
    }
    if(!count)return 0;
    *out=0xff000000u|(r/count)|((g/count)<<8)|((b/count)<<16);
    return 1;
}
/* The colour a background shorthand or image paints: a gradient's, or the
   colour layer's. */
static int background_color(const char *v,uint32_t *out)
{
    for(const char *p=v;(p=strstr(p,"gradient("))!=NULL;p+=9)if(gradient_color(p,out))return 1;
    return shorthand_color(v,out);
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
    else if(eq(end,"ch"))px=x*8;
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
static void border(browser_box *b,int side,const char *v,float scale,uint32_t current)
{
    char copy[128];if(strlen(v)>=sizeof(copy))return;strcpy(copy,v);
    int width=-1;uint32_t c=0;int has_color=0,none=0;
    for(char *p=strtok(copy," \t");p;p=strtok(NULL," \t")){
        int w;if(eq(p,"none")||eq(p,"hidden")||eq(p,"0"))none=1;
        else if(length(p,scale,0,&w,NULL)&&w!=BOX_AUTO)width=w;
        else if(eq(p,"currentcolor")){c=current;has_color=1;}
        else if(color(p,&c))has_color=1;
    }
    for(int i=0;i<4;i++)if(side<0||side==i){
        if(none)b->border[i]=0;else{b->border[i]=(unsigned char)clamp(width<0?1:width<1?1:width,0,6);if(has_color)b->border_color[i]=c;}
    }
    if(none&&side<0)b->plain|=2;
}

/* A transform that moves an element out of view: a drawer or skip link
   translated by its whole size (100%) or far away, or scaled to nothing. */
/* Whether translation amounts ("0 -200%", "-100%, 0") move by a whole size or far. */
static int far(const char *a)
{
    for(int k=0;k<3;k++){
        while(*a==' '||*a==',')a++;
        char *end;float x=strtof(a,&end);if(end==a)break;
        if(*end=='%'?fabsf(x)>=100:fabsf(x)>=500)return 1;
        a=end;while(*a&&*a!=','&&*a!=' '&&*a!=')')a++;
        if(*a==')')break;
    }
    return 0;
}
static int offscreen(const char *v)
{
    for(const char *p=strstr(v,"translate");p;p=strstr(p+9,"translate")){
        const char *a=strchr(p,'(');if(!a)break;
        if(far(a+1))return 1;
    }
    return strstr(v,"scale(0)")||strstr(v,"scaleX(0)")||strstr(v,"scaleY(0)")||strstr(v,"scale(0,")!=NULL;
}
/* var(): an element's own rules define custom properties first, then the
   root's. A declaration whose variables can't be resolved is dropped. */
#define SCOPE_MAX 96
typedef struct {
    const browser_css *css;
    const char *name[SCOPE_MAX],*value[SCOPE_MAX];
    size_t name_length[SCOPE_MAX],value_length[SCOPE_MAX];
    int count;
} scope;
static void scope_add(void *ud,const char *k,size_t kn,const char *v,size_t vn)
{
    scope *sc=ud;
    if(kn<3||k[0]!='-'||k[1]!='-')return;
    for(int i=0;i<sc->count;i++)if(sc->name_length[i]==kn&&!memcmp(sc->name[i],k,kn)){sc->value[i]=v;sc->value_length[i]=vn;return;}
    if(sc->count<SCOPE_MAX){sc->name[sc->count]=k;sc->name_length[sc->count]=kn;sc->value[sc->count]=v;sc->value_length[sc->count]=vn;sc->count++;}
}
static int lookup(const scope *sc,const char *name,size_t n,const char **value,size_t *length_out)
{
    for(int i=0;i<sc->count;i++)if(sc->name_length[i]==n&&!memcmp(sc->name[i],name,n)){*value=sc->value[i];*length_out=sc->value_length[i];return 1;}
    const browser_css *css=sc->css;unsigned key=hash(name,n,0);
    for(int i=css->variable_count-1;i>=0;i--){
        const char *k=css->text+css->variables[i].name;
        if(css->variables[i].key==key&&strlen(k)==n&&!memcmp(k,name,n)){*value=css->text+css->variables[i].value;*length_out=strlen(*value);return 1;}
    }
    return 0;
}
static int resolve(const char *v,const scope *sc,char *out,size_t size,int depth)
{
    size_t n=0;
    while(*v){
        if(same(v,"var(",4)&&sc){
            const char *p=v+4,*end=p;int nest=1;
            while(*end&&nest){if(*end=='(')nest++;else if(*end==')')nest--;if(nest)end++;}
            if(!*end)return 0;
            while(isspace((unsigned char)*p))p++;
            const char *name=p;while(p<end&&*p!=','&&!isspace((unsigned char)*p))p++;
            size_t name_length=(size_t)(p-name);
            while(p<end&&*p!=',')p++;
            const char *value;size_t value_length;char piece[512];
            if(lookup(sc,name,name_length,&value,&value_length)&&value_length<sizeof(piece)){memcpy(piece,value,value_length);piece[value_length]=0;}
            else if(p<end){size_t m=(size_t)(end-p-1);if(m>=sizeof(piece))return 0;memcpy(piece,p+1,m);piece[m]=0;}
            else return 0;
            char *t=trim(piece);
            if(strstr(t,"var(")){
                char deeper[512];
                if(depth>=6||!resolve(t,sc,deeper,sizeof(deeper),depth+1))return 0;
                strcpy(piece,deeper);t=piece;
            }
            size_t m=strlen(t);if(n+m>=size)return 0;
            memcpy(out+n,t,m);n+=m;v=end+1;continue;
        }
        if(n+1>=size)return 0;
        out[n++]=*v++;
    }
    out[n]=0;return 1;
}
static void declarations(browser_style *s,browser_box *b,const char *text,int priority,const scope *sc)
{
    if(strlen(text)>=4096)return;
    char copy[4096];strcpy(copy,text);char *p=copy;
    while(*p){char *next=p;int depth=0;char quote=0;
        for(;*next;next++){if(quote){if(*next==quote)quote=0;continue;}if(*next=='"'||*next=='\'')quote=*next;else if(*next=='(')depth++;else if(*next==')')depth--;else if(*next==';'&&depth<=0)break;}
        if(*next)*next++=0;else next=NULL;
        char *colon=strchr(p,':');if(colon){*colon++=0;char *key=trim(p),*v=trim(colon),*important=strrchr(v,'!');int is_important=important&&eq(trim(important+1),"important");if(is_important)*important=0;v=trim(v);
        if(is_important!=priority||(key[0]=='-'&&key[1]=='-')){if(!next)break;p=next;continue;}
        char resolved[512];
        if(strstr(v,"var(")){if(!resolve(v,sc,resolved,sizeof(resolved),0)){if(!next)break;p=next;continue;}v=trim(resolved);}
        uint32_t current=s->color;
        if(eq(key,"color")){if(!eq(v,"currentcolor"))color(v,&s->color);}
        else if(eq(key,"background-color")){if(eq(v,"currentcolor"))s->background=current;else if(color(v,&s->background)&&!s->background&&b)b->plain|=4;}
        else if(eq(key,"background")){if(!background_color(v,&s->background)&&eq(v,"none"))s->background=0;if(!s->background&&b&&(eq(v,"none")||eq(v,"transparent")||eq(v,"0 0")))b->plain|=4;}
        else if(eq(key,"background-image")){uint32_t c;if(gradient_color(v,&c))s->background=c;}
        else if(eq(key,"background-clip")||eq(key,"-webkit-background-clip")){if(eq(v,"text"))s->flags|=CSS_CLIP_TEXT;else s->flags&=~CSS_CLIP_TEXT;}
        else if((eq(key,"appearance")||eq(key,"-webkit-appearance")||eq(key,"-moz-appearance"))&&eq(v,"none")){if(b)b->plain|=1;}
        else if(eq(key,"display")){
            int d=eq(v,"none")?DISPLAY_NONE:eq(v,"block")||eq(v,"grid")||eq(v,"flow-root")||eq(v,"-webkit-box")?DISPLAY_BLOCK:eq(v,"flex")?DISPLAY_FLEX:
                  eq(v,"list-item")?DISPLAY_LIST_ITEM:eq(v,"table")?DISPLAY_TABLE:eq(v,"table-row")?DISPLAY_ROW:eq(v,"table-cell")?DISPLAY_CELL:
                  eq(v,"inline")||eq(v,"contents")?DISPLAY_INLINE:eq(v,"inline-block")||eq(v,"inline-flex")||eq(v,"inline-grid")||eq(v,"inline-table")?DISPLAY_INLINE_BLOCK:-1;
            if(d==DISPLAY_NONE)s->hidden=1;else if(d>=0){s->hidden=0;s->block=d!=DISPLAY_INLINE&&d!=DISPLAY_INLINE_BLOCK;}
            if(b&&d>=0)b->display=(unsigned char)d;
        }
        else if(eq(key,"visibility")&&(eq(v,"hidden")||eq(v,"collapse")))s->hidden=1;
        else if(eq(key,"content-visibility")&&eq(v,"hidden"))s->hidden=1;
        else if(eq(key,"font-weight")){if(eq(v,"bold")||eq(v,"bolder")||atoi(v)>=600)s->flags|=CSS_BOLD;else if(eq(v,"normal")||eq(v,"lighter")||(atoi(v)>0&&atoi(v)<600))s->flags&=~CSS_BOLD;}
        else if(eq(key,"font-style")){if(eq(v,"italic")||eq(v,"oblique"))s->flags|=CSS_ITALIC;else if(eq(v,"normal"))s->flags&=~CSS_ITALIC;}
        else if(eq(key,"text-decoration")||eq(key,"text-decoration-line")){
            if(eq(v,"none")||!strncmp(v,"none ",5))s->flags&=~(CSS_UNDERLINE|CSS_STRIKE);
            else{if(strstr(v,"underline"))s->flags|=CSS_UNDERLINE;if(strstr(v,"line-through"))s->flags|=CSS_STRIKE;}
        }
        else if(eq(key,"text-transform")){s->flags&=~(CSS_UPPERCASE|CSS_LOWERCASE);if(eq(v,"uppercase"))s->flags|=CSS_UPPERCASE;else if(eq(v,"lowercase"))s->flags|=CSS_LOWERCASE;}
        else if(eq(key,"white-space")||eq(key,"white-space-collapse"))s->pre=eq(v,"pre")||eq(v,"pre-wrap")||eq(v,"break-spaces")||eq(v,"pre-line")||eq(v,"preserve");
        else if(eq(key,"text-align"))s->align=eq(v,"center")||eq(v,"-webkit-center")?1:eq(v,"right")||eq(v,"end")?2:0;
        else if(eq(key,"list-style")||eq(key,"list-style-type"))s->list_none=strstr(v,"none")!=NULL;
        else if(eq(key,"font-size")||eq(key,"font")){
            char size[64]="";
            if(eq(key,"font")){char tmp[256];if(strlen(v)<sizeof(tmp)){strcpy(tmp,v);for(char *t=strtok(tmp," \t");t;t=strtok(NULL," \t")){if(eq(t,"bold"))s->flags|=CSS_BOLD;else if(eq(t,"italic"))s->flags|=CSS_ITALIC;else if(isdigit((unsigned char)*t)||*t=='.'){char *slash=strchr(t,'/');if(slash)*slash=0;if(strlen(t)<sizeof(size))strcpy(size,t);if(strtof(t,NULL)>=100&&!strpbrk(t,"pe%r"))size[0]=0;}}}}
            else if(strlen(v)<sizeof(size))strcpy(size,v);
            if(!strncmp(size,"clamp(",6)||!strncmp(size,"min(",4)||!strncmp(size,"max(",4)){
                /* clamp(), min(), max(): the first value, the smallest of a
                   clamp(), which suits the small screen */
                char *open=strchr(size,'(');char *first=open+1,*comma=strchr(first,',');if(comma){*comma=0;memmove(size,first,strlen(first)+1);}
            }
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
            else if(eq(key,"border"))border(b,-1,v,s->scale,current);
            else if(!strncmp(key,"border-",7)&&!strchr(key+7,'-')){int i=eq(key+7,"top")?0:eq(key+7,"right")?1:eq(key+7,"bottom")?2:eq(key+7,"left")?3:-1;if(i>=0)border(b,i,v,s->scale,current);}
            else if(eq(key,"border-width")){if(sides(v,s->scale,four)){for(int i=0;i<4;i++)b->border[i]=(unsigned char)clamp(four[i]==BOX_AUTO?0:four[i]<1&&four[i]>0?1:four[i],0,6);if(!four[0]&&!four[1]&&!four[2]&&!four[3])b->plain|=2;}}
            else if(eq(key,"border-color")){uint32_t c;if(eq(v,"currentcolor")){for(int i=0;i<4;i++)b->border_color[i]=current;}else if(color(v,&c))for(int i=0;i<4;i++)b->border_color[i]=c;}
            else if(eq(key,"border-style")&&(eq(v,"none")||eq(v,"hidden"))){memset(b->border,0,sizeof(b->border));b->plain|=2;}
            else if(eq(key,"width")||eq(key,"max-width")||eq(key,"height")||eq(key,"max-height")){
                if(length(v,s->scale,1,&value,&px)){
                    if(value==BOX_AUTO)value=0;
                    if(eq(key,"width")){b->width=(short)(value>2000?2000:value);if(px>=0&&px<=1)b->hide|=HIDE_TINY_W;}
                    else if(eq(key,"max-width"))b->max_width=(short)(value>2000?2000:value);
                    else if(eq(key,"max-height")){if(px==0)b->hide|=HIDE_HEIGHT0;}
                    else{b->height=(short)(value>2000?2000:value);if(px==0)b->hide|=HIDE_HEIGHT0;if(px>0&&px<=1)b->hide|=HIDE_TINY_H;}
                }
                else if(eq(v,"none")&&eq(key,"max-width"))b->max_width=0;
            }
            else if(eq(key,"overflow")&&(eq(v,"hidden")||eq(v,"clip")))b->hide|=HIDE_OVERFLOW;
            else if(eq(key,"justify-content"))b->justify=eq(v,"center")||eq(v,"space-around")||eq(v,"space-evenly")?2:eq(v,"flex-end")||eq(v,"end")||eq(v,"right")?3:eq(v,"flex-start")||eq(v,"start")||eq(v,"left")?1:0;
            else if(eq(key,"gap")||eq(key,"column-gap")||eq(key,"grid-gap")||eq(key,"grid-column-gap")){char first[32];size_t m=strcspn(v," ");if(m<sizeof(first)){memcpy(first,v,m);first[m]=0;if(length(first,s->scale,0,&value,NULL)&&value!=BOX_AUTO)b->gap=(short)clamp(value,0,40);}}
            else if((eq(key,"clip")&&strstr(v,"rect"))||(eq(key,"clip-path")&&(strstr(v,"inset(50%")||strstr(v,"inset(100%"))))b->hide|=HIDE_CLIP;
            else if((eq(key,"left")||eq(key,"top")||eq(key,"right")||eq(key,"text-indent")||eq(key,"inset-inline-start"))&&length(v,s->scale,0,&value,&px)&&px<=-500)b->hide|=HIDE_OFFSCREEN;
            else if(eq(key,"transform")&&offscreen(v))b->hide|=HIDE_OFFSCREEN;
            else if(eq(key,"translate")&&far(v))b->hide|=HIDE_OFFSCREEN;
        }
    }if(!next)break;p=next;}
}

/* ---- the cascade ---- */

static int lower_bound(const browser_css *css,unsigned char kind,unsigned key)
{
    int low=0,high=css->count;
    while(low<high){int mid=low+(high-low)/2;const css_rule *r=&css->rules[css->index[mid]];if(r->kind<kind||(r->kind==kind&&r->key<key))low=mid+1;else high=mid;}
    return low;
}
/* Compiled matching: hashes first, the compound's text only for the rest. */
static int compound_fast(const browser_css *css,const css_compound *c,int node,const char *selector,int *budget)
{
    const css_features *f=css->features;
    if(!element(&css->dom->nodes[node]))return 0;
    if(c->tag&&f->tag[node]!=c->tag)return 0;
    if(c->id&&f->id[node]!=c->id)return 0;
    for(int i=0;i<c->class_count;i++){
        const unsigned *have=f->classes+f->class_first[node];int k=0,count=f->class_count[node];
        while(k<count&&have[k]!=c->classes[i])k++;
        if(k==count)return 0;
    }
    return !c->rest||compound(css->dom,node,selector+c->start,selector+c->end,budget);
}
static int match_compiled(const browser_css *css,const css_rule *r,int node,int k,int *budget)
{
    const css_compound *c=&css->compounds[r->compound+k];
    const char *selector=css->text+r->selector;
    if(--*budget<0||!compound_fast(css,c,node,selector,budget))return 0;
    if(!k)return 1;
    const browser_dom *dom=css->dom;
    switch(c->combinator){
    case '>':{int p=dom->nodes[node].parent;return p>=0&&match_compiled(css,r,p,k-1,budget);}
    case '+':{int s=previous_element(dom,node);return s>=0&&match_compiled(css,r,s,k-1,budget);}
    case '~':for(int s=previous_element(dom,node);s>=0;s=previous_element(dom,s))if(match_compiled(css,r,s,k-1,budget))return 1;return 0;
    default:for(int p=dom->nodes[node].parent;p>=0;p=dom->nodes[p].parent)if(match_compiled(css,r,p,k-1,budget))return 1;return 0;
    }
}
static int rule_matches(const browser_css *css,const browser_dom *dom,int index,const css_rule *r)
{
    if(dom==css->dom&&css->features&&css->features->nodes==dom->count&&r->compounds){
        int budget=2048;
        return match_compiled(css,r,index,r->compounds-1,&budget);
    }
    return css_matches(dom,index,css->text+r->selector);
}
#define MATCHED_MAX 512
static int matched_rules(const browser_css *css,const browser_dom *dom,int index,int *out)
{
    if(!css->count||!css->index)return 0;
    const dom_node *n=&dom->nodes[index];
    unsigned char kinds[34];unsigned keys[34];int ranges=0;
    kinds[ranges]=KEY_ANY;keys[ranges++]=0;
    kinds[ranges]=KEY_TAG;keys[ranges++]=hash(n->tag,strlen(n->tag),1);
    const char *id=n->attribute_count?dom_attr(n,"id"):"";if(*id){kinds[ranges]=KEY_ID;keys[ranges++]=hash(id,strlen(id),0);}
    for(const char *c=n->attribute_count?dom_attr(n,"class"):"";*c&&ranges<34;){while(isspace((unsigned char)*c))c++;const char *end=c;while(*end&&!isspace((unsigned char)*end))end++;if(end>c){kinds[ranges]=KEY_CLASS;keys[ranges++]=hash(c,(size_t)(end-c),0);}c=end;}
    int count=0;
    for(int k=0;k<ranges;k++){
        for(int i=lower_bound(css,kinds[k],keys[k]);i<css->count;i++){
            const css_rule *r=&css->rules[css->index[i]];
            if(r->kind!=kinds[k]||r->key!=keys[k])break;
            int duplicate=0;for(int j=0;j<count;j++)if(out[j]==css->index[i]){duplicate=1;break;}
            if(duplicate||!rule_matches(css,dom,index,r))continue;
            if(count==MATCHED_MAX)break;
            /* in cascade order: specificity, then source order */
            int j=count;
            while(j>0){const css_rule *q=&css->rules[out[j-1]];if(q->specificity<r->specificity||(q->specificity==r->specificity&&q->order<r->order))break;out[j]=out[j-1];j--;}
            out[j]=css->index[i];count++;
        }
    }
    return count;
}
static int compare_names(const void *a,const void *b){return strcmp((const char *)a,*(const char *const *)b);}
/* Elements that are blocks by default (tags are lowercase). */
static int block_tag(const char *tag)
{
    static const char *const blocks[]={"address","article","aside","blockquote","body","br","caption","center","dd","details","dir","div","dl","dt",
        "fieldset","figcaption","figure","footer","form","h1","h2","h3","h4","h5","h6","header","hgroup","hr","html","legend","li","main","menu",
        "nav","ol","p","pre","search","section","summary","table","tbody","tfoot","thead","tr","ul"};
    char lower[32];size_t i=0;for(;tag[i]&&i<sizeof(lower)-1;i++)lower[i]=(char)tolower((unsigned char)tag[i]);lower[i]=0;
    return bsearch(lower,blocks,sizeof(blocks)/sizeof(*blocks),sizeof(*blocks),compare_names)!=NULL;
}
static browser_style compute(const browser_css *css,const browser_dom *dom,int index,browser_style parent,browser_box *box)
{
    browser_style s=parent;const dom_node *n=&dom->nodes[index];s.block=0;
    if(box)memset(box,0,sizeof(*box));
    if(!strcmp(n->tag,"#text"))return s;
    s.hidden=0;s.background=0;s.flags&=~CSS_CLIP_TEXT;
    s.block=block_tag(n->tag);
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
    int cell=eq(n->tag,"td")||eq(n->tag,"th");
    if(n->attribute_count&&(s.block||cell)){const char *align=dom_attr(n,"align");if(*align){if(eq(align,"center")||eq(align,"middle"))s.align=1;else if(eq(align,"right"))s.align=2;else if(eq(align,"left"))s.align=0;}}
    /* Presentational attributes, which old and minimal pages still use. */
    uint32_t c;
    if((eq(n->tag,"font")||eq(n->tag,"basefont"))&&*dom_attr(n,"color")&&color(dom_attr(n,"color"),&c))s.color=c;
    if(eq(n->tag,"font")&&*dom_attr(n,"size")){int k=atoi(dom_attr(n,"size"));const char *v=dom_attr(n,"size");if(*v=='+'||*v=='-')k=3+k;static const float sizes[]={0.48f,0.54f,0.64f,0.74f,0.86f,1.0f,1.1f};if(k<1)k=1;if(k>7)k=7;s.scale=sizes[k-1];}
    if(n->attribute_count&&(cell||eq(n->tag,"body")||eq(n->tag,"table")||eq(n->tag,"tr"))&&*dom_attr(n,"bgcolor")&&color(dom_attr(n,"bgcolor"),&c))s.background=c;
    if(eq(n->tag,"body")&&*dom_attr(n,"text")&&color(dom_attr(n,"text"),&c))s.color=c;
    if(eq(n->tag,"body")&&*dom_attr(n,"link")&&color(dom_attr(n,"link"),&c))s.link=c;
    if(eq(n->tag,"a")&&*dom_attr(n,"href")){s.color=s.link?s.link:0xffe6bc52;s.flags|=CSS_UNDERLINE;}
    if(box&&n->attribute_count){
        if(eq(n->tag,"table")&&*dom_attr(n,"border")&&atoi(dom_attr(n,"border"))>0)for(int i=0;i<4;i++){box->border[i]=1;box->border_color[i]=0xff808080;}
        const char *w=dom_attr(n,"width");
        if(*w&&(eq(n->tag,"table")||eq(n->tag,"td")||eq(n->tag,"th")||eq(n->tag,"img")||eq(n->tag,"col")||eq(n->tag,"hr")||eq(n->tag,"iframe"))){char *end;float x=strtof(w,&end);if(x>0)box->width=(short)(*end=='%'?-(x>100?100:x):x*CSS_PX+0.5f);}
        const char *h=dom_attr(n,"height");
        if(*h&&(eq(n->tag,"img")||eq(n->tag,"iframe"))){float x=strtof(h,NULL);if(x>0)box->height=(short)(x*CSS_PX+0.5f);}
    }
    int matched[MATCHED_MAX],count=matched_rules(css,dom,index,matched);
    /* Custom properties for var(): this element's rules and style attribute. */
    scope sc;sc.css=css;sc.count=0;
    for(int i=0;i<count;i++){const css_rule *r=&css->rules[matched[i]];if(r->variables){const char *d=css->text+r->declarations;each_declaration(d,strlen(d),scope_add,&sc);}}
    const char *style=n->attribute_count?dom_attr(n,"style"):"";
    if(*style&&strstr(style,"--"))each_declaration(style,strlen(style),scope_add,&sc);
    for(int priority=0;priority<2;priority++){
        for(int i=0;i<count;i++)declarations(&s,box,css->text+css->rules[matched[i]].declarations,priority,&sc);
        declarations(&s,box,style,priority,&sc);
    }s.hidden|=parent.hidden;
    /* Gradient text: transparent letters show the background's colour. */
    if(s.flags&CSS_CLIP_TEXT){if(s.background&&!(s.color>>24))s.color=s.background;s.background=0;s.flags&=~CSS_CLIP_TEXT;}
    /* A closed <details> shows only its <summary>. */
    if(n->parent>=0){const dom_node *up=&dom->nodes[n->parent];if(eq(up->tag,"details")&&!has_attribute(up,"open")&&!eq(n->tag,"summary"))s.hidden=1;}
    /* The hidden attribute, present even when its value is empty. */
    for(int i=0;i<n->attribute_count;i++)if(eq(n->attributes[i].name,"hidden")&&!eq(n->attributes[i].value,"until-found"))s.hidden=1;
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

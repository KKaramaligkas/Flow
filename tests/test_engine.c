#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/document.h"
#include "../src/script.h"
#include "../src/layout.h"
static int checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;fprintf(stderr,"%d: %s\n",__LINE__,#x);}}while(0)
static browser_document *parse(const char *html){browser_document *d=calloc(1,sizeof(*d));char err[256];CHECK(browser_document_parse(d,html,strlen(html),"https://example.org/path/page","text/html",err,sizeof(err))==0);return d;}
static void drop(browser_document *d){browser_document_free(d);free(d);}
static void run(browser_document *d){char err[256];CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,1000,err,sizeof(err))==0);}
static void modern(void){browser_document *d=parse("<html><head><title>old</title></head><body><p id='result'>old</p><script>const data=[1,2,3];class Message { static text(){return `modern ${data.map(x=>x*2).join(',')}`;} } let {a=4}={};document.querySelector('#result').textContent=Message.text()+' '+(2n**8n);document.title='JS works';Promise.resolve().then(()=>document.querySelector('#result').classList.add('done'));document.addEventListener('DOMContentLoaded',()=>{const p=document.createElement('p');p.textContent='loaded';document.body.append(p);});</script></body></html>");run(d);CHECK(strstr(d->text,"modern 2,4,6 256"));CHECK(strstr(d->text,"loaded"));CHECK(!strcmp(d->title,"JS works"));CHECK(d->scripts_run==1&&d->scripts_failed==0);drop(d);}
static void mutation(void){browser_document *d=parse("<body><div id='root'></div><script>const r=document.getElementById('root');r.innerHTML='<p class=note>new &amp; text</p>';r.querySelector('.note').style.color='red';r.querySelector('.note').classList.toggle('active');const a=document.createElement('a');a.href='../next';a.textContent='Continue';r.appendChild(a);setTimeout(()=>document.write('<p>timer</p>'),100);</script></body>");run(d);CHECK(strstr(d->text,"new & text"));CHECK(strstr(d->text,"timer"));CHECK(d->count==1&&!strcmp(d->links[0].url,"https://example.org/next"));CHECK(browser_style_at(d,0).color==0xff0000ff);drop(d);}
static char *fixture(void *ud,const char *url,int limit,int *len,char *err,size_t errlen){(void)ud;(void)err;(void)errlen;CHECK(!strcmp(url,"https://example.org/data"));CHECK(limit>=11);*len=11;char *p=malloc(12);memcpy(p,"{\"value\":7}",12);return p;}
static void fetching(void){browser_document *d=parse("<p id='x'>before</p><script>(async()=>{const r=await fetch('/data');const j=await r.json();document.getElementById('x').textContent='value '+j.value;})();</script>");char err[256];CHECK(browser_script_run(d,fixture,NULL,NULL,NULL,1000,err,sizeof(err))==0);CHECK(strstr(d->text,"value 7"));drop(d);d=parse("<p id='x'>before</p><script>fetch('https://other.org/data').catch(()=>document.getElementById('x').textContent='blocked');</script>");CHECK(browser_script_run(d,fixture,NULL,NULL,NULL,1000,err,sizeof(err))==0);CHECK(strstr(d->text,"blocked"));drop(d);CHECK(browser_same_origin("https://a/","https://a:443/x"));CHECK(browser_same_origin("http://a/","http://a:80/x"));CHECK(!browser_same_origin("https://a/","http://a/"));CHECK(!browser_same_origin("https://a/","https://a.evil/"));}
static char *module_fixture(void *ud,const char *url,int limit,int *len,char *err,size_t errlen){(void)ud;(void)err;(void)errlen;CHECK(!strcmp(url,"https://example.org/path/helper.js"));const char *code="export const value = 9;";*len=(int)strlen(code);CHECK(*len<limit);char *out=malloc((size_t)*len+1);strcpy(out,code);return out;}
static void modules(void){browser_document *d=parse("<p id=x>before</p><script type=module>import {value} from './helper.js';document.getElementById('x').textContent=`module ${await Promise.resolve(value)}`;</script>");char err[256];CHECK(browser_script_run(d,module_fixture,NULL,NULL,NULL,1000,err,sizeof(err))==0);CHECK(strstr(d->text,"module 9"));CHECK(d->scripts_failed==0);drop(d);}
static int cancel(void *ud){(void)ud;return 1;}
static void limits(void){char err[256];browser_document *d=parse("<p>safe</p><script>while(true){}</script>");CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,30,err,sizeof(err))<0);CHECK(strstr(d->text,"safe"));CHECK(d->scripts_failed>0);drop(d);d=parse("<p>safe</p><script>while(true){}</script>");CHECK(browser_script_run(d,NULL,NULL,cancel,NULL,1000,err,sizeof(err))<0);CHECK(strstr(d->text,"safe"));drop(d);d=parse("<p>safe</p><script>const a=[];while(true)a.push('x'.repeat(10000));</script>");CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,1000,err,sizeof(err))<0||d->scripts_failed>0);CHECK(strstr(d->text,"safe"));drop(d);d=parse("<p>safe</p><script>document.body.appendChild(document.body);</script>");run(d);CHECK(strstr(d->text,"safe"));CHECK(d->scripts_failed>0);drop(d);d=parse("<p>safe</p><script type=module>import x from '/x.js';</script>");run(d);CHECK(d->scripts_failed>0);drop(d);}
static void styling(void){browser_document *d=parse("<style>p{color:blue}div .note{color:red}#x{color:green}p{color:yellow!important}.gone{display:none}</style><body><div><p id=x class=note style='color:white'>visible</p><p class=gone>secret</p></div></body>");CHECK(!strstr(d->text,"secret"));CHECK(browser_style_at(d,0).color==0xff00ffff);drop(d);d=parse("<style>@media screen {@media screen {p{color:#123456}}}p{font-size:24px;text-align:center}</style><p>center</p>");browser_style st=browser_style_at(d,0);CHECK(st.color==0xff563412);CHECK(st.align==1&&st.scale>0.9f);drop(d);char css[4096]="";for(int i=0;i<100;i++)strcat(css,"@media screen {");strcat(css,"p{color:red}");for(int i=0;i<100;i++)strcat(css,"}");browser_css rules={0};CHECK(css_add(&rules,css,strlen(css))==0);CHECK(rules.omitted);css_free(&rules);}
typedef struct{const char *s;size_t pos,len;int chunk;} reader;
static int reading(void *ud,char *out,size_t max){reader *r=ud;size_t n=r->len-r->pos;if(n>max)n=max;if(n>(size_t)r->chunk)n=(size_t)r->chunk;memcpy(out,r->s+r->pos,n);r->pos+=n;return (int)n;}
static void streaming(void){const char *html="<!--hidden--><p>α &amp; β 😀</p><script>if(1<2)document.write('ok')</script><p>tail</p>";char err[256];for(int chunk=1;chunk<18;chunk++){reader r={html,0,strlen(html),chunk};browser_document *d=calloc(1,sizeof(*d));CHECK(browser_document_read(d,reading,&r,"https://example.org/","text/html",NULL,NULL,err,sizeof(err))==0);CHECK(strstr(d->text,"α & β 😀"));CHECK(strstr(d->text,"tail"));drop(d);}size_t n=2*1024*1024;char *large=malloc(n+100);strcpy(large,"<script>");memset(large+8,' ',n);strcpy(large+8+n,"</script><p>tail after huge script</p>");reader r={large,0,strlen(large),113};browser_document *d=calloc(1,sizeof(*d));CHECK(browser_document_read(d,reading,&r,"https://example.org/","text/html",NULL,NULL,err,sizeof(err))==0);CHECK(strstr(d->text,"tail after huge script"));CHECK(!d->dom->nodes[1].text||*dom_attr(&d->dom->nodes[1],"data-ark-omitted"));drop(d);free(large);browser_dom dom;const char *cycle="[{\"tag\":\"#document\",\"text\":\"\",\"attrs\":{},\"children\":[0]}]";CHECK(dom_from_json(&dom,cycle,strlen(cycle))<0);}
static void spans(void){browser_document *d=parse("<pre><span style='color:red'>a </span><span style='color:blue'>  </span></pre><p style='color:green'>b</p>");for(int i=1;i<d->span_count;i++)CHECK(d->spans[i].offset>=d->spans[i-1].offset);const char *b=strchr(d->text,'b');CHECK(b&&browser_style_at(d,(size_t)(b-d->text)).color==0xff008000);drop(d);}
static float measure(void *ud,const char *s,size_t n,browser_style style){(void)ud;(void)s;(void)n;return style.scale*10;}
static void layout(void){browser_document *d=parse("<p>one two αβγδε long</p>");browser_line lines[32];int count=browser_layout(d,lines,32,30,measure,NULL);CHECK(count>2);for(int i=0;i<count;i++){CHECK(lines[i].length>0);CHECK(((unsigned char)d->text[lines[i].start]&0xc0)!=0x80);CHECK(lines[i].height>=11);}drop(d);}
/* A script's text takes about its own size: real pages have dozens of scripts,
   and a fixed buffer for each one ran a PSP out of memory on a 29 KB page. */
static void scripts_fit(void){size_t size=16*1024;char *html=malloc(size);strcpy(html,"<p>start</p>");for(int i=0;i<64;i++){char s[96];snprintf(s,sizeof(s),"<script>var a%d=%d;</script><script src='/s%d.js'></script>",i,i,i);strcat(html,s);}strcat(html,"<style>p{color:red}</style><p>end</p>");browser_document *d=parse(html);int scripts=0;for(int i=0;i<d->dom->count;i++){const dom_node *n=&d->dom->nodes[i];if(strcmp(n->tag,"script"))continue;scripts++;CHECK(!n->text||malloc_usable_size(n->text)<256);}CHECK(scripts==128);CHECK(strstr(d->text,"end")!=NULL);CHECK(d->dom->script_bytes<4096);drop(d);free(html);}
/* SVG drawings and templates keep their element but not their content; a
   page with thousands of elements fits. */
static void dom_size(void){browser_document *d=parse("<p>a<svg width=10 aria-label=icon><path d='M0'/><g><circle r=1/></g><svg><rect/></svg><text>label</text></svg>b</p><template><p>inert</p></template><p>after</p>");int svg=-1;for(int i=0;i<d->dom->count;i++)if(!strcmp(d->dom->nodes[i].tag,"svg"))svg=i;CHECK(svg>=0&&d->dom->nodes[svg].first<0&&!strcmp(dom_attr(&d->dom->nodes[svg],"aria-label"),"icon"));CHECK(strstr(d->text,"ab")||strstr(d->text,"a b"));CHECK(!strstr(d->text,"inert")&&!strstr(d->text,"label")&&strstr(d->text,"after"));CHECK(d->dom->count<16);drop(d);
    size_t size=5000*16+64;char *html=malloc(size);strcpy(html,"<body>");char *end=html+6;for(int i=0;i<5000;i++)end+=sprintf(end,"<p>item %d</p>",i);strcpy(end,"</body>");d=parse(html);CHECK(!d->dom->shortened&&d->dom->count>10000);CHECK(strstr(d->text,"item 4999")!=NULL);drop(d);free(html);}
/* The style of the element with an id, from a plain parent style. */
static browser_style style_of(browser_document *d,const char *id){browser_style base={.color=0xff000000,.scale=0.64f};for(int i=0;i<d->dom->count;i++)if(!strcmp(dom_attr(&d->dom->nodes[i],"id"),id))return css_compute(&d->css,d->dom,i,base);browser_style none={0};return none;}
static int hidden_box(browser_document *d,const char *id){browser_style base={.color=0xff000000,.scale=0.64f};browser_box box;for(int i=0;i<d->dom->count;i++)if(!strcmp(dom_attr(&d->dom->nodes[i],"id"),id)){browser_style st=css_compute_box(&d->css,d->dom,i,base,&box);return st.hidden||box.hide;}return -1;}
#define RED 0xff0000ffu
#define GREEN 0xff008000u
static void media(void){
    CHECK(css_media_matches("screen")&&!css_media_matches("print")&&css_media_matches("")&&css_media_matches("all"));
    CHECK(css_media_matches("(min-width: 768px)")&&!css_media_matches("(max-width: 767px)")&&!css_media_matches("(min-width:1024px)"));
    CHECK(css_media_matches("(width >= 40rem)")&&!css_media_matches("(width < 48rem)")&&css_media_matches("(48rem <= width < 64rem)"));
    CHECK(css_media_matches("not print")&&!css_media_matches("only screen and (max-width: 600px)")&&css_media_matches("print, screen and (min-width:700px)"));
    CHECK(!css_media_matches("(prefers-color-scheme: dark)")&&css_media_matches("(prefers-color-scheme: light)")&&css_media_matches("(hover: hover)"));
    CHECK(!css_media_matches("(orientation: portrait)")&&!css_media_matches("(-webkit-min-device-pixel-ratio: 2)")&&css_media_matches("(min-resolution: 96dpi)"));
    CHECK(!css_media_matches("screen and (max-width: 480px), print")&&css_media_matches("(min-width: 640px) and (max-width: 1023px)"));
}
static void selectors(void){
    browser_document *d=parse("<style>@layer base{#a{color:red}}@supports (display:grid){#b{color:red}}@supports not (display:grid){#b{color:green}}"
        ".md\\:flex{color:red}[data-x=\"a b\"]{color:red}a[href^=\"https\"]{color:red}li:first-child{color:red}li:nth-child(2n+1){font-weight:bold}"
        "p.n:not(.skip){color:red}:is(h1,h2).t{color:red}:where(.w) span{color:red}h3+p{color:green}h3~div{color:green}#e::before{color:red}#e:hover{color:red}"
        ":root{--brand:#ff0000}#v{color:var(--brand)}.btn{--c:#00f;color:var(--c)}.btn-red{--c:#f00}#f{color:var(--missing, green)}#g{color:var(--missing)}"
        "#h{color:hsl(120, 100%, 25.1%)}#i{color:oklch(62.8% 0.2577 29.23)}#j{color:rgb(255 0 0 / 50%)}#k{color:rgb(0 0 255 / 10%)}"
        "#m{transform:translateY(-100%)}#n{max-height:0;overflow:hidden}#q{position:fixed;transform:translate(100%)}@property --tx{syntax:'*';inherits:false;initial-value:0}#s2{--ty:-200%;translate:var(--tx) var(--ty)}#r{transform:translate3d(0,-9999px,0)}#w2{transform:translateY(10px) scale(1.1)}@media print{#o{color:red}}@media (min-width:768px){#o{color:green}}</style>"
        "<p id=a>a</p><p id=b>b</p><p id=c class='md:flex'>c</p><p id=d data-x='a b'>d</p><a id=l href='https://x/'>l</a><ul><li id=l1>1</li><li id=l2>2</li><li id=l3>3</li></ul>"
        "<p id=s class='n skip'>s</p><p id=a2 class=n>a2</p><h1 id=t class=t>t</h1><div class=w><span id=u>u</span></div><h3>h</h3><p id=x>x</p><div id=y>y</div><p id=e>e</p>"
        "<p id=v>v</p><p id=btn class='btn btn-red'>b</p><p id=btn2 class=btn>b2</p><p id=f>f</p><p id=g>g</p><p id=h>h</p><p id=i>i</p><p id=j>j</p><p id=k>k</p>"
        "<p id=m>m</p><p id=n>n</p><p id=o>o</p><p id=q>q</p><p id=s2>s2</p><p id=r>r</p><p id=w2>w</p>");
    CHECK(style_of(d,"a").color==RED);CHECK(style_of(d,"b").color==RED);
    CHECK(style_of(d,"c").color==RED);CHECK(style_of(d,"d").color==RED);CHECK(style_of(d,"l").color==RED);
    CHECK(style_of(d,"l1").color==RED&&style_of(d,"l2").color!=RED);CHECK((style_of(d,"l3").flags&CSS_BOLD)&&!(style_of(d,"l2").flags&CSS_BOLD));
    CHECK(style_of(d,"s").color!=RED&&style_of(d,"a2").color==RED);CHECK(style_of(d,"t").color==RED);CHECK(style_of(d,"u").color==RED);
    CHECK(style_of(d,"x").color==GREEN&&style_of(d,"y").color==GREEN);CHECK(style_of(d,"e").color!=RED);
    CHECK(style_of(d,"v").color==RED);CHECK(style_of(d,"btn").color==RED&&style_of(d,"btn2").color==0xffff0000u);
    CHECK(style_of(d,"f").color==GREEN);CHECK(style_of(d,"g").color==0xff000000u);   /* unresolved var(): the parent's color stays */
    uint32_t h=style_of(d,"h").color,i=style_of(d,"i").color;
    CHECK((h&0xff00)>=0x7f00&&(h&0xff)==0&&(h>>16&0xff)==0);
    CHECK((i&0xff)>=0xf0&&(i>>8&0xff)<0x10&&(i>>16&0xff)<0x10);
    CHECK(style_of(d,"j").color==RED&&style_of(d,"k").color!=0xffff0000u);
    CHECK(hidden_box(d,"m")==1&&hidden_box(d,"n")==1&&hidden_box(d,"x")==0);
    CHECK(hidden_box(d,"q")==1&&hidden_box(d,"r")==1&&hidden_box(d,"w2")==0&&hidden_box(d,"s2")==1);
    CHECK(style_of(d,"o").color==GREEN);
    drop(d);
}
/* Large stylesheets are reduced to the rules the page can use. */
static char *big_sheet(void *ud,const char *url,int max,int *length,char *final){(void)ud;
    if(strstr(url,"print"))return NULL;
    size_t size=400*1024;char *css=malloc(size+1);size_t n=0;
    for(int i=0;n+200<size;i++)n+=(size_t)sprintf(css+n,".unused-%d{color:blue;margin:1px}",i);
    n+=(size_t)sprintf(css+n,"@media print{.used{color:blue}}.used{color:red}#late{color:red}");
    css[n]=0;*length=(int)n;strcpy(final,url);if((int)n>max){free(css);return NULL;}return css;}
static int sheet_requests;
static char *count_sheet(void *ud,const char *url,int max,int *length,char *final){(void)ud;(void)max;sheet_requests++;char *css=strdup("p{color:red}");*length=(int)strlen(css);strcpy(final,url);return css;}
static void compacting(void){
    size_t length=0;const char *sheet="@media print{p{color:red}}@media screen{.a{color:red}.missing{color:blue}}@font-face{font-family:x}.b,.nothere{color:green}@layer x{#c{color:red}}";
    browser_document *d=parse("<p class=a>a</p><p class=b>b</p><p id=c>c</p>");
    char *c=css_compact(sheet,strlen(sheet),d->dom,&length);
    CHECK(c&&strstr(c,".a{color:red}")&&strstr(c,".b{color:green}")&&strstr(c,"#c{color:red}"));
    CHECK(c&&!strstr(c,"missing")&&!strstr(c,"nothere")&&!strstr(c,"font-face")&&!strstr(c,"@media"));
    free(c);
    const char *registered="@property --x{syntax:'*';initial-value:2px}.a{margin:var(--x)}";
    c=css_compact(registered,strlen(registered),d->dom,&length);
    CHECK(c&&strstr(c,"@property --x{")&&strstr(c,"initial-value:2px"));
    free(c);drop(d);
    d=parse("<html><head><link rel=stylesheet href=/big.css><link rel=stylesheet media=print href=/print.css><link rel=preload as=style href=/big.css?2></head><body><p id=used class=used>x</p><p id=late>y</p></body></html>");
    d->scripting=1;browser_assets(d,0,big_sheet,NULL,NULL,NULL);char err[256];CHECK(browser_document_render(d,err,sizeof(err))==0);
    CHECK(style_of(d,"used").color==RED&&style_of(d,"late").color==RED);
    CHECK(d->dom->bytes<64*1024&&d->css.count<16&&d->assets_omitted==0);
    drop(d);
    /* A stylesheet in <noscript> isn't fetched while JavaScript is on. */
    for(int javascript=0;javascript<2;javascript++){
        d=parse("<head><noscript><link rel=stylesheet href=/noscript.css></noscript><link rel=stylesheet href=/main.css></head><p>x</p>");
        d->scripting=javascript;sheet_requests=0;browser_assets(d,javascript,count_sheet,NULL,NULL,NULL);
        CHECK(sheet_requests==2-javascript);
        drop(d);
    }
}
/* Omitted end tags close as in browsers: a block ends an open <p>, items,
   cells and rows end the previous one. */
static int child_count(const browser_dom *dom,int node,const char *tag){int n=0;for(int c=dom->nodes[node].first;c>=0;c=dom->nodes[c].next)n+=!strcmp(dom->nodes[c].tag,tag);return n;}
static int find_tag(const browser_dom *dom,const char *tag,int nth){for(int i=0;i<dom->count;i++)if(!strcmp(dom->nodes[i].tag,tag)&&!nth--)return i;return -1;}
static void implied(void){browser_document *d=parse("<style>.hidden{display:none}</style><p class=hidden><script>var a;</script><section>visible</section>"
    "<ul><li>a<li>b<ul><li>c<li>d</ul><li>e</ul><table><tr><td>1<td>2<tr><td>3</table><dl><dt>t<dd>d<dt>t2</dl><select><option>x<option>y</select>");
    CHECK(strstr(d->text,"visible")!=NULL);
    const browser_dom *m=d->dom;int ul=find_tag(m,"ul",0),inner=find_tag(m,"ul",1),table=find_tag(m,"table",0),dl=find_tag(m,"dl",0),sel=find_tag(m,"select",0);
    CHECK(ul>=0&&child_count(m,ul,"li")==3&&inner>=0&&child_count(m,inner,"li")==2);
    int body=table>=0?m->nodes[table].first:-1,rows=0;for(int c=body;c>=0;c=m->nodes[c].next)rows+=!strcmp(m->nodes[c].tag,"tr");
    CHECK(rows==2&&child_count(m,find_tag(m,"tr",0),"td")==2);
    CHECK(dl>=0&&child_count(m,dl,"dt")==2&&child_count(m,dl,"dd")==1&&sel>=0&&child_count(m,sel,"option")==2);
    drop(d);
    /* A stray quote in a tag, from apache.org: the rest of the page stays. */
    d=parse("<div \"=\"\" class=\"logos\" id=\"para-1\"><img alt=\"Logo\"></div><p title='it\"s'>after</p>");
    CHECK(strstr(d->text,"after")!=NULL&&find_tag(d->dom,"p",0)>=0&&!strcmp(dom_attr(&d->dom->nodes[find_tag(d->dom,"div",0)],"id"),"para-1"));
    CHECK(!strcmp(dom_attr(&d->dom->nodes[find_tag(d->dom,"p",0)],"title"),"it\"s"));
    drop(d);}
/* The scripts' DOM: selectors with combinators and attributes, parents kept
   through changes, elements created on big pages, and the browser objects
   scripts expect. Each check writes its result into #out. */
static void script_dom(void)
{
    const char *html=
        "<html class=no-js><body><nav class='menu top'><ul id=items><li class=first><a href='/a' data-x=1>A</a></li><li><a href='https://x.org/b' lang=en-GB>B</a></li><li class=last><a>C</a></li></ul></nav>"
        "<form><input type=checkbox checked name=c><input type=text name=t disabled></form><p id=out></p>"
        "<script>"
        "const r=[];const q=s=>document.querySelectorAll(s).length;"
        "r.push(q('nav > ul > li'),q('nav > li'),q('li + li'),q('li ~ li'),q('.first ~ li'),q('a[href]'),q('a[href^=\"https\"]'),q('a[data-x=\"1\"]'),q('a[lang|=en]'),q('li:not(.first)'),q('li:first-child'),q('li:nth-child(2n+1)'),q('.menu.top a'),q('input:checked'),q('input:disabled'),q('ul#items, form'),q('a:hover'),q('p::before'));"
        "r.push(document.querySelector('li a').textContent,document.querySelector('a').closest('nav').className,document.querySelector('.last').matches('ul > .last'));"
        "let bad=0;try{document.querySelector('a[');}catch(e){bad=e instanceof SyntaxError;}r.push(bad);"
        "const li=document.createElement('li');li.innerHTML='<b>new</b>';document.getElementById('items').appendChild(li);"
        "r.push(li.parentNode.id,li.firstChild.parentNode===li,document.querySelectorAll('#items > li').length,li.previousElementSibling.className,document.getElementById('items').contains(li.firstChild));"
        "li.remove();r.push(li.parentNode,document.querySelectorAll('#items > li').length);"
        "document.documentElement.classList.replace('no-js','js');r.push(document.documentElement.className,li.ownerDocument===document,document.body.tagName,document.head===null);"
        "r.push(typeof performance.now(),typeof requestAnimationFrame,typeof new MutationObserver(()=>{}).observe,localStorage.getItem('k'),(localStorage.setItem('k',5),localStorage.getItem('k')));"
        "const u=new URL('../x?a=1&b=2#h',location.href);r.push(u.href,u.searchParams.get('b'),new URL('/y','https://other.org/z').host,location.pathname);"
        "r.push(matchMedia('(min-width: 700px)').matches,matchMedia('(max-width: 600px)').matches,window.innerWidth);"
        "let ran=0;setInterval(()=>ran++,10);requestAnimationFrame(()=>ran++);"
        "document.addEventListener('DOMContentLoaded',()=>{throw Error('broken listener');});"
        "document.addEventListener('DOMContentLoaded',()=>{r.push('next listener ran');});"
        "window.addEventListener('load',()=>{r.push('load ran',ran);document.getElementById('out').textContent=r.join('|');});"
        "</script></body></html>";
    browser_document *d=parse(html);char err[256];
    CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,1000,err,sizeof(err))==0);
    const char *want="3|0|2|2|2|2|1|1|1|2|1|2|3|1|1|2|0|0|A|menu top|true|true|items|true|4|last|true||3|js|true|BODY|true|number|function|function||5|https://example.org/x?a=1&b=2#h|2|other.org|/path/page|true|false|786|next listener ran|load ran|0";
    CHECK(strstr(d->text,want)!=NULL);
    if(!strstr(d->text,want))fprintf(stderr,"got: %s\n",d->text);
    CHECK(d->scripts_failed==1);    /* the broken listener */
    drop(d);
    /* Timers ran after the load listeners: the result above was written first. */
    d=parse("<p id=out></p><script>let n=0;setInterval(()=>{n++;document.getElementById('out').textContent='ticks '+n;},5);</script>");
    run(d);CHECK(strstr(d->text,"ticks 1"));drop(d);
    /* Scripts that only read the page leave it as it was, without a rebuild;
       one change is enough to bring the scripts' version back. */
    d=parse("<p id=a>same</p><script>window.n=document.querySelectorAll('p').length+document.getElementById('a').textContent.length;</script>");
    const browser_dom *before=d->dom;
    run(d);CHECK(d->dom==before&&strstr(d->text,"same")&&d->scripts_run==1&&d->scripts_failed==0);drop(d);
    d=parse("<p id=a>same</p><script>document.getElementById('a').setAttribute('title','x');</script>");
    before=d->dom;
    run(d);CHECK(d->dom!=before);       /* the new one is made before the old one goes */
    {int titled=0;for(int i=0;i<d->dom->count;i++)if(!strcmp(dom_attr(&d->dom->nodes[i],"title"),"x"))titled=1;CHECK(titled);}
    drop(d);
    /* Markup cut off inside a tag, as some scripts write it, drops the tag. */
    d=parse("<p id=out></p><script>const t=document.createElement('div');t.innerHTML='<div class=x><a href=\\'#m\\'>Skip</a></div';document.getElementById('out').textContent='['+t.textContent+']';</script>");
    run(d);CHECK(strstr(d->text,"[Skip]"));drop(d);
    /* Elements can be created on a page bigger than 2,048 nodes, and
       queries stay fast on it. */
    size_t size=1500*40+400;char *big=malloc(size),*p=big;
    p+=sprintf(p,"<div id=root>");
    for(int i=0;i<1500;i++)p+=sprintf(p,"<div class=c%d><span>x</span></div>",i%7);
    sprintf(p,"</div><p id=out></p><script>let found=0;for(let i=0;i<40;i++)found+=document.querySelectorAll('#root .c3 span').length+(document.querySelector('div.c6 > span')?1:0);const e=document.createElement('b');e.textContent='made '+found;document.getElementById('out').appendChild(e);</script>");
    d=parse(big);
    CHECK(d->dom->count>4000);
    CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,3000,err,sizeof(err))==0);
    CHECK(strstr(d->text,"made 8600"));      /* 40 x (214 + 1) */
    drop(d);free(big);
    /* Only scripts that weren't downloaded (other sites'): no trip through
       JavaScript at all, and nothing to report. */
    d=parse("<p>kept</p><script src='https://cdn.example/a.js'></script><script src='/b.js'></script>");
    CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,1000,err,sizeof(err))==0);
    CHECK(d->scripts_run==0&&d->scripts_failed==0&&strstr(d->text,"kept"));
    drop(d);
    /* A page too large for its scripts is shown as it is, without running them. */
    size=7000*12+200;big=malloc(size);p=big;
    for(int i=0;i<7000;i++)p+=sprintf(p,"<b>%d</b>",i%10);
    sprintf(p,"<p id=out>untouched</p><script>document.getElementById('out').textContent='ran';</script>");
    d=parse(big);
    CHECK(d->dom->count>6000);
    CHECK(browser_script_run(d,NULL,NULL,NULL,NULL,1000,err,sizeof(err))<0);
    CHECK(d->scripts_run==0&&d->scripts_failed==1&&strstr(d->text,"untouched")&&!strstr(d->text,"ran"));
    drop(d);free(big);
}
int main(void){modern();mutation();fetching();modules();limits();styling();streaming();spans();layout();scripts_fit();dom_size();media();selectors();compacting();implied();script_dom();printf("engine: %d checks, %d failures\n",checks,failures);return failures?1:0;}

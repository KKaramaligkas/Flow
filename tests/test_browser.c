#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/url.h"
#include "../src/document.h"
#include "../src/session.h"
static int checks,failures;
#define CHECK(test) do{checks++;if(!(test)){failures++;fprintf(stderr,"%d: %s\n",__LINE__,#test);}}while(0)
#define SAME(a,b) CHECK(strcmp((a),(b))==0)
static void urls(void)
{
    const char *base="https://EXAMPLE.org/a/b/page?q=old#last";
    const struct {const char *ref,*expected;} cases[]={
        {"next","https://example.org/a/b/next"},{"../next","https://example.org/a/next"},
        {"../../next","https://example.org/next"},{"../../../next","https://example.org/next"},
        {"/a/./b/../c/","https://example.org/a/c/"},{"/a//b","https://example.org/a//b"},
        {"?x=1&y=2","https://example.org/a/b/page?x=1&y=2"},
        {"#section","https://example.org/a/b/page?q=old#section"},
        {"","https://example.org/a/b/page?q=old"},
        {"//other.org/file","https://other.org/file"},{"HTTP://other.org:8080","http://other.org:8080/"},
        {"./","https://example.org/a/b/"},{"..","https://example.org/a/"},
        {"/","https://example.org/"},{"%2e%2e/item","https://example.org/a/b/%2e%2e/item"},
        {"download.zip?token=abc#top","https://example.org/a/b/download.zip?token=abc#top"}};
    char out[BROWSER_URL_MAX];
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);i++){CHECK(browser_url_resolve(base,cases[i].ref,out,sizeof(out))==0);SAME(out,cases[i].expected);}
    const char *bad[]={"javascript:alert(1)","data:text/html,hi","file:///flash0/a","ftp://a/b","https://user:password@a/",
        "https://a:0/","https://a:65536/","https:///path","https://a\\b/","https://a/a b","https://a/%", "https://a/%xy", "https://[::1]/", "https://a/\r\nInjected: header"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(browser_url_resolve(base,bad[i],out,sizeof(out))<0);
    CHECK(browser_url_resolve(base,"next",out,8)<0);
    CHECK(browser_url_enter("  example.org/docs  ",out,sizeof(out))==0);SAME(out,"https://example.org/docs");
    CHECK(browser_url_enter("http://example.org",out,sizeof(out))==0);CHECK(!browser_url_secure(out));
    CHECK(browser_url_enter("",out,sizeof(out))<0);
    CHECK(browser_url_resolve(NULL,"https://example.org?x=1",out,sizeof(out))==0);SAME(out,"https://example.org/?x=1");
    browser_filename("https://host/a/b/file.zip?secret=yes",out,sizeof(out));SAME(out,"file.zip");
    browser_filename("https://host/a/../../.evil",out,sizeof(out));SAME(out,"download.bin");
    browser_filename("https://host/",out,sizeof(out));SAME(out,"download.bin");
    browser_filename("https://host/a/b%20c:evil.zip",out,sizeof(out));SAME(out,"b_20c_evil.zip");
    char long_url[2048];memset(long_url,'a',sizeof(long_url));long_url[sizeof(long_url)-1]=0;
    CHECK(browser_url_enter(long_url,out,sizeof(out))<0);
}
static browser_document *parse(const char *text,const char *type)
{
    browser_document *doc=calloc(1,sizeof(*doc));char error[256];
    int result=browser_document_parse(doc,text,strlen(text),"https://example.org/docs/page",type,error,sizeof(error));
    CHECK(result==0);return doc;
}
static void release(browser_document *doc){browser_document_free(doc);free(doc);}
static void documents(void)
{
    browser_document *d=parse("<head><title>A &amp; B</title><base href='/other/'><style>invisible</style></head>"
        "<h1>Hello</h1><p>one <b>two</b> &#x3b1; &lt;three&gt;</p>"
        "<script>if(a<b){ '<a href=evil>hidden</a>' }</script><template>secret</template>"
        "<p><a HREF='next?q=1&amp;b=2'>Read <em>more</em></a> <a href='javascript:alert(1)'>unsafe</a>"
        "<img src='https://tracking/a' alt='image description'></p>","text/html; charset=utf-8");
    SAME(d->title,"A & B");CHECK(strstr(d->text,"Hello\none two α <three>")!=NULL);
    CHECK(!strstr(d->text,"invisible"));CHECK(!strstr(d->text,"hidden"));CHECK(!strstr(d->text,"secret"));
    CHECK(strstr(d->text,"Read more [1]")!=NULL);CHECK(strstr(d->text,"image description")!=NULL);
    CHECK(d->count==1);SAME(d->links[0].url,"https://example.org/other/next?q=1&b=2");SAME(d->links[0].label,"Read more");
    CHECK(d->links[0].offset<strlen(d->text));release(d);
    d=parse("<p><a href='next'>Unclosed","text/html");CHECK(d->count==1);CHECK(strstr(d->text,"Unclosed [1]"));release(d);
    d=parse("a &amp; b\n  indented\nα","text/plain");SAME(d->text,"a &amp; b\n  indented\nα");release(d);
    d=parse("<pre>a\n  b</pre>","text/html");SAME(d->text,"a\n  b\n");release(d);
    d=parse("<p>caf\xe9</p>","text/html; charset=iso-8859-1");CHECK(strstr(d->text,"café"));release(d);
    d=parse("<p>&#0; &#xD800; &#x110000; &#999999999999999999999;</p>","text/html");CHECK(strlen(d->text)<100);release(d);
    d=parse("<script>never closes", "text/html");CHECK(strstr(d->text,"no readable text"));release(d);
    char *large=malloc(BROWSER_PAGE_MAX+1);memset(large,'x',BROWSER_PAGE_MAX);large[BROWSER_PAGE_MAX]=0;
    d=parse(large,"text/plain");CHECK(d->shortened);CHECK(strlen(d->text)<BROWSER_TEXT_MAX);CHECK(strstr(d->text,"Page shortened"));release(d);free(large);
    char many[24000]="";
    for(int i=0;i<280;i++)strcat(many,"<a href='next'>link</a> ");
    d=parse(many,"text/html");CHECK(d->count==BROWSER_LINKS_MAX);CHECK(d->links_omitted);release(d);
    browser_document empty;char error[256];
    CHECK(browser_document_parse(&empty,"%PDF",4,"https://a/","application/pdf",error,sizeof(error))<0);
    CHECK(browser_document_parse(&empty,"a\0b",3,"https://a/","text/plain",error,sizeof(error))<0);
    CHECK(browser_document_parse(&empty,"a",BROWSER_PAGE_MAX+1,"https://a/","text/plain",error,sizeof(error))<0);
    CHECK(browser_document_parse(&empty,"a",1,"file:///a","text/plain",error,sizeof(error))<0);
}
static void histories(void)
{
    browser_history h={0};CHECK(!browser_history_back(&h));
    CHECK(browser_history_commit(&h,NAV_BACK,"https://a/")<0);
    CHECK(browser_history_commit(&h,NAV_NEW,"https://a/one")==0);
    h.visits[h.current].scroll=12;
    CHECK(browser_history_commit(&h,NAV_NEW,"https://a/two")==0);SAME(browser_history_back(&h),"https://a/one");
    CHECK(browser_history_commit(&h,NAV_BACK,"https://a/one")==0);CHECK(h.visits[h.current].scroll==12);
    CHECK(browser_history_commit(&h,NAV_NEW,"https://a/three")==0);CHECK(h.count==2);
    CHECK(browser_history_commit(&h,NAV_RELOAD,"https://a/redirected")==0);CHECK(h.count==2);
    int previous=h.current;CHECK(browser_history_commit(&h,NAV_NEW,"javascript:bad")<0);CHECK(h.current==previous&&h.count==2);
    for(int i=0;i<20;i++){char url[128];snprintf(url,sizeof(url),"https://a/%d",i);CHECK(browser_history_commit(&h,NAV_NEW,url)==0);}
    CHECK(h.count==BROWSER_HISTORY_MAX);CHECK(h.current==BROWSER_HISTORY_MAX-1);SAME(h.visits[0].url,"https://a/8");
}
static void malformed(void)
{
    unsigned state=1234;
    for(int test=0;test<300;test++) {
        char input[4096];size_t length=(size_t)(test*13)%sizeof(input);
        const char alphabet[]="<>/='\"&; abcdefghijklmnopqrstuvwxyz0123456789\n\t";
        for(size_t i=0;i<length;i++){state=state*1664525u+1013904223u;input[i]=alphabet[state%(sizeof(alphabet)-1)];}
        browser_document *doc=calloc(1,sizeof(*doc));char error[256];
        int result=browser_document_parse(doc,input,length,"https://example.org/","text/html",error,sizeof(error));
        CHECK(result==0);CHECK(doc->text&&strlen(doc->text)<BROWSER_TEXT_MAX);CHECK(doc->count<=BROWSER_LINKS_MAX);
        for(int i=0;i<doc->count;i++)CHECK(doc->links[i].offset<=strlen(doc->text));
        release(doc);
    }
}
int main(void)
{
    urls();documents();histories();malformed();
    printf("browser: %d checks, %d failures\n",checks,failures);return failures?1:0;
}

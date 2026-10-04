/* document.cookie: the cookies a page's scripts see and set, in libcurl's
   cookie store (PluginManager/src/jar.c). */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "jar.h"
static int checks, failures;
#define CHECK(test) do{checks++;if(!(test)){failures++;fprintf(stderr,"%d: %s\n",__LINE__,#test);}}while(0)

static CURLSH *share;
static time_t now;
static const char *seen(const char *url)
{
    static char out[1024];
    if (jar_cookie_string(share, url, now, out, sizeof(out)) < 0) return "(error)";
    return out;
}
#define SEES(url, expected) do { const char *got_ = seen(url); if (strcmp(got_, expected)) fprintf(stderr, "  %s -> \"%s\"\n", url, got_); CHECK(!strcmp(got_, expected)); } while (0)

int main(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    share = curl_share_init();
    curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    now = time(NULL);
    /* a cookie for the page's own host and directory */
    CHECK(jar_cookie_set(share, "https://www.example.com/dir/page?x=1", "a=1", now) == 0);
    SEES("https://www.example.com/dir/other", "a=1");
    SEES("https://www.example.com/dir", "a=1");
    SEES("https://www.example.com/directory", "");
    SEES("https://www.example.com/", "");
    SEES("https://example.com/dir/x", "");
    SEES("https://shop.www.example.com/dir/x", "");
    /* for the whole domain, and the whole site */
    CHECK(jar_cookie_set(share, "https://www.example.com/dir/page", " b = 2 ; Domain=.Example.COM; path=/", now) == 0);
    SEES("https://shop.example.com/", "b=2");
    SEES("http://example.com/x/y", "b=2");
    SEES("https://www.example.com/dir/x", "a=1; b=2");
    SEES("https://notexample.com/", "");
    /* not another site's, a server's HttpOnly, or a Secure one from http */
    CHECK(jar_cookie_set(share, "https://www.example.com/", "c=3; domain=other.com", now) < 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "c=3; domain=com.example", now) < 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "c=3; HttpOnly", now) < 0);
    CHECK(jar_cookie_set(share, "http://www.example.com/", "c=3; Secure", now) < 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "d=4; Secure; path=/", now) == 0);
    SEES("https://www.example.com/", "b=2; d=4");
    SEES("http://www.example.com/", "b=2");
    /* a server's HttpOnly cookie stays hidden from scripts */
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_SHARE, share);
    curl_easy_setopt(c, CURLOPT_COOKIELIST, "#HttpOnly_.example.com\tTRUE\t/\tFALSE\t0\tsession\tsecret");
    curl_easy_setopt(c, CURLOPT_COOKIELIST, ".example.com\tTRUE\t/\tFALSE\t0\tseen\tyes");
    curl_easy_cleanup(c);
    SEES("http://example.com/", "b=2; seen=yes");
    /* expiry: max-age and expires; 0 or the past deletes */
    CHECK(jar_cookie_set(share, "https://www.example.com/", "e=5; max-age=3600", now) == 0);
    SEES("https://www.example.com/", "b=2; d=4; seen=yes; e=5");
    CHECK(jar_cookie_set(share, "https://www.example.com/", "e=; max-age=0", now) == 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "f=6; expires=Thu, 01 Jan 1970 00:00:01 GMT", now) == 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "g=7; expires=Fri, 01 Jan 2100 00:00:00 GMT", now) == 0);
    SEES("https://www.example.com/", "b=2; d=4; seen=yes; g=7");
    /* setting again replaces */
    CHECK(jar_cookie_set(share, "https://www.example.com/dir/page", "a=one", now) == 0);
    SEES("https://www.example.com/dir/x", "a=one; b=2; d=4; seen=yes; g=7");
    /* what isn't a cookie, or a page that can't have them */
    CHECK(jar_cookie_set(share, "https://www.example.com/", "novalue", now) < 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "=nameless", now) < 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "tab=a\tb", now) < 0);
    CHECK(jar_cookie_set(share, "https://www.example.com/", "nl=a\nb", now) < 0);
    CHECK(jar_cookie_set(share, "javascript:alert(1)", "x=1", now) < 0);
    CHECK(jar_cookie_set(share, "ftp://example.com/", "x=1", now) < 0);
    CHECK(!strcmp(seen("about:blank"), "(error)"));
    char tiny[4];
    CHECK(jar_cookie_string(share, "https://www.example.com/dir/x", now, tiny, sizeof(tiny)) == 0 && !strcmp(tiny, ""));
    /* a user name in the address isn't the host */
    SEES("https://user@www.example.com/dir/x", "a=one; b=2; d=4; seen=yes; g=7");
    curl_share_cleanup(share);
    curl_global_cleanup();
    printf("jar: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/* Native fixture for the exact protocol policy used by both PSP applications. */
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include "../../PluginManager/src/http_policy.h"
static size_t discard(char *data,size_t size,size_t count,void *ud)
{
    (void)data;(void)ud;return size*count;
}
int main(int argc,char **argv)
{
    if(argc<3)return 2;
    curl_global_init(CURL_GLOBAL_ALL);
    CURL *curl=curl_easy_init();
    CURLcode result=pm_http_policy(curl,argv[1],"ARKBrowser-test",1);
    if(result!=CURLE_OK)return 2;
    curl_easy_setopt(curl,CURLOPT_URL,argv[1]);
    curl_easy_setopt(curl,CURLOPT_PROXY,""); /* local test fixtures only */
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,discard);
    if(argv[2][0])curl_easy_setopt(curl,CURLOPT_CAINFO,argv[2]);
    if(argc>3) {
        /* Control case: prove the test's old-TLS server can negotiate without the policy. */
        curl_easy_setopt(curl,CURLOPT_SSLVERSION,(long)(CURL_SSLVERSION_TLSv1_0|CURL_SSLVERSION_MAX_TLSv1_0));
        curl_easy_setopt(curl,CURLOPT_SSL_CIPHER_LIST,"ALL:@SECLEVEL=0");
    }
    result=curl_easy_perform(curl);
    char *effective=NULL;curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);
    if(effective)printf("%s\n",effective);
    curl_easy_cleanup(curl);curl_global_cleanup();
    return result==CURLE_OK?0:1;
}

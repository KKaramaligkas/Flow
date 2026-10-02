#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include "jobs.h"
#include "net.h"
#include "util.h"
#include "fs.h"
#include "script.h"

browser_job browser_work;
static SceUID thread=-1, wake=-1;
static volatile int quit;
static char page_cache[256];
static int progress(void *ud, int64_t done, int64_t total)
{
    (void)ud; browser_work.done=done; browser_work.total=total;
    return quit || browser_work.cancel;
}
static int cancelled(void *ud) { (void)ud; return quit || browser_work.cancel; }
static int read_page(void *ud,char *out,size_t size) { return fs_read(*(fs_file *)ud,out,(int)size); }
static char *fetch_same_origin(void *ud,const char *url,int max,int *len,char *err,size_t errlen)
{
    browser_document *doc=ud; net_response response;
    if (!browser_same_origin(doc->url,url)) return NULL;
    char *data=net_get_info(url,max,len,&response,progress,NULL,err,(int)errlen);
    if (data && !browser_same_origin(doc->url,response.url)) { free(data); data=NULL; }
    return data;
}
static void assets(browser_document *doc)
{
    int requests=0; size_t script_bytes=doc->dom->script_bytes, css_bytes=0;
    char base[BROWSER_URL_MAX]; strcpy(base,doc->url);
    for(int i=0;i<doc->dom->count;i++) if(!strcmp(doc->dom->nodes[i].tag,"base")) {
        char resolved[BROWSER_URL_MAX];
        if(browser_url_resolve(base,dom_attr(&doc->dom->nodes[i],"href"),resolved,sizeof(resolved))==0) strcpy(base,resolved);
        break;
    }
    for(int i=0;i<doc->dom->count&&!cancelled(NULL);i++) {
        dom_node *n=&doc->dom->nodes[i]; int script=!strcmp(n->tag,"script");
        int css=!strcmp(n->tag,"link")&&!strcmp(dom_attr(n,"rel"),"stylesheet");
        if(!script&&!css)continue;
        const char *ref=dom_attr(n,script?"src":"href"); if(!*ref||script&&!browser_work.javascript)continue;
        char target[BROWSER_URL_MAX],err[256]; int limit=script?512*1024:64*1024;
        size_t left=script?1024*1024-script_bytes:128*1024-css_bytes;
        if(requests++>=16||!left||browser_url_resolve(base,ref,target,sizeof(target))<0||
           browser_url_secure(doc->url)&&!browser_url_secure(target)||script&&!browser_same_origin(doc->url,target)) { doc->assets_omitted++; continue; }
        if((size_t)limit>left)limit=(int)left;
        net_response response;int length=0;
        char *data=net_get_info(target,limit,&length,&response,progress,NULL,err,sizeof(err));
        if(data&&(browser_url_secure(doc->url)&&!browser_url_secure(response.url)||script&&!browser_same_origin(doc->url,response.url))) { free(data); data=NULL; }
        if(!data||memchr(data,0,(size_t)length)) {free(data);doc->assets_omitted++;continue;}
        if(css && (size_t)length+1>DOM_BYTES_MAX-doc->dom->bytes) {free(data);doc->assets_omitted++;continue;}
        if(script)script_bytes+=(size_t)length;else{css_bytes+=(size_t)length;doc->dom->bytes+=(size_t)length+1;}
        free(n->text); n->text=data;
    }
}
static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while(!quit) {
        if(sceKernelWaitSema(wake,1,NULL)<0 || quit) break;
        if(browser_work.type==BROWSER_JOB_DOWNLOAD) {
            browser_work.result=net_download(browser_work.url,browser_work.destination,progress,NULL,
                                            browser_work.error,sizeof(browser_work.error));
        } else {
            net_response response;
            int length=net_get_file(browser_work.url,page_cache,BROWSER_PAGE_MAX,&response,progress,NULL,
                                    browser_work.error,sizeof(browser_work.error));
            browser_work.result=-1;
            if(length>=0 && !cancelled(NULL)) {
                browser_document *doc=calloc(1,sizeof(*doc)); fs_file f=fs_open(page_cache,FS_READ);
                if(!doc) snprintf(browser_work.error,sizeof(browser_work.error),"Not enough memory to open the page.");
                else if(f<0 || browser_document_read(doc,read_page,&f,response.url,response.content_type,cancelled,NULL,
                                              browser_work.error,sizeof(browser_work.error))<0) {
                    browser_document_free(doc); free(doc); doc=NULL;
                }
                if(f>=0)fs_close(f);
                if(doc) {
                    doc->source_bytes=(size_t)length;
                    assets(doc);
                    if(browser_document_render(doc,browser_work.error,sizeof(browser_work.error))<0) {
                        browser_document_free(doc);free(doc);
                    } else {
                        if(browser_work.javascript)browser_script_run(doc,fetch_same_origin,doc,cancelled,NULL,30000,
                                                        browser_work.error,sizeof(browser_work.error));
                        browser_work.page=doc;browser_work.result=0;
                    }
                }
            }
            fs_remove(page_cache);
        }
        if(browser_work.cancel || quit) browser_work.result=-1;
        browser_work.running=0; browser_work.finished=1;
    }
    return 0;
}
int browser_jobs_start(const char *cache)
{
    if(pm_strlcpy(page_cache,cache,sizeof(page_cache))>=sizeof(page_cache))return -1;
    quit=0;
    wake=sceKernelCreateSema("arkb_jobs",0,0,1,NULL);
    if(wake<0) return -1;
    thread=sceKernelCreateThread("arkb_worker",worker,0x30,512*1024,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);
    if(thread<0 || sceKernelStartThread(thread,0,NULL)<0) {
        if(thread>=0) sceKernelDeleteThread(thread);
        sceKernelDeleteSema(wake); thread=wake=-1; return -1;
    }
    return 0;
}
void browser_jobs_stop(void)
{
    if(thread<0) return;
    quit=browser_work.cancel=1; sceKernelSignalSema(wake,1);
    /* Network timeouts are finite; let libcurl finish before freeing TLS state. */
    sceKernelWaitThreadEnd(thread,NULL);
    sceKernelDeleteThread(thread); sceKernelDeleteSema(wake); thread=wake=-1;
    if(browser_work.page) { browser_document_free(browser_work.page); free(browser_work.page); browser_work.page=NULL; }
}
int browser_jobs_busy(void) { return browser_work.running || browser_work.finished; }
int browser_jobs_submit(int type, int navigation, const char *url, const char *destination,int javascript)
{
    if(browser_jobs_busy() || thread<0 || strlen(url)>=sizeof(browser_work.url) ||
        (destination && strlen(destination)>=sizeof(browser_work.destination))) return -1;
    memset(&browser_work,0,sizeof(browser_work));
    browser_work.javascript=javascript; browser_work.type=type; browser_work.navigation=navigation; browser_work.total=-1;
    strcpy(browser_work.url,url);
    if(destination) strcpy(browser_work.destination,destination);
    browser_work.running=1; sceKernelSignalSema(wake,1); return 0;
}

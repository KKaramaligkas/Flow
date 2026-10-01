#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include "jobs.h"
#include "net.h"
#include "util.h"

browser_job browser_work;
static SceUID thread=-1, wake=-1;
static volatile int quit;
static int progress(void *ud, int64_t done, int64_t total)
{
    (void)ud; browser_work.done=done; browser_work.total=total;
    return quit || browser_work.cancel;
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
            net_response response; int length=0;
            char *data=net_get_info(browser_work.url,BROWSER_PAGE_MAX,&length,&response,progress,NULL,
                                    browser_work.error,sizeof(browser_work.error));
            browser_work.result=-1;
            if(!data && strstr(browser_work.error,"too large"))
                snprintf(browser_work.error,sizeof(browser_work.error),"Page exceeds 512 KB. Use Download to save the file instead.");
            if(data && !quit && !browser_work.cancel) {
                browser_document *doc=calloc(1,sizeof(*doc));
                if(!doc) snprintf(browser_work.error,sizeof(browser_work.error),"Not enough memory to open the page.");
                else if(browser_document_parse(doc,data,(size_t)length,response.url,response.content_type,
                                              browser_work.error,sizeof(browser_work.error))<0) {
                    browser_document_free(doc); free(doc);
                } else { browser_work.page=doc; browser_work.result=0; }
            }
            free(data);
        }
        if(browser_work.cancel || quit) browser_work.result=-1;
        browser_work.running=0; browser_work.finished=1;
    }
    return 0;
}
int browser_jobs_start(void)
{
    wake=sceKernelCreateSema("arkb_jobs",0,0,1,NULL);
    if(wake<0) return -1;
    thread=sceKernelCreateThread("arkb_worker",worker,0x30,256*1024,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);
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
int browser_jobs_submit(int type, int navigation, const char *url, const char *destination)
{
    if(browser_jobs_busy() || thread<0 || strlen(url)>=sizeof(browser_work.url) ||
        (destination && strlen(destination)>=sizeof(browser_work.destination))) return -1;
    memset(&browser_work,0,sizeof(browser_work));
    browser_work.type=type; browser_work.navigation=navigation; browser_work.total=-1;
    strcpy(browser_work.url,url);
    if(destination) strcpy(browser_work.destination,destination);
    browser_work.running=1; sceKernelSignalSema(wake,1); return 0;
}

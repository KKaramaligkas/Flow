#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include "jobs.h"
#include "net.h"
#include "util.h"
#include "fs.h"
#include "script.h"
#include "view.h"
#include "text.h"

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
/* Lays out the page here, off the main thread: text_measure() is safe in any thread. */
static float measure(const char *s,int len,float scale,int flags) { return text_measure(s,len,scale,(flags&CSS_BOLD)?TEXT_BOLD:0); }
void browser_build_view(browser_document *doc)
{
    char err[256];
    browser_view *view=calloc(1,sizeof(*view));
    if(!view) return;
    if(browser_view_build(view,doc,VIEW_WIDTH,measure,err,sizeof(err))<0) { free(view); return; }
    if(doc->view) { browser_view_free(doc->view); free(doc->view); }
    doc->view=view;
}
static char *fetch_same_origin(void *ud,const char *url,int max,int *len,char *err,size_t errlen)
{
    browser_document *doc=ud; net_response response;
    if (!browser_same_origin(doc->url,url)) return NULL;
    char *data=net_get_info(url,max,len,&response,progress,NULL,err,(int)errlen);
    if (data && !browser_same_origin(doc->url,response.url)) { free(data); data=NULL; }
    return data;
}
/* Assets come over the network, with the progress shown while loading. */
static char *fetch_asset(void *ud,const char *url,int max,int *length,char *final)
{
    (void)ud; net_response response; char err[256];
    char *data=net_get_info(url,max,length,&response,progress,NULL,err,sizeof(err));
    if(data) pm_strlcpy(final,response.url,BROWSER_URL_MAX);
    return data;
}
#ifdef PM_AUTOTEST
/* Test builds log how long each step of a page load takes. */
static unsigned phase_start;
static void phase(const char *name)
{
    unsigned now=sceKernelGetSystemTimeLow();
    FILE *f=fopen("ms0:/arkb_timing.txt","a");
    if(f){fprintf(f,"%s %u ms\n",name,(now-phase_start)/1000);fclose(f);}
    phase_start=sceKernelGetSystemTimeLow();
}
#else
#define phase(name) ((void)0)
#endif
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
#ifdef PM_AUTOTEST
            phase_start=sceKernelGetSystemTimeLow();
#endif
            int length=browser_work.post?
                net_post_file(browser_work.url,browser_work.post,page_cache,BROWSER_PAGE_MAX,&response,progress,NULL,
                              browser_work.error,sizeof(browser_work.error)):
                net_get_file(browser_work.url,page_cache,BROWSER_PAGE_MAX,&response,progress,NULL,
                             browser_work.error,sizeof(browser_work.error));
            browser_work.result=-1;
            phase("download");
            if(length>=0 && !cancelled(NULL)) {
                browser_document *doc=calloc(1,sizeof(*doc)); fs_file f=fs_open(page_cache,FS_READ);
                if(!doc) snprintf(browser_work.error,sizeof(browser_work.error),"Not enough memory to open the page.");
                else if(f<0 || browser_document_load(doc,read_page,&f,response.url,response.content_type,cancelled,NULL,
                                              browser_work.error,sizeof(browser_work.error))<0) {
                    browser_document_free(doc); free(doc); doc=NULL;
                }
                if(f>=0)fs_close(f);
                phase("parse");
                if(doc) {
                    doc->source_bytes=(size_t)length;
                    doc->scripting=browser_work.javascript;
                    browser_assets(doc,browser_work.javascript,fetch_asset,NULL,cancelled,NULL);
                    phase("assets");
                    /* Scripts first: a page they change is styled once, by them. */
                    if(browser_work.javascript)browser_script_run(doc,fetch_same_origin,doc,cancelled,NULL,30000,
                                                    browser_work.error,sizeof(browser_work.error));
                    phase("scripts");
                    if(doc->rendered!=doc->dom&&browser_document_render(doc,browser_work.error,sizeof(browser_work.error))<0) {
                        browser_document_free(doc);free(doc);
                    } else {
                        phase("render");
                        if(!cancelled(NULL)) browser_build_view(doc);
                        phase("view");
                        browser_work.page=doc;browser_work.result=0;
                    }
                }
            }
            fs_remove(page_cache);
        }
        if(browser_work.cancel || quit) browser_work.result=-1;
        free(browser_work.post); browser_work.post=NULL;
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
int browser_jobs_submit(int type, int navigation, const char *url, const char *destination,int javascript,const char *post)
{
    if(browser_jobs_busy() || thread<0 || strlen(url)>=sizeof(browser_work.url) ||
        (destination && strlen(destination)>=sizeof(browser_work.destination))) return -1;
    char *body=NULL;
    if(post && !(body=strdup(post))) return -1;
    memset(&browser_work,0,sizeof(browser_work));
    browser_work.post=body;
    browser_work.javascript=javascript; browser_work.type=type; browser_work.navigation=navigation; browser_work.total=-1;
    strcpy(browser_work.url,url);
    if(destination) strcpy(browser_work.destination,destination);
    browser_work.running=1; sceKernelSignalSema(wake,1); return 0;
}

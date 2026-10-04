#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include "jobs.h"
#include "net.h"
#include "util.h"
#include "fs.h"
#include "picture.h"
#include "script.h"
#include "view.h"
#include "text.h"

#define BARRIER() __asm__ __volatile__("" ::: "memory")
#define PICTURE_RESERVE (3*1024*1024)   /* memory a picture leaves for the rest of the browser */
#define SCREEN_ROWS 238

browser_job browser_work;
static SceUID thread=-1, wake=-1;
static volatile int quit;
static char page_cache[256];
/* Pictures: the page shown (set by the main thread), the page of the
   worker's current step, and a view laid out again for the main thread. */
static size_t picture_budget=4*1024*1024;
static browser_document *volatile picture_page;
static browser_document *picture_doc;
static volatile int picture_busy, view_top, view_bottom=SCREEN_ROWS;
static browser_view *volatile picture_view;
static browser_document *volatile picture_view_page;
static unsigned last_layout, layout_cost;
static int near_learned;        /* a size came for a picture near the screen since the last layout */
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
    if(!doc->pictures&&doc->dom&&doc->dom->count>0) doc->pictures=picture_table_new(picture_budget,doc->dom->count);
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
    FILE *f=fopen("ms0:/flow_timing.txt","a");
    if(f){fprintf(f,"%s %u ms\n",name,(now-phase_start)/1000);fclose(f);}
    phase_start=sceKernelGetSystemTimeLow();
}
#else
#define phase(name) ((void)0)
#endif
/* ---- pictures ---- */

/* A picture's download stops for a job, or when the page isn't shown. */
static int picture_cancelled(void *ud,int64_t done,int64_t total)
{
    (void)ud; (void)done; (void)total;
    return quit || picture_page!=picture_doc || browser_work.running;
}
/* Fetches and decodes entry i; it stays waiting when that was stopped. */
static void load_picture(picture_table *t,int i)
{
    picture_entry *e=&t->entries[i];
    unsigned char *data=NULL; size_t length=0; int ok=0;
#ifdef PM_AUTOTEST
    unsigned start=sceKernelGetSystemTimeLow();
#endif
    if(!strncmp(e->url,"data:",5)) data=picture_data(e->url,&length);
    else {
        net_response response; char err[160]; int n=0;
        data=(unsigned char *)net_get_info(e->url,PICTURE_BYTES_MAX,&n,&response,picture_cancelled,NULL,err,sizeof(err));
        length=(size_t)n;
        if(!data&&picture_cancelled(NULL,0,0)) return;
    }
#ifdef PM_AUTOTEST
    unsigned fetched=sceKernelGetSystemTimeLow();
#endif
    if(data) {
        /* decoded only with memory to spare for the rest of the browser */
        size_t need=picture_decode_memory(data,length);
        void *room=need?malloc(need+PICTURE_RESERVE):NULL;
        if(room) { free(room); ok=picture_load(t,i,data,length)==0; }
    }
    free(data);
    if(ok) sceKernelDcacheWritebackRange(e->image.pixels,picture_bytes(&e->image));
#ifdef PM_AUTOTEST
    FILE *f=fopen("ms0:/flow_timing.txt","a");
    if(f){fprintf(f,"picture %s %u+%u ms, %u bytes, %dx%d of %dx%d: %.120s\n",ok?"ok":"failed",(fetched-start)/1000,
                  (sceKernelGetSystemTimeLow()-fetched)/1000,(unsigned)length,e->image.w,e->image.h,e->natural_w,e->natural_h,e->url);fclose(f);}
#endif
    BARRIER();
    e->state=ok?PICTURE_READY:PICTURE_FAILED;
}
/* Lays the page out again with its pictures' sizes, for the main thread. */
static void relayout(browser_document *doc)
{
    char err[160];
    unsigned start=sceKernelGetSystemTimeLow();
    doc->pictures->learned=0;
    browser_view *view=calloc(1,sizeof(*view));
    if(!view) return;
    if(browser_view_build(view,doc,VIEW_WIDTH,measure,err,sizeof(err))<0) { free(view); return; }
    last_layout=sceKernelGetSystemTimeLow(); layout_cost=last_layout-start;
#ifdef PM_AUTOTEST
    FILE *f=fopen("ms0:/flow_timing.txt","a");
    if(f){fprintf(f,"relayout %u ms, %u KB of pictures\n",layout_cost/1000,(unsigned)(doc->pictures->bytes/1024));fclose(f);}
#endif
    if(picture_page!=doc) { browser_view_free(view); free(view); return; }
    picture_view_page=doc;
    BARRIER();
    picture_view=view;
}
static int near_screen(int y,int top,int bottom) { return y>=top-SCREEN_ROWS&&y<bottom+SCREEN_ROWS; }
/* One picture of the page shown, and the page laid out again when sizes it
   was waiting for came: once those for the screen are in, when all are, or
   now and then while the rest load. 1 when there was something to do. */
static int pictures_step(void)
{
    browser_document *doc=picture_page;
    if(!doc||!doc->pictures||browser_work.running) return 0;
    picture_busy=1;
    BARRIER();
    if(picture_page!=doc||browser_work.running) { picture_busy=0; return 0; }
    unsigned now=sceKernelGetSystemTimeLow();
    if(picture_doc!=doc) { picture_doc=doc; last_layout=now; near_learned=0; }
    picture_table *t=doc->pictures;
    int top=view_top,bottom=view_bottom,did=0,i=picture_next(t,top,bottom),learned=t->learned;
    if(i>=0) {
        load_picture(t,i); did=1;
        if(t->learned>learned&&near_screen(t->entries[i].top,top,bottom)) near_learned=1;
    }
    if(t->learned&&!picture_view&&picture_page==doc&&!browser_work.running) {
        int next=picture_next(t,top,bottom);
        unsigned pause=layout_cost*4>2500000?layout_cost*4:2500000;
        now=sceKernelGetSystemTimeLow();
        if(next<0||(near_learned&&!near_screen(t->entries[next].top,top,bottom))||now-last_layout>pause) {
            relayout(doc); near_learned=0; did=1;
        }
    }
    BARRIER();
    picture_busy=0;
    return did;
}
void browser_pictures(browser_document *doc)
{
    if(picture_page==doc) return;
    picture_page=doc;
    if(doc&&wake>=0) sceKernelSignalSema(wake,1);
}
int browser_pictures_stop(int ms)
{
    picture_page=NULL;
    for(int waited=0;picture_busy;waited+=10) {
        if(ms>=0&&waited>=ms) return -1;
        sceKernelDelayThread(10000);
    }
    return 0;
}
void browser_pictures_release(browser_document *doc)
{
    picture_table *t=doc?doc->pictures:NULL;
    for(int i=0;t&&i<t->count;i++) {
        picture_entry *e=&t->entries[i];
        if(e->state!=PICTURE_READY) continue;
        e->state=PICTURE_WAITING;
        t->bytes-=picture_bytes(&e->image);
        picture_free(&e->image);
    }
}
void browser_pictures_viewport(int top,int bottom) { view_top=top; view_bottom=bottom; }
browser_view *browser_pictures_view(browser_document *doc)
{
    browser_view *v=picture_view;
    if(!v||picture_view_page!=doc) return NULL;
    picture_view=NULL;
    if(wake>=0) sceKernelSignalSema(wake,1);    /* it may wait to lay the page out again */
    return v;
}
/* Before a page is freed: a view laid out for it isn't taken. */
void browser_pictures_forget(void)
{
    browser_view *v=picture_view;
    picture_view=NULL; picture_view_page=NULL;
    if(v) { browser_view_free(v); free(v); }
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while(!quit) {
        /* pictures between jobs */
        if(!browser_work.running) {
            if(pictures_step()) continue;
            if(sceKernelWaitSema(wake,1,NULL)<0) break;
            continue;
        }
        sceKernelPollSema(wake,1);
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
    /* PSP-2000 and later have twice the memory: more of it for pictures */
    void *probe=malloc(24*1024*1024);
    picture_budget=(probe?12:4)*1024*1024;
    free(probe);
    wake=sceKernelCreateSema("flow_jobs",0,0,1,NULL);
    if(wake<0) return -1;
    thread=sceKernelCreateThread("flow_worker",worker,0x30,512*1024,PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);
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
    picture_page=NULL; browser_pictures_forget();
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

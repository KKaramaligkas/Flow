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
#include "session.h"
#include "view.h"
#include "text.h"

#define BARRIER() __asm__ __volatile__("" ::: "memory")
#define PICTURE_RESERVE (3*1024*1024)   /* memory a picture leaves for the rest of the browser */
#define SCREEN_ROWS 238
#define REDIRECTS_MAX 5                 /* pages sending the browser on, as they load */
#define REFRESH_SECONDS 10              /* a <meta> refresh followed */
#define CLICK_BUDGET_MS 10000

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
static volatile int relayout_wanted;
static int roomy;
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
    /* by source_id, which stays the same for an element when scripts change the page */
    if(!doc->pictures&&doc->dom&&doc->dom->count>0) doc->pictures=picture_table_new(picture_budget,DOM_NODES_MAX);
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
    if(relayout_wanted&&!picture_view) {
        relayout_wanted=0; relayout(doc);
        BARRIER(); picture_busy=0;
        return 1;
    }
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
void browser_relayout(void) { relayout_wanted=1; if(wake>=0) sceKernelSignalSema(wake,1); }
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

/* ---- pages ---- */

/* Whether `target` is `page` itself, or with `fragment_only` a part of it
   (an address with a #fragment): nothing to load. */
static int same_page(const char *page,const char *target,int fragment_only)
{
    size_t n=strcspn(page,"#"),m=strcspn(target,"#");
    return n==m&&!strncmp(page,target,n)&&(!fragment_only||target[m]=='#');
}
/* Downloads a page (POST with `post`), reads it, runs its scripts and lays
   it out, unless it sends the browser on (doc->redirect): NULL when that
   fails, with the reason in browser_work.error. */
static browser_document *load_page(const char *url,const char *post)
{
    net_response response;
    browser_document *doc=NULL;
#ifdef PM_AUTOTEST
    phase_start=sceKernelGetSystemTimeLow();
#endif
    int length=post?
        net_post_file(url,post,page_cache,BROWSER_PAGE_MAX,&response,progress,NULL,browser_work.error,sizeof(browser_work.error)):
        net_get_file(url,page_cache,BROWSER_PAGE_MAX,&response,progress,NULL,browser_work.error,sizeof(browser_work.error));
    phase("download");
    if(length>=0 && !cancelled(NULL)) {
        fs_file f=fs_open(page_cache,FS_READ);
        doc=calloc(1,sizeof(*doc));
        if(!doc) snprintf(browser_work.error,sizeof(browser_work.error),"Not enough memory to open the page.");
        else if(f<0 || browser_document_load(doc,read_page,&f,response.url,response.content_type,cancelled,NULL,
                                          browser_work.error,sizeof(browser_work.error))<0) {
            browser_document_free(doc); free(doc); doc=NULL;
        }
        if(f>=0)fs_close(f);
        phase("parse");
        if(doc) {
            doc->source_bytes=(size_t)length;
            doc->scripting=browser_work.javascript&&browser_scripts_present(doc);
            browser_assets(doc,doc->scripting,fetch_asset,NULL,cancelled,NULL);
            phase("assets");
            /* Scripts first: a page they change is styled once, by them. */
            if(doc->scripting) {
                browser_script_run(doc,fetch_same_origin,doc,cancelled,NULL,30000,browser_work.error,sizeof(browser_work.error));
                if(!doc->scripts_run) doc->scripting=0;     /* none could run: as without JavaScript */
            }
            phase("scripts");
            if(doc->rendered!=doc->dom&&browser_document_render(doc,browser_work.error,sizeof(browser_work.error))<0) {
                browser_document_free(doc); free(doc); doc=NULL;
            } else {
                phase("render");
                /* where it sends the browser on to: not to a #fragment of
                   itself, and a refresh of itself isn't followed */
                browser_redirect *r=&doc->redirect;
                if(r->url[0]&&!r->post&&same_page(doc->url,r->url,1)) r->url[0]=0;
                if(!r->url[0]&&browser_meta_refresh(doc,REFRESH_SECONDS,r->url,sizeof(r->url))&&same_page(doc->url,r->url,0)) r->url[0]=0;
                if(!cancelled(NULL)&&!r->url[0]) browser_build_view(doc);
                phase("view");
            }
        }
    }
    fs_remove(page_cache);
    return doc;
}
/* browser_work.url, and the pages it sends the browser on to as they load:
   their scripts' navigation and <meta> refreshes. */
static void page_job(void)
{
    char url[BROWSER_URL_MAX];
    pm_strlcpy(url,browser_work.url,sizeof(url));
    char *post=browser_work.post; browser_work.post=NULL;
    browser_document *doc=NULL;
    for(int hop=0;;hop++) {
        doc=load_page(url,post);
        free(post); post=NULL;
        if(!doc||cancelled(NULL)) break;
        browser_redirect *r=&doc->redirect;
        if(!r->url[0]) break;
        if(hop==REDIRECTS_MAX) { r->url[0]=0; free(r->post); r->post=NULL; browser_build_view(doc); break; }
        pm_strlcpy(url,r->url,sizeof(url)); pm_strlcpy(browser_work.url,url,sizeof(browser_work.url));
        post=r->post; r->post=NULL;
        browser_document_free(doc); free(doc); doc=NULL;
    }
    free(post);
    if(doc) { browser_work.page=doc; browser_work.result=0; }
}
/* An element of the page shown whose scripts handle clicks: what they do
   with it. The page itself isn't changed: the main thread shows it meanwhile. */
static void click_job(void)
{
    browser_document *doc=browser_work.target;
    browser_dom *changed=NULL; browser_redirect to;
    if(browser_script_click(doc,browser_work.node,browser_work.values,fetch_same_origin,doc,cancelled,NULL,CLICK_BUDGET_MS,
                            &changed,&to,browser_work.error,sizeof(browser_work.error))<0) return;
    if(to.url[0]&&!to.post&&same_page(doc->url,to.url,1)) to.url[0]=0;
    if(to.url[0]) {
        /* to another page: this one's scripts are done, and their memory is the next page's */
        browser_script_free(doc);
        pm_strlcpy(browser_work.url,to.url,sizeof(browser_work.url));
        browser_work.post=to.post; browser_work.navigation=to.replace?NAV_REPLACE:NAV_NEW;
        BARRIER();
        browser_work.type=BROWSER_JOB_PAGE;
        if(changed) { dom_free(changed); free(changed); }
        page_job();
        return;
    }
    free(to.post);
    if(!changed) { browser_work.unchanged=1; browser_work.result=0; return; }
    /* the page as the scripts left it, laid out with the shown page's pictures */
    browser_document *next=calloc(1,sizeof(*next));
    if(!next) { dom_free(changed); free(changed); snprintf(browser_work.error,sizeof(browser_work.error),"Not enough memory for the page's change."); return; }
    strcpy(next->url,doc->url);
    next->dom=changed; next->scripting=doc->scripting; next->source_bytes=doc->source_bytes;
    next->scripts_run=doc->scripts_run; next->assets_omitted=doc->assets_omitted;
    for(int i=0;i<doc->hidden_count;i++) next->hidden[i]=doc->hidden[i];
    next->hidden_count=doc->hidden_count;
    if(browser_document_render(next,browser_work.error,sizeof(browser_work.error))<0) { browser_document_free(next); free(next); return; }
    picture_table_rebind(doc->pictures,doc->dom,next->dom);
    next->pictures=doc->pictures;
    if(!cancelled(NULL)) browser_build_view(next);
    next->pictures=NULL;    /* the main thread hands them over with the scripts */
    browser_work.page=next; browser_work.navigation=NAV_SCRIPT; browser_work.result=0;
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
        } else if(browser_work.type==BROWSER_JOB_CLICK) click_job();
        else page_job();
        if(browser_work.cancel || quit) browser_work.result=-1;
        free(browser_work.post); browser_work.post=NULL;
        free(browser_work.values); browser_work.values=NULL;
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
    picture_budget=(probe?12:4)*1024*1024; roomy=probe!=NULL;
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
int browser_jobs_roomy(void) { return roomy; }
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
int browser_jobs_click(browser_document *doc,int node,const char *values,int javascript)
{
    if(browser_jobs_busy() || thread<0 || !doc || !doc->script) return -1;
    char *copy=NULL;
    if(values && !(copy=strdup(values))) return -1;
    memset(&browser_work,0,sizeof(browser_work));
    browser_work.type=BROWSER_JOB_CLICK; browser_work.navigation=NAV_SCRIPT; browser_work.total=-1;
    browser_work.javascript=javascript; browser_work.target=doc; browser_work.node=node; browser_work.values=copy;
    pm_strlcpy(browser_work.url,doc->url,sizeof(browser_work.url));
    browser_work.running=1; sceKernelSignalSema(wake,1); return 0;
}

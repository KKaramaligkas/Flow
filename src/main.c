/* ARK Browser: direct HTTP(S), styled reader and on-device JavaScript. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>
#include "document.h"
#include "layout.h"
#include "version.h"
#include "session.h"
#include "jobs.h"
#include "entropy.h"
#include "fs.h"
#include "gfx.h"
#include "input.h"
#include "net.h"
#include "text.h"
#include "util.h"

PSP_MODULE_INFO("ARKBrowser",PSP_MODULE_USER,0,2);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU);
PSP_HEAP_THRESHOLD_SIZE_KB(4*1024);
#define PAGE_LINES 16
#define MAX_LINES BROWSER_LINES_MAX
#define BG RGB(12,20,31)
#define FG RGB(224,232,242)
#define DIM RGB(135,158,180)
#define ACCENT RGB(82,188,230)
#define ALERT RGB(255,184,95)

static volatile int exit_requested;
static browser_document *page;
static browser_history history;
static browser_line page_lines[MAX_LINES];
static int javascript=1;
static int line_count, scroll, selected=-1, link_list, menu, menu_selection;
static char app_dir[192], downloads[224], message[256], failed_url[BROWSER_URL_MAX];
static const char *menu_labels[]={"Open address","Back","Reload","Download selected link / page","Connect Wi-Fi","Start page","JavaScript: on/off","Exit"};

static int exit_callback(int a,int b,void *ud)
{
    (void)a; (void)b; (void)ud; exit_requested=1; browser_work.cancel=1; return 0;
}
static int callbacks(SceSize args,void *argp)
{
    (void)args; (void)argp;
    int cb=sceKernelCreateCallback("arkb_exit",exit_callback,NULL);
    sceKernelRegisterExitCallback(cb); sceKernelSleepThreadCB(); return 0;
}
static void notice(const char *text) { pm_strlcpy(message,text,sizeof(message)); }
static void clamp_scroll(void)
{
    int last=line_count;float height=0;
    while(last>0&&height+page_lines[last-1].height<=177){last--;height+=page_lines[last].height;}
    if(last==line_count&&last>0)last--;
    if(scroll<0) scroll=0;
    if(scroll>last) scroll=last;
}
static float measure(void *ud,const char *text,size_t length,browser_style style)
{
    (void)ud; char utf8[8];if(length>=sizeof(utf8))return 0;
    memcpy(utf8,text,length);utf8[length]=0;
    return text_width(utf8,style.scale,(style.flags&CSS_BOLD)?TEXT_BOLD:0);
}
static void layout(void)
{
    line_count=page?browser_layout(page,page_lines,MAX_LINES,452,measure,NULL):0;
    clamp_scroll();
}
static void draw_line(int index,float y)
{
    const browser_line *line=&page_lines[index];size_t end=line->start+line->length,pos=line->start;
    float x=12+(line->align==1?(452-line->width)/2:line->align==2?452-line->width:0);
    while(pos<end) {
        browser_style style=browser_style_at(page,pos);size_t stop=end;
        for(int i=0;i<page->span_count;i++)if(page->spans[i].offset>pos){if(page->spans[i].offset<stop)stop=page->spans[i].offset;break;}
        if(stop-pos>255){stop=pos+255;while(stop>pos&&((unsigned char)page->text[stop]&0xc0)==0x80)stop--;}
        char run[256];memcpy(run,page->text+pos,stop-pos);run[stop-pos]=0;
        int flags=(style.flags&CSS_BOLD)?TEXT_BOLD:0;float width=text_width(run,style.scale,flags);
        int mark=selected>=0&&page->links[selected].offset>=pos&&page->links[selected].offset<stop;
        if(style.background)gfx_rect((int)x,(int)y,(int)(width+1),(int)line->height,style.background);
        text_draw(x,y,run,style.scale,mark?ACCENT:style.color,flags);
        if(style.flags&CSS_UNDERLINE)gfx_rect((int)x,(int)(y+text_line_height(style.scale)-1),(int)width,1,style.color);
        x+=width;pos=stop;
    }
}
static void draw_scene(void *ud)
{
    (void)ud;
    gfx_rect(0,0,SCREEN_W,SCREEN_H,BG);
    gfx_rect(0,0,480,44,RGB(23,40,57));
    text_draw(12,5,"ARK Browser",0.72f,FG,TEXT_BOLD);
    const char *security=history.count?(browser_url_secure(page->url)?"HTTPS":"HTTP - unencrypted"):"Start";
    text_draw(468,7,security,0.52f,browser_url_secure(page->url)&&history.count?ACCENT:ALERT,TEXT_RIGHT);
    text_draw_fit(12,27,455,history.count?page->url:"Triangle: enter a web address",0.5f,DIM,0);
    gfx_clip(8,47,464,177);
    if(link_list && page->count) {
        int first=selected>7?selected-7:0;
        for(int i=0;i<13 && first+i<page->count;i++) {
            int index=first+i; char label[128];
            snprintf(label,sizeof(label),"[%d] %s",index+1,page->links[index].label);
            if(index==selected) gfx_rect(8,48+i*13,464,13,RGB(24,65,85));
            text_draw_fit(12,49+i*13,450,label,0.6f,index==selected?ACCENT:FG,0);
        }
    } else {
        gfx_rect(8,47,464,177,page->paper);
        float y=48;
        for(int i=scroll;i<line_count&&y<222;i++) {draw_line(i,y);y+=page_lines[i].height;}
    }
    gfx_noclip();
    gfx_rect(0,226,480,46,RGB(18,32,47));
    if(selected>=0 && selected<page->count) {
        char target[BROWSER_URL_MAX+32]; snprintf(target,sizeof(target),"[%d/%d] %s",selected+1,page->count,page->links[selected].url);
        text_draw_fit(10,228,460,target,0.5f,ACCENT,0);
    } else text_draw_fit(10,228,460,page->title,0.55f,DIM,0);
    text_draw(10,246,"Triangle: URL   L/R: links   Confirm: open   Cancel: back",0.47f,FG,0);
    text_draw(10,259,"Select: link list   Square: download   Start: menu",0.43f,DIM,0);
    if(message[0]) {
        gfx_rect(10,163,460,58,RGB(40,46,53));
        const char *starts[5]; int lengths[5];
        int count=text_wrap(message,0.56f,438,0,starts,lengths,4);
        for(int i=0;i<count;i++) text_draw_n(20,170+i*12,starts[i],lengths[i],0.56f,FG,0);
    }
    if(menu) {
        gfx_rect(75,47,330,173,RGB(28,46,61));
        text_draw(90,53,"Browser menu",0.65f,FG,TEXT_BOLD);
        for(int i=0;i<8;i++) {
            if(menu_selection==i) gfx_rect(83,73+i*17,314,19,RGB(36,80,99));
            text_draw(90,75+i*17,menu_labels[i],0.55f,menu_selection==i?ACCENT:FG,0);
        }
    }
    if(browser_jobs_busy()) {
        gfx_rect(42,87,396,96,RGB(26,48,64));
        text_draw(56,100,browser_work.type==BROWSER_JOB_DOWNLOAD?"Downloading file":"Loading page",0.72f,FG,TEXT_BOLD);
        char progress[128],done[24],total[24];
        int64_t d=browser_work.done,t=browser_work.total;
        pm_format_size(d,done,sizeof(done));
        if(t>0) { pm_format_size(t,total,sizeof(total)); snprintf(progress,sizeof(progress),"%s / %s",done,total); }
        else snprintf(progress,sizeof(progress),"%s received",done);
        text_draw(56,130,progress,0.6f,ACCENT,0);
        text_draw(56,158,"Cancel: stop and keep the current page",0.5f,DIM,0);
    }
}
static void frame(void)
{
    gfx_begin(); draw_scene(NULL); gfx_end(); gfx_swap();
}
static int connect_wifi(void)
{
    if(net_is_connected()) return 1;
    if(!net_wlan_switch_on()) { notice("Wi-Fi is off. Turn on the WLAN switch; PSP Street has no Wi-Fi."); return 0; }
    int result=net_connect_dialog(draw_scene,NULL); input_flush();
    if(result<0) notice(net_last_error());
    return result==NET_CONNECTED;
}
static int address_input(char *out,size_t size)
{
    static unsigned short description[]={ 'W','e','b',' ','a','d','d','r','e','s','s',0 };
    static unsigned short initial[256],output[256];
    const char *url=history.count?page->url:"https://";
    size_t i;
    for(i=0;i<255 && url[i];i++) initial[i]=(unsigned char)url[i];
    initial[i]=0;
    memset(output,0,sizeof(output));
    SceUtilityOskData data={0}; data.language=PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines=1; data.unk_24=1; data.inputtype=PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc=description; data.intext=initial; data.outtext=output;
    data.outtextlength=255; data.outtextlimit=254;
    SceUtilityOskParams params={0}; params.base.size=sizeof(params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,&params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN,&params.base.buttonSwap);
    params.base.graphicsThread=17; params.base.accessThread=19; params.base.fontThread=18; params.base.soundThread=16;
    params.datacount=1; params.data=&data;
    if(sceUtilityOskInitStart(&params)<0) { notice("Could not open the keyboard."); return 0; }
    int complete=0,stopping=0;
    while(!complete) {
        gfx_begin(); draw_scene(NULL); gfx_end();
        int state=sceUtilityOskGetStatus();
        if(state==PSP_UTILITY_DIALOG_VISIBLE) {
            if(exit_requested && !stopping) { sceUtilityOskShutdownStart(); stopping=1; }
            else sceUtilityOskUpdate(1);
        } else if(state==PSP_UTILITY_DIALOG_QUIT && !stopping) { sceUtilityOskShutdownStart(); stopping=1; }
        else if(state==PSP_UTILITY_DIALOG_NONE) complete=1;
        gfx_swap();
    }
    input_flush();
    if(exit_requested || data.result!=PSP_UTILITY_OSK_RESULT_CHANGED) return 0;
    char entered[256];
    for(i=0;i<255 && output[i];i++) {
        if(output[i]>=128) { notice("Use ASCII or percent-encoded URLs; international host names need punycode."); return 0; }
        entered[i]=(char)output[i];
    }
    entered[i]=0;
    if(browser_url_enter(entered,out,size)<0) { notice("Enter an HTTP or HTTPS address without spaces or embedded credentials."); return 0; }
    return 1;
}
static void navigate(const char *url,int mode)
{
    char target[BROWSER_URL_MAX];
    if(browser_url_resolve(NULL,url,target,sizeof(target))<0) { notice("Unsupported or invalid link."); return; }
    const char *fragment=strchr(target,'#');
    if(page&&fragment){char current[BROWSER_URL_MAX],next[BROWSER_URL_MAX];strcpy(current,page->url);strcpy(next,target);current[strcspn(current,"#")]=0;next[strcspn(next,"#")]=0;
        if(!strcmp(current,next)){size_t offset=0;for(int i=0;i<page->anchor_count;i++)if(!strcmp(page->anchors[i].id,fragment+1))offset=page->anchors[i].offset;
            for(int i=0;i<line_count;i++)if(page_lines[i].start+page_lines[i].length>=offset){scroll=i;break;}clamp_scroll();return;}}
    if(!connect_wifi() || exit_requested) return;
    message[0]=0; failed_url[0]=0;
    if(history.count) history.visits[history.current].scroll=scroll;
    if(browser_jobs_submit(BROWSER_JOB_PAGE,mode,target,NULL,javascript)<0) notice("Could not start loading the page.");
}
static void open_address(void)
{
    char url[BROWSER_URL_MAX]; if(address_input(url,sizeof(url))) navigate(url,NAV_NEW);
}
static void download_file(void)
{
    const char *url=failed_url[0]?failed_url:selected>=0?page->links[selected].url:history.count?page->url:NULL;
    if(!url) { notice("Choose a link or open a page first."); return; }
    if(!connect_wifi() || exit_requested) return;
    char filename[104],destination[256]; browser_filename(url,filename,sizeof(filename));
    if(fs_mkdirs(downloads,NULL,NULL)<0) { notice("Could not create the downloads folder."); return; }
    int index;
    for(index=0;index<100;index++) {
        int n=index?snprintf(destination,sizeof(destination),"%s%s.%d",downloads,filename,index):
            snprintf(destination,sizeof(destination),"%s%s",downloads,filename);
        if(n<0 || n>=(int)sizeof(destination)) { notice("Download path is too long."); return; }
        if(!fs_exists(destination)) break;
    }
    if(index==100) { notice("Too many files with that name. Rename or remove an old download."); return; }
    message[0]=0;
    if(browser_jobs_submit(BROWSER_JOB_DOWNLOAD,NAV_NEW,url,destination,0)<0) notice("Could not start the download.");
}
static void home(void)
{
    const char *html="<title>ARK Browser 0.2</title><h1>ARK Browser</h1><p>Direct HTTPS with TLS 1.2 and certificate checks.</p>"
        "<p>Press Triangle to enter a web address. Up/Down scroll; L/R select a numbered link; Confirm opens it.</p>"
        "<p>Try <a href='https://example.org/'>Example.org</a> or <a href='http://info.cern.ch/'>the first website (HTTP)</a>.</p>"
        "<p>Select opens the link list. Square saves a selected link. Start opens the menu.</p>"
        "<p>Modern JavaScript runs on your PSP. Basic CSS styles text. Images, video and interactive forms are unavailable. Start toggles JavaScript.</p>";
    browser_document *next=calloc(1,sizeof(*next)); char err[256];
    if(!next) { notice("Not enough memory for the start page."); return; }
    if(browser_document_parse(next,html,strlen(html),"https://example.org/","text/html",err,sizeof(err))<0) { free(next); notice(err); return; }
    if(page) { browser_document_free(page); free(page); }
    page=next; memset(&history,0,sizeof(history)); scroll=0; selected=-1; link_list=0;
    message[0]=failed_url[0]=0; layout();
}
static void collect(void)
{
    if(!browser_work.finished) return;
    browser_document *loaded=browser_work.page;
    if(browser_work.result==0 && browser_work.type==BROWSER_JOB_PAGE && loaded) {
        if(browser_history_commit(&history,browser_work.navigation,loaded->url)==0) {
            browser_document_free(page); free(page); page=loaded; browser_work.page=NULL;
            scroll=history.visits[history.current].scroll; selected=-1; link_list=0; failed_url[0]=0; layout();
            if(page->shortened) notice("Page shortened to fit PSP memory.");
            else if(page->scripts_failed) notice("Some scripts failed or exceeded PSP limits. Showing available text.");
            else if(page->assets_omitted||page->css_omitted) notice("Some page assets or CSS exceeded PSP limits.");
            else if(page->links_omitted) notice("Only the first 256 links are available.");
        } else notice("Could not update browsing history.");
    } else if(browser_work.result==0) {
        char saved[256]; snprintf(saved,sizeof(saved),"Saved: %s",browser_work.destination); notice(saved);
        failed_url[0]=0;
    } else if(browser_work.cancel) notice("Cancelled. Your current page is unchanged.");
    else {
        notice(browser_work.error[0]?browser_work.error:"The request failed.");
        pm_strlcpy(failed_url,browser_work.url,sizeof(failed_url));
    }
    if(browser_work.page) { browser_document_free(browser_work.page); free(browser_work.page); browser_work.page=NULL; }
    browser_work.finished=0;
}
static void select_link(int direction)
{
    if(!page->count) { notice("This page has no supported links."); return; }
    if(selected<0) selected=direction>0?0:page->count-1;
    else { selected+=direction; if(selected<0) selected=page->count-1; if(selected>=page->count) selected=0; }
    failed_url[0]=0;
    if(!link_list) {
        for(int i=0;i<line_count;i++) {
            size_t end=page_lines[i].start+page_lines[i].length;
            if(page->links[selected].offset<=end) { scroll=i; clamp_scroll(); break; }
        }
    }
}
static void back(void)
{
    const char *url=browser_history_back(&history);
    if(url) navigate(url,NAV_BACK); else notice("No previous page. Start opens the browser menu.");
}
static void menu_action(int choice)
{
    menu=0;
    if(choice==0) open_address();
    else if(choice==1) back();
    else if(choice==2) { if(history.count) navigate(page->url,NAV_RELOAD); else notice("Open a web address first."); }
    else if(choice==3) download_file();
    else if(choice==4) { net_disconnect(); connect_wifi(); }
    else if(choice==5) home();
    else if(choice==6) { javascript=!javascript; notice(javascript?"JavaScript enabled. Reload the page to apply.":"JavaScript disabled. Reload the page to apply."); }
    else if(choice==7) exit_requested=1;
}
int main(int argc,char **argv)
{
    int callback_thread=sceKernelCreateThread("arkb_callbacks",callbacks,0x11,0x1000,PSP_THREAD_ATTR_USER,NULL);
    if(callback_thread>=0) sceKernelStartThread(callback_thread,0,NULL);
    scePowerSetClockFrequency(333,333,166);
    entropy_init(); gfx_init();
    if(text_init()<0) { gfx_term(); sceKernelExitGame(); return 1; }
    input_init();
    if(argc && argv[0]) pm_dirname(argv[0],app_dir,sizeof(app_dir));
    if(!app_dir[0]) strcpy(app_dir,"ms0:/PSP/GAME/ARKBrowser/");
    snprintf(downloads,sizeof(downloads),"%sdownloads/",app_dir);
    char ca[256]; snprintf(ca,sizeof(ca),"%scacert.pem",app_dir);
    net_set_tls(ca,1); net_set_client("ARKBrowser/" APP_VERSION " (PSP; text browser)",1);
    char cache_dir[256],cache[256];snprintf(cache_dir,sizeof(cache_dir),"%s.cache/",app_dir);
    int cache_ok=fs_mkdirs(cache_dir,NULL,NULL);snprintf(cache,sizeof(cache),"%s.cache/page.tmp",app_dir);
    home();
    if(!page || cache_ok<0 || browser_jobs_start(cache)<0) {
        if(page) { notice("Could not start the browser worker. Press Cancel to exit."); input_state in; do { input_update(&in); frame(); } while(!exit_requested && !(in.pressed&BTN_CANCEL)); }
    } else {
        input_state in;
        while(!exit_requested) {
            collect(); input_update(&in);
            if(browser_jobs_busy()) { if(in.pressed&BTN_CANCEL) browser_work.cancel=1; }
            else if(menu) {
                if(in.repeat&PSP_CTRL_DOWN) menu_selection=(menu_selection+1)%8;
                if(in.repeat&PSP_CTRL_UP) menu_selection=(menu_selection+7)%8;
                if(in.pressed&BTN_CANCEL) menu=0;
                if(in.pressed&BTN_CONFIRM) menu_action(menu_selection);
            } else {
                if(in.pressed) message[0]=0;
                if(in.pressed&PSP_CTRL_START) { menu=1; menu_selection=0; }
                else if(in.pressed&PSP_CTRL_TRIANGLE) open_address();
                else if(in.pressed&PSP_CTRL_SQUARE) download_file();
                else if(in.pressed&PSP_CTRL_SELECT) { link_list=!link_list; if(link_list && selected<0 && page->count) { selected=0; failed_url[0]=0; } }
                else if(in.pressed&BTN_CANCEL) back();
                else if(in.pressed&BTN_CONFIRM) { if(selected>=0) navigate(page->links[selected].url,NAV_NEW); else notice("Select a link with L/R or enter an address with Triangle."); }
                else if(in.repeat&PSP_CTRL_RTRIGGER) select_link(1);
                else if(in.repeat&PSP_CTRL_LTRIGGER) select_link(-1);
                else if(in.repeat&PSP_CTRL_DOWN) { if(link_list) select_link(1); else { scroll+=2; clamp_scroll(); } }
                else if(in.repeat&PSP_CTRL_UP) { if(link_list) select_link(-1); else { scroll-=2; clamp_scroll(); } }
                else if(in.repeat&PSP_CTRL_RIGHT) { scroll+=PAGE_LINES; clamp_scroll(); }
                else if(in.repeat&PSP_CTRL_LEFT) { scroll-=PAGE_LINES; clamp_scroll(); }
            }
            frame();
        }
    }
    browser_work.cancel=1; browser_jobs_stop(); net_term();
    if(page) { browser_document_free(page); free(page); }
    text_term(); gfx_term(); sceKernelExitGame(); return 0;
}

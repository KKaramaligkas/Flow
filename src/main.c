/* ARK Browser: direct HTTP(S), a laid-out page with a cursor, a reader view
   and on-device JavaScript. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psputility.h>
#include "document.h"
#include "layout.h"
#include "view.h"
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

PSP_MODULE_INFO("ARKBrowser",PSP_MODULE_USER,0,3);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU);
PSP_HEAP_THRESHOLD_SIZE_KB(4*1024);
#define PAGE_LINES 16
#define MAX_LINES BROWSER_LINES_MAX
#define BG RGB(12,20,31)
#define FG RGB(224,232,242)
#define DIM RGB(135,158,180)
#define ACCENT RGB(82,188,230)
#define ALERT RGB(255,184,95)
/* The page view: a bar above, the page, a status bar below. */
#define TOP 18
#define BOTTOM 256
#define PAGE_H (BOTTOM-TOP)
#define SEARCH "https://lite.duckduckgo.com/lite/?q="

static volatile int exit_requested;
static browser_document *page;
static browser_history history;
static int javascript=1, reader;
/* page view */
static float scroll_y, scroll_target, cursor_x=150, cursor_y=TOP+60;
static int hover_item=-1, hover_link=-1, hover_control=-1, picker=-1, picker_selection, picker_first;
/* reader view */
static browser_line page_lines[MAX_LINES];
static int line_count, scroll, selected=-1, link_list;
static int menu, menu_selection;
static char app_dir[192], downloads[224], message[256], failed_url[BROWSER_URL_MAX];
enum { M_ADDRESS, M_BACK, M_FORWARD, M_RELOAD, M_LINKS, M_READER, M_DOWNLOAD, M_HOME, M_WIFI, M_JAVASCRIPT, M_COOKIES, M_EXIT, M_COUNT };

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
static browser_view *view(void) { return page&&!reader?page->view:NULL; }

/* ---- reader view ---- */

static void clamp_scroll(void)
{
    int last=line_count;float height=0;
    while(last>0&&height+page_lines[last-1].height<=PAGE_H-12){last--;height+=page_lines[last].height;}
    if(last==line_count&&last>0)last--;
    if(scroll<0) scroll=0;
    if(scroll>last) scroll=last;
}
static float measure(void *ud,const char *text,size_t length,browser_style style)
{
    (void)ud; return text_measure(text,(int)length,style.scale,(style.flags&CSS_BOLD)?TEXT_BOLD:0);
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
        int flags=(style.flags&CSS_BOLD)?TEXT_BOLD:0;float width=text_measure(run,(int)(stop-pos),style.scale,flags);
        int mark=selected>=0&&page->links[selected].offset>=pos&&page->links[selected].offset<stop;
        if(style.background)gfx_rect((int)x,(int)y,(int)(width+1),(int)line->height,style.background);
        text_print(x,y,run,style.scale,mark?ACCENT:style.color,flags);
        if(style.flags&CSS_UNDERLINE)gfx_rect((int)x,(int)(y+text_line_height(style.scale)-1),(int)width,1,style.color);
        x+=width;pos=stop;
    }
}
static void draw_reader(void)
{
    gfx_rect(0,TOP,480,PAGE_H,page->paper);
    gfx_clip(0,TOP,480,PAGE_H);
    float y=TOP+4;
    for(int i=scroll;i<line_count&&y<BOTTOM;i++) {draw_line(i,y);y+=page_lines[i].height;}
    gfx_noclip();
}
static void draw_link_list(void)
{
    gfx_rect(0,TOP,480,PAGE_H,RGB(16,28,40));
    int first=selected>7?selected-7:0;
    for(int i=0;i<18 && first+i<page->count;i++) {
        int index=first+i; char label[128];
        snprintf(label,sizeof(label),"[%d] %s",index+1,page->links[index].label);
        if(index==selected) gfx_rect(0,TOP+2+i*13,480,13,RGB(24,65,85));
        text_draw_fit(8,TOP+3+i*13,464,label,0.6f,index==selected?ACCENT:FG,0);
    }
    if(!page->count) text_draw(240,TOP+100,"This page has no links.",0.6f,DIM,TEXT_CENTER);
}

/* ---- page view ---- */

static void clip_page(int x,int y,int w,int h)
{
    int x2=x+w,y2=y+h;
    if(x<0)x=0;
    if(y<TOP)y=TOP;
    if(x2>VIEW_WIDTH)x2=VIEW_WIDTH;
    if(y2>BOTTOM)y2=BOTTOM;
    gfx_clip(x,y,x2>x?x2-x:1,y2>y?y2-y:1);
}
static void frame_rect(int x,int y,int w,int h,int t,u32 c)
{
    gfx_rect(x,y,w,t,c); gfx_rect(x,y+h-t,w,t,c); gfx_rect(x,y,t,h,c); gfx_rect(x+w-t,y,t,h,c);
}
static int text_field(int kind) { return kind==CONTROL_TEXT||kind==CONTROL_PASSWORD||kind==CONTROL_TEXTAREA; }

static void draw_control(const browser_view *v,const view_item *it,int x,int y)
{
    const view_control *c=&v->controls[it->control];
    int w=it->w,h=it->h,hot=it->control==hover_control;
    u32 border=hot?RGB(40,110,220):RGB(140,140,140),ink=c->disabled?RGB(150,150,150):RGB(20,20,20);
    switch(c->kind) {
    case CONTROL_CHECKBOX:
        gfx_rect(x,y,w,h,RGB(255,255,255)); frame_rect(x,y,w,h,1,border);
        if(c->checked) { gfx_line(x+2,y+5,x+4,y+8,ink); gfx_line(x+4,y+8,x+9,y+2,ink); gfx_line(x+2,y+6,x+4,y+9,ink); gfx_line(x+4,y+9,x+9,y+3,ink); }
        return;
    case CONTROL_RADIO:
        gfx_circle(x+w/2.0f,y+h/2.0f,w/2.0f,RGB(255,255,255),1); gfx_circle(x+w/2.0f,y+h/2.0f,w/2.0f,border,0);
        if(c->checked) gfx_circle(x+w/2.0f,y+h/2.0f,w/2.0f-3,ink,1);
        return;
    case CONTROL_SUBMIT: case CONTROL_RESET: case CONTROL_BUTTON: case CONTROL_IMAGE: case CONTROL_FILE:
        if(it->flags&CONTROL_PLAIN) {
            /* styled as text by the page */
            if(hot) gfx_rect(x,y,w,h,RGBA(60,130,255,40));
            text_print(x+1,y,view_string(v,c->label),it->scale,c->disabled?RGB(150,150,150):it->color,(it->flags&2)?TEXT_BOLD:0);
            return;
        }
        gfx_gradient(x,y,w,h,hot?RGB(232,242,255):RGB(250,250,250),hot?RGB(200,220,250):RGB(220,220,220));
        frame_rect(x,y,w,h,1,border);
        clip_page(x+2,y,w-4,h);
        text_draw(x+w/2.0f,y+(h-10)/2.0f,view_string(v,c->label),it->scale,ink,TEXT_CENTER);
        clip_page(0,TOP,VIEW_WIDTH,PAGE_H);
        return;
    default: break;
    }
    gfx_rect(x,y,w,h,c->disabled||c->readonly?RGB(240,240,240):RGB(255,255,255));
    frame_rect(x,y,w,h,1,border);
    const char *value=c->value?c->value:"";
    char shown[VIEW_VALUE_MAX+1];
    u32 color=ink;
    if(c->kind==CONTROL_SELECT) {
        value=c->option_count?view_string(v,v->options[c->option_first+c->selected].label):"";
        gfx_triangle(x+w-11,y+h/2-2,x+w-3,y+h/2-2,x+w-7,y+h/2+3,RGB(90,90,90));
    } else if(c->kind==CONTROL_PASSWORD) {
        size_t n=0;for(const char *p=value;*p&&n<VIEW_VALUE_MAX;p++)if(((unsigned char)*p&0xc0)!=0x80)shown[n++]='*';
        shown[n]=0;value=shown;
    }
    if(!*value){value=view_string(v,c->label);color=RGB(150,150,150);}
    clip_page(x+2,y+1,w-(c->kind==CONTROL_SELECT?14:4),h-2);
    if(c->kind==CONTROL_TEXTAREA&&h>20) {
        const char *starts[12];int lengths[12];
        int lines=text_wrap(value,it->scale,w-6,0,starts,lengths,(h-4)/11<12?(h-4)/11:12);
        for(int i=0;i<lines;i++)text_draw_n(x+3,y+2+i*11,starts[i],lengths[i],it->scale,color,0);
    } else text_print(x+3,y+(h-10)/2.0f,value,it->scale,color,0);
    clip_page(0,TOP,VIEW_WIDTH,PAGE_H);
}

static void draw_item(const browser_view *v,const view_item *it,int top)
{
    int x=it->x,y=it->y-top+TOP;
    switch(it->kind) {
    case ITEM_RECT: gfx_rect(x,y,it->w,it->h,it->color); break;
    case ITEM_FRAME: frame_rect(x,y,it->w,it->h,it->flags>0?it->flags:1,it->color); break;
    case ITEM_BULLET: gfx_rect(x,y,it->w,it->h,it->color); break;
    case ITEM_CONTROL: if(it->control>=0) draw_control(v,it,x,y); break;
    case ITEM_IMAGE: {
        gfx_rect(x,y,it->w,it->h,RGB(232,234,237)); frame_rect(x,y,it->w,it->h,1,RGB(196,199,204));
        if(it->length>0&&it->h>=10) {
            char alt[256];int n=it->length<255?it->length:255;memcpy(alt,v->text+it->text,(size_t)n);alt[n]=0;
            clip_page(x+2,y+1,it->w-4,it->h-2);
            text_print(x+3,y+(it->h-9)/2.0f,alt,0.5f,RGB(100,104,110),0);
            clip_page(0,TOP,VIEW_WIDTH,PAGE_H);
        }
        break;
    }
    case ITEM_TEXT: {
        char run[512];int n=it->length<511?it->length:511;
        memcpy(run,v->text+it->text,(size_t)n);run[n]=0;
        int flags=(it->flags&CSS_BOLD)?TEXT_BOLD:0;
        text_print(x,y,run,it->scale,it->color,flags);
        if(it->flags&CSS_UNDERLINE) gfx_rect(x,y+(int)(13*it->scale)+1,it->w,1,it->color);
        if(it->flags&CSS_STRIKE) gfx_rect(x,y+(int)(9*it->scale),it->w,1,it->color);
        break;
    }
    }
}

static void draw_page(void)
{
    const browser_view *v=page->view;
    int top=(int)(scroll_y+0.5f);
    gfx_rect(0,TOP,VIEW_WIDTH,PAGE_H,v->paper);
    clip_page(0,TOP,VIEW_WIDTH,PAGE_H);
    for(int i=browser_view_first(v,top);browser_view_more(v,i,top+PAGE_H);i++) {
        const view_item *it=&v->items[i];
        if(it->y+it->h>top&&it->y<top+PAGE_H) draw_item(v,it,top);
    }
    if(hover_link>=0) {
        for(int i=browser_view_first(v,top);browser_view_more(v,i,top+PAGE_H);i++) {
            const view_item *it=&v->items[i];
            if(it->link==hover_link) gfx_rect(it->x-1,it->y-top+TOP-1,it->w+2,it->h+2,RGBA(60,130,255,56));
        }
    }
    gfx_noclip();
    /* scroll bar */
    gfx_rect(VIEW_WIDTH,TOP,480-VIEW_WIDTH,PAGE_H,RGB(226,229,233));
    if(v->height>PAGE_H) {
        int h=PAGE_H*PAGE_H/v->height;if(h<12)h=12;
        int y=(int)(TOP+(PAGE_H-h)*scroll_y/(v->height-PAGE_H));
        gfx_round_rect(VIEW_WIDTH+1,y,480-VIEW_WIDTH-2,h,2,RGB(150,156,164));
    }
}

static void draw_cursor(void)
{
    float x=(int)cursor_x,y=(int)cursor_y;
    const browser_view *v=view();
    if(v&&hover_control>=0&&text_field(v->controls[hover_control].kind)) {
        gfx_rect(x-2,y-8,5,17,RGB(255,255,255)); gfx_rect(x,y-7,1,15,RGB(0,0,0));
        gfx_rect(x-2,y-7,5,1,RGB(0,0,0)); gfx_rect(x-2,y+7,5,1,RGB(0,0,0));
        return;
    }
    u32 fill=hover_link>=0||hover_control>=0?RGB(120,190,255):RGB(255,255,255);
    gfx_triangle(x-1,y-2,x-1,y+16,x+12,y+11,RGB(0,0,0));
    gfx_triangle(x,y,x,y+13,x+9,y+9,fill);
}

static void draw_picker(void)
{
    const browser_view *v=page->view;
    const view_control *c=&v->controls[picker];
    int rows=c->option_count<12?c->option_count:12,h=rows*15+10,y=TOP+(PAGE_H-h)/2;
    gfx_rect(0,TOP,480,PAGE_H,RGBA(0,0,0,110));
    gfx_round_rect(60,y,360,h,6,RGB(250,250,250)); gfx_round_frame(60,y,360,h,6,1,RGB(120,120,120));
    for(int i=0;i<rows;i++) {
        int index=picker_first+i;if(index>=c->option_count)break;
        if(index==picker_selection) gfx_rect(64,y+5+i*15,352,15,RGB(60,130,230));
        text_draw_fit(70,y+7+i*15,340,view_string(v,v->options[c->option_first+index].label),0.6f,index==picker_selection?RGB(255,255,255):RGB(20,20,20),0);
    }
}

/* ---- the screen ---- */

static void status_line(char *out,size_t size)
{
    const browser_view *v=view();
    if(v&&hover_link>=0) { snprintf(out,size,"%s",view_string(v,v->links[hover_link].url)); return; }
    if(v&&hover_control>=0) {
        static const char *hints[]={"Confirm: type text","Confirm: type a password","Confirm: type text","Confirm: check",
            "Confirm: choose","Confirm: choose an option","Confirm: send the form","Confirm: clear the form",
            "This button needs JavaScript","Confirm: send the form","","File uploads aren't supported"};
        const view_control *c=&v->controls[hover_control];
        snprintf(out,size,"%s",c->disabled?"This field is disabled":hints[c->kind]);
        return;
    }
    if(!reader&&link_list) { snprintf(out,size,"%d links. Confirm: open   Cancel: close",page->count); return; }
    snprintf(out,size,"%s",page->title);
}

static void draw_scene(void *ud)
{
    (void)ud;
    gfx_rect(0,0,SCREEN_W,SCREEN_H,BG);
    if(link_list) draw_link_list();
    else if(view()) draw_page();
    else draw_reader();
    /* address bar */
    gfx_rect(0,0,480,TOP,RGB(23,40,57));
    int secure=history.count&&browser_url_secure(page->url);
    const char *badge=!history.count?"ARK":secure?"HTTPS":"HTTP";
    float bw=text_width(badge,0.48f,TEXT_BOLD)+8;
    gfx_round_rect(3,3,bw,12,3,!history.count?RGB(60,90,120):secure?RGB(40,140,90):RGB(170,110,40));
    text_draw(7,3,badge,0.48f,RGB(255,255,255),TEXT_BOLD);
    text_draw_fit(bw+8,2,468-bw,history.count?page->url:"ARK Browser " APP_VERSION " - Triangle: address or search",0.52f,FG,0);
    /* status bar */
    gfx_rect(0,BOTTOM,480,SCREEN_H-BOTTOM,RGB(18,32,47));
    char status[BROWSER_URL_MAX+64];
    status_line(status,sizeof(status));
    text_draw_fit(6,BOTTOM+2,300,status,0.5f,hover_link>=0?ACCENT:DIM,0);
    text_draw(474,BOTTOM+2,reader?"Select: page view  Start: menu":"Triangle: address  Start: menu",0.45f,FG,TEXT_RIGHT);
    if(view()&&!link_list&&!menu&&picker<0&&!browser_jobs_busy()) draw_cursor();
    if(picker>=0&&view()) draw_picker();
    if(message[0]) {
        gfx_rect(10,BOTTOM-62,460,58,RGB(40,46,53));
        const char *starts[5]; int lengths[5];
        int count=text_wrap(message,0.56f,438,0,starts,lengths,4);
        for(int i=0;i<count;i++) text_draw_n(20,BOTTOM-55+i*12,starts[i],lengths[i],0.56f,FG,0);
    }
    if(menu) {
        static const char *labels[M_COUNT]={"Address or search","Back","Forward","Reload","Links on this page",NULL,
            "Download link or page","Start page","Connect Wi-Fi",NULL,"Clear cookies","Exit"};
        gfx_round_rect(110,TOP+4,260,M_COUNT*16+24,6,RGB(28,46,61));
        text_draw(124,TOP+8,"Browser menu",0.62f,FG,TEXT_BOLD);
        for(int i=0;i<M_COUNT;i++) {
            const char *label=labels[i];
            if(i==M_READER) label=reader?"Page view":"Reader view";
            if(i==M_JAVASCRIPT) label=javascript?"JavaScript: on":"JavaScript: off";
            if(menu_selection==i) gfx_rect(116,TOP+24+i*16,248,16,RGB(36,80,99));
            text_draw(124,TOP+25+i*16,label,0.55f,menu_selection==i?ACCENT:FG,0);
        }
    }
    if(browser_jobs_busy()) {
        gfx_round_rect(42,77,396,96,8,RGB(26,48,64));
        text_draw(56,90,browser_work.type==BROWSER_JOB_DOWNLOAD?"Downloading file":browser_work.post?"Sending form":"Loading page",0.72f,FG,TEXT_BOLD);
        char progress[128],done[24],total[24];
        int64_t d=browser_work.done,t=browser_work.total;
        pm_format_size(d,done,sizeof(done));
        if(t>0) { pm_format_size(t,total,sizeof(total)); snprintf(progress,sizeof(progress),"%s / %s",done,total); }
        else snprintf(progress,sizeof(progress),"%s received",done);
        text_draw(56,120,progress,0.6f,ACCENT,0);
        text_draw(56,148,"Cancel: stop and keep the current page",0.5f,DIM,0);
    }
}
static void frame(void)
{
    gfx_begin(); draw_scene(NULL); gfx_end(); gfx_swap();
}

/* ---- keyboard ---- */

static int utf8_to_utf16(const char *s,unsigned short *out,int size)
{
    int n=0;
    while(*s&&n<size-1) {
        unsigned char c=(unsigned char)*s;unsigned cp;int k;
        if(c<0x80){cp=c;k=1;}else if((c&0xE0)==0xC0&&s[1]){cp=((c&31)<<6)|(s[1]&63);k=2;}
        else if((c&0xF0)==0xE0&&s[1]&&s[2]){cp=((c&15)<<12)|((s[1]&63)<<6)|(s[2]&63);k=3;}
        else{cp='?';k=1;while(s[k]&&((unsigned char)s[k]&0xC0)==0x80)k++;}
        out[n++]=(unsigned short)cp;s+=k;
    }
    out[n]=0;return n;
}
static void utf16_to_utf8(const unsigned short *s,char *out,size_t size)
{
    size_t n=0;
    for(;*s&&n+4<size;s++) {
        unsigned cp=*s;
        if(cp<0x80)out[n++]=(char)cp;
        else if(cp<0x800){out[n++]=(char)(0xC0|(cp>>6));out[n++]=(char)(0x80|(cp&63));}
        else{out[n++]=(char)(0xE0|(cp>>12));out[n++]=(char)(0x80|((cp>>6)&63));out[n++]=(char)(0x80|(cp&63));}
    }
    out[n]=0;
}
/* The system keyboard. Returns 1 when the text was changed. */
static int keyboard(const char *title,const char *initial,char *out,size_t size,int lines)
{
#ifdef PM_AUTOTEST
    /* Test builds type the lines of ms0:/arkb_osk.txt, one per keyboard. */
    static char *answers,*answer;
    if(!answers) answer=answers=fs_read_all("ms0:/arkb_osk.txt",NULL,16*1024);
    if(!answer||!*answer) return 0;
    size_t length=strcspn(answer,"\r\n");
    if(length>=size) length=size-1;
    memcpy(out,answer,length); out[length]=0;
    answer+=length; while(*answer=='\r'||*answer=='\n') answer++;
    (void)title; (void)initial; (void)lines;
    return 1;
#endif
    static unsigned short description[64],intext[512],outtext[512];
    utf8_to_utf16(*title?title:"Text",description,64);
    utf8_to_utf16(initial,intext,512);
    memset(outtext,0,sizeof(outtext));
    int limit=(int)size/3;if(limit>500)limit=500;if(limit<1)limit=1;
    SceUtilityOskData data={0}; data.language=PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines=lines; data.unk_24=1; data.inputtype=PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc=description; data.intext=intext; data.outtext=outtext;
    data.outtextlength=511; data.outtextlimit=limit;
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
    utf16_to_utf8(outtext,out,size);
    return 1;
}

/* ---- navigation ---- */

static int connect_wifi(void)
{
    if(net_is_connected()) return 1;
    if(!net_wlan_switch_on()) { notice("Wi-Fi is off. Turn on the WLAN switch; PSP Street has no Wi-Fi."); return 0; }
    int result=net_connect_dialog(draw_scene,NULL); input_flush();
    if(result<0) notice(net_last_error());
    return result==NET_CONNECTED;
}
static void scroll_to(float y)
{
    const browser_view *v=page?page->view:NULL;
    float last=v&&v->height>PAGE_H?(float)(v->height-PAGE_H):0;
    if(y>last)y=last;
    if(y<0)y=0;
    scroll_target=y;
}
static void navigate(const char *url,int mode,const char *post)
{
    char target[BROWSER_URL_MAX];
    if(browser_url_resolve(NULL,url,target,sizeof(target))<0) { notice("Unsupported or invalid link."); return; }
    const char *fragment=strchr(target,'#');
    if(page&&fragment&&!post){char current[BROWSER_URL_MAX],next[BROWSER_URL_MAX];strcpy(current,page->url);strcpy(next,target);current[strcspn(current,"#")]=0;next[strcspn(next,"#")]=0;
        if(!strcmp(current,next)){
            if(page->view&&!reader){int y=browser_view_anchor(page->view,fragment+1);if(y>=0){scroll_to((float)y-4);scroll_y=scroll_target;}return;}
            size_t offset=0;for(int i=0;i<page->anchor_count;i++)if(!strcmp(page->anchors[i].id,fragment+1))offset=page->anchors[i].offset;
            for(int i=0;i<line_count;i++)if(page_lines[i].start+page_lines[i].length>=offset){scroll=i;break;}
            clamp_scroll();return;}}
    if(!connect_wifi() || exit_requested) return;
    message[0]=0; failed_url[0]=0;
    if(history.count) history.visits[history.current].scroll=(int)scroll_y;
    if(browser_jobs_submit(BROWSER_JOB_PAGE,mode,target,NULL,javascript,post)<0) notice("Could not start loading the page.");
}
/* Words become a search; anything else is an address. */
static void open_address(void)
{
    char entered[512],url[BROWSER_URL_MAX];
    if(!keyboard("Address or search",history.count?page->url:"",entered,sizeof(entered),1)) return;
    char *text=entered;while(*text==' ')text++;
    size_t n=strlen(text);while(n&&text[n-1]==' ')text[--n]=0;
    if(!*text) return;
    int search=strchr(text,' ')||(!strchr(text,'.')&&!strstr(text,"://")&&strncmp(text,"localhost",9));
    for(const char *p=text;*p;p++)if((unsigned char)*p>=0x80&&!strstr(text,"://"))search=1;
    if(!search&&browser_url_enter(text,url,sizeof(url))==0) { navigate(url,NAV_NEW,NULL); return; }
    size_t used=strlen(SEARCH);strcpy(url,SEARCH);
    static const char hex[]="0123456789ABCDEF";
    for(const unsigned char *p=(const unsigned char *)text;*p&&used+4<sizeof(url);p++) {
        if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'||*p=='.'||*p=='_'||*p=='*') url[used++]=(char)*p;
        else if(*p==' ') url[used++]='+';
        else { url[used++]='%'; url[used++]=hex[*p>>4]; url[used++]=hex[*p&15]; }
    }
    url[used]=0;
    navigate(url,NAV_NEW,NULL);
}
static void download_file(void)
{
    const browser_view *v=view();
    const char *url=failed_url[0]?failed_url:v&&hover_link>=0?view_string(v,v->links[hover_link].url):
        reader&&selected>=0?page->links[selected].url:history.count?page->url:NULL;
    if(!url) { notice("Point at a link or open a page first."); return; }
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
    if(browser_jobs_submit(BROWSER_JOB_DOWNLOAD,NAV_NEW,url,destination,0,NULL)<0) notice("Could not start the download.");
}
static void home(void)
{
    const char *html="<title>ARK Browser " APP_VERSION "</title><body style='background:#f3f5f8;color:#202428'>"
        "<h1 style='color:#1d4f7c;margin-bottom:4px'>ARK Browser</h1>"
        "<form action='https://lite.duckduckgo.com/lite/'><input name=q size=44 placeholder='Search the web'> <input type=submit value=Search></form>"
        "<p>Move the cursor with the analog stick and press Confirm to open a link or fill in a field. "
        "Up/Down scroll, Left/Right turn pages, L/R jump between links and fields. "
        "Triangle: address or search. Cancel: back. Select: reader view. Start: menu.</p>"
        "<h3>Places to start</h3><ul>"
        "<li><a href='https://lite.duckduckgo.com/lite/'>DuckDuckGo Lite</a> - search that works without JavaScript</li>"
        "<li><a href='https://www.google.com/'>Google</a></li>"
        "<li><a href='https://en.m.wikipedia.org/'>Wikipedia</a></li>"
        "<li><a href='https://lite.cnn.com/'>CNN Lite</a> and <a href='https://text.npr.org/'>NPR Text</a></li>"
        "<li><a href='http://info.cern.ch/'>The first website</a> (HTTP)</li></ul>"
        "<p style='color:#606870'>Pages are laid out on your PSP with HTTPS, CSS and JavaScript. Images show as boxes with their description. "
        "Cookies are kept in cookies.txt next to the app.</p>";
    browser_document *next=calloc(1,sizeof(*next)); char err[256];
    if(!next) { notice("Not enough memory for the start page."); return; }
    if(browser_document_parse(next,html,strlen(html),"https://example.org/","text/html",err,sizeof(err))<0) { free(next); notice(err); return; }
    browser_build_view(next);
    if(page) { browser_document_free(page); free(page); }
    page=next; memset(&history,0,sizeof(history)); scroll=0; scroll_y=scroll_target=0; selected=-1; link_list=0;
    message[0]=failed_url[0]=0; layout();
}
static void collect(void)
{
    if(!browser_work.finished) return;
    browser_document *loaded=browser_work.page;
    if(browser_work.result==0 && browser_work.type==BROWSER_JOB_PAGE && loaded) {
        if(browser_history_commit(&history,browser_work.navigation,loaded->url)==0) {
            browser_document_free(page); free(page); page=loaded; browser_work.page=NULL;
            selected=-1; link_list=0; picker=-1; failed_url[0]=0; scroll=0; layout();
            scroll_y=scroll_target=0; scroll_to((float)history.visits[history.current].scroll); scroll_y=scroll_target;
            const char *fragment=strchr(page->url,'#');
            if(fragment&&page->view){int y=browser_view_anchor(page->view,fragment+1);if(y>=0){scroll_to((float)y-4);scroll_y=scroll_target;}}
            if(!page->view&&!reader) notice("This page is shown as text: there wasn't enough memory to lay it out.");
            else if(page->shortened) notice("Page shortened to fit PSP memory.");
            else if(page->scripts_failed) notice("Some scripts failed or exceeded PSP limits. Showing the page without them.");
            else if(page->view&&page->view->truncated) notice("Only the first part of this page fits in PSP memory.");
            else if(page->assets_omitted||page->css_omitted) notice("Some page assets or CSS exceeded PSP limits.");
        } else notice("Could not update browsing history.");
    } else if(browser_work.result==0) {
        char saved[256]; snprintf(saved,sizeof(saved),"Saved: %.248s",browser_work.destination); notice(saved);
        failed_url[0]=0;
    } else if(browser_work.cancel) notice("Cancelled. Your current page is unchanged.");
    else {
        notice(browser_work.error[0]?browser_work.error:"The request failed.");
        pm_strlcpy(failed_url,browser_work.url,sizeof(failed_url));
    }
    if(browser_work.page) { browser_document_free(browser_work.page); free(browser_work.page); browser_work.page=NULL; }
    browser_work.finished=0;
}
static void back(void)
{
    const char *url=browser_history_back(&history);
    if(url) navigate(url,NAV_BACK,NULL); else notice("No previous page. Start opens the browser menu.");
}
static void forward(void)
{
    if(history.current+1<history.count) {
        /* Forward is a new visit to the next address; history keeps the rest. */
        char url[BROWSER_URL_MAX];strcpy(url,history.visits[history.current+1].url);
        navigate(url,NAV_NEW,NULL);
    } else notice("No next page.");
}

/* ---- forms ---- */

static void submit(int form,int submitter)
{
    browser_view *v=page->view;
    if(form<0) { notice("This field isn't part of a form."); return; }
    if(submitter<0) for(int i=0;i<v->control_count;i++) if(v->controls[i].form==form&&(v->controls[i].kind==CONTROL_SUBMIT||v->controls[i].kind==CONTROL_IMAGE)&&!v->controls[i].disabled){submitter=i;break;}
    char url[BROWSER_URL_MAX],err[160],*body=NULL;
    if(browser_view_submit(v,form,submitter,url,sizeof(url),&body,err,sizeof(err))<0) { notice(err); return; }
    if(v->forms[form].multipart) notice("This form uploads files, which isn't supported. Sending its text fields.");
    navigate(url,NAV_NEW,body);
    free(body);
}
static void activate(int index)
{
    browser_view *v=page->view;
    view_control *c=&v->controls[index];
    if(c->disabled) { notice("This field is disabled."); return; }
    switch(c->kind) {
    case CONTROL_TEXT: case CONTROL_PASSWORD: case CONTROL_TEXTAREA: {
        if(c->readonly) { notice("This field can't be changed."); return; }
        char text[VIEW_VALUE_MAX+1];
        const char *title=*view_string(v,c->label)?view_string(v,c->label):*view_string(v,c->name)?view_string(v,c->name):"Text";
        int lines=c->kind==CONTROL_TEXTAREA&&c->item>=0&&v->items[c->item].h>20?4:1;
        if(!keyboard(title,c->value,text,sizeof(text),lines)) return;
        if(browser_view_set_value(v,index,text)<0) { notice("Not enough memory for that text."); return; }
        if(browser_view_submits_on_enter(v,index)&&*text) submit(c->form,-1);
        return;
    }
    case CONTROL_CHECKBOX: case CONTROL_RADIO: browser_view_toggle(v,index); return;
    case CONTROL_SELECT:
        if(!c->option_count) return;
        picker=index; picker_selection=c->selected; picker_first=picker_selection>5?picker_selection-5:0;
        return;
    case CONTROL_SUBMIT: case CONTROL_IMAGE: submit(c->form,index); return;
    case CONTROL_RESET: browser_view_reset(v,c->form); return;
    case CONTROL_FILE: notice("File uploads aren't supported."); return;
    default: notice("This button needs JavaScript, which runs only while the page loads."); return;
    }
}

/* ---- page view input ---- */

static void update_hover(void)
{
    hover_item=hover_link=hover_control=-1;
    const browser_view *v=view();
    if(!v||cursor_y<TOP||cursor_y>=BOTTOM) return;
    hover_item=browser_view_hit(v,(int)cursor_x,(int)(scroll_y+cursor_y-TOP),&hover_link,&hover_control);
}
static float stick(int value)
{
    int a=value<0?-value:value;
    if(a<26) return 0;
    float t=(a-26)/101.0f;if(t>1)t=1;
    float speed=0.7f+t*t*7.5f;
    return value<0?-speed:speed;
}
static void move_cursor(int lx,int ly)
{
    cursor_x+=stick(lx);
    float dy=stick(ly);
    cursor_y+=dy;
    if(cursor_x<0)cursor_x=0;
    if(cursor_x>VIEW_WIDTH-2)cursor_x=VIEW_WIDTH-2;
    /* Pushing past the top or bottom scrolls the page. */
    if(cursor_y<TOP+2){ if(dy<0){scroll_to(scroll_target+dy*1.6f);scroll_y=scroll_target;} cursor_y=TOP+2; }
    if(cursor_y>BOTTOM-3){ if(dy>0){scroll_to(scroll_target+dy*1.6f);scroll_y=scroll_target;} cursor_y=BOTTOM-3; }
}
static void step(int direction)
{
    const browser_view *v=page->view;
    int next=browser_view_step(v,hover_item,(int)cursor_x,(int)(scroll_y+cursor_y-TOP),direction);
    if(next<0) { notice(direction>0?"No more links or fields below.":"No more links or fields above."); return; }
    const view_item *it=&v->items[next];
    if(it->y<scroll_target+12||it->y+it->h>scroll_target+PAGE_H-12) { scroll_to((float)(it->y-PAGE_H/3)); scroll_y=scroll_target; }
    cursor_x=(float)(it->x+(it->w<24?it->w/2:12));
    cursor_y=it->y-scroll_y+TOP+it->h/2.0f;
    update_hover();
}
static void click(void)
{
    const browser_view *v=view();
    if(!v) return;
    if(hover_control>=0) activate(hover_control);
    else if(hover_link>=0) navigate(view_string(v,v->links[hover_link].url),NAV_NEW,NULL);
}
static void picker_input(const input_state *in)
{
    view_control *c=&page->view->controls[picker];
    if(in->repeat&PSP_CTRL_DOWN&&picker_selection+1<c->option_count) picker_selection++;
    if(in->repeat&PSP_CTRL_UP&&picker_selection>0) picker_selection--;
    if(picker_selection<picker_first) picker_first=picker_selection;
    if(picker_selection>=picker_first+12) picker_first=picker_selection-11;
    if(in->pressed&BTN_CANCEL) picker=-1;
    else if(in->pressed&BTN_CONFIRM) { c->selected=picker_selection; picker=-1; }
}
static void page_input(const input_state *in)
{
    move_cursor(in->lx,in->ly);
    if(in->repeat&PSP_CTRL_DOWN) scroll_to(scroll_target+40);
    else if(in->repeat&PSP_CTRL_UP) scroll_to(scroll_target-40);
    else if(in->repeat&PSP_CTRL_RIGHT) scroll_to(scroll_target+PAGE_H-30);
    else if(in->repeat&PSP_CTRL_LEFT) scroll_to(scroll_target-(PAGE_H-30));
    if(in->repeat&PSP_CTRL_RTRIGGER) step(1);
    else if(in->repeat&PSP_CTRL_LTRIGGER) step(-1);
    else if(in->pressed&BTN_CONFIRM) click();
}
static void reader_input(const input_state *in)
{
    if(in->pressed&BTN_CONFIRM) { if(selected>=0) navigate(page->links[selected].url,NAV_NEW,NULL); else notice("Select a link with L/R or enter an address with Triangle."); }
    else if(in->repeat&PSP_CTRL_RTRIGGER||(link_list&&in->repeat&PSP_CTRL_DOWN)) {
        if(!page->count) { notice("This page has no supported links."); return; }
        selected=selected<0?0:(selected+1)%page->count;
        if(!link_list) for(int i=0;i<line_count;i++) if(page->links[selected].offset<=page_lines[i].start+page_lines[i].length) { scroll=i; clamp_scroll(); break; }
    }
    else if(in->repeat&PSP_CTRL_LTRIGGER||(link_list&&in->repeat&PSP_CTRL_UP)) {
        if(!page->count) { notice("This page has no supported links."); return; }
        selected=selected<=0?page->count-1:selected-1;
        if(!link_list) for(int i=0;i<line_count;i++) if(page->links[selected].offset<=page_lines[i].start+page_lines[i].length) { scroll=i; clamp_scroll(); break; }
    }
    else if(link_list) return;
    else if(in->repeat&PSP_CTRL_DOWN) { scroll+=2; clamp_scroll(); }
    else if(in->repeat&PSP_CTRL_UP) { scroll-=2; clamp_scroll(); }
    else if(in->repeat&PSP_CTRL_RIGHT) { scroll+=PAGE_LINES; clamp_scroll(); }
    else if(in->repeat&PSP_CTRL_LEFT) { scroll-=PAGE_LINES; clamp_scroll(); }
}
static void menu_action(int choice)
{
    menu=0;
    switch(choice) {
    case M_ADDRESS: open_address(); break;
    case M_BACK: back(); break;
    case M_FORWARD: forward(); break;
    case M_RELOAD: if(history.count) navigate(page->url,NAV_RELOAD,NULL); else notice("Open a web address first."); break;
    case M_LINKS: link_list=1; if(selected<0&&page->count) selected=0; break;
    case M_READER:
        if(reader&&!page->view) notice("This page couldn't be laid out; only the reader view is available.");
        else reader=!reader;
        break;
    case M_DOWNLOAD: download_file(); break;
    case M_HOME: home(); break;
    case M_WIFI: net_disconnect(); connect_wifi(); break;
    case M_JAVASCRIPT: javascript=!javascript; notice(javascript?"JavaScript enabled. Reload the page to apply.":"JavaScript disabled. Reload the page to apply."); break;
    case M_COOKIES: net_clear_cookies(); notice("Cookies cleared. Sites you were signed in to will ask again."); break;
    case M_EXIT: exit_requested=1; break;
    }
}
int main(int argc,char **argv)
{
    int callback_thread=sceKernelCreateThread("arkb_callbacks",callbacks,0x11,0x1000,PSP_THREAD_ATTR_USER,NULL);
    if(callback_thread>=0) sceKernelStartThread(callback_thread,0,NULL);
    scePowerSetClockFrequency(333,333,166);
    entropy_init(); gfx_init();
    if(text_init()<0) { gfx_term(); sceKernelExitGame(); return 1; }
    input_init();
    input_set_analog_dpad(0);
    if(argc && argv[0]) pm_dirname(argv[0],app_dir,sizeof(app_dir));
    if(!app_dir[0]) strcpy(app_dir,"ms0:/PSP/GAME/ARKBrowser/");
    snprintf(downloads,sizeof(downloads),"%sdownloads/",app_dir);
    char ca[256]; snprintf(ca,sizeof(ca),"%scacert.pem",app_dir);
    net_set_tls(ca,1); net_set_client("Mozilla/5.0 (PlayStation Portable; Mobile) ARKBrowser/" APP_VERSION,1);
    char cookies[256]; snprintf(cookies,sizeof(cookies),"%scookies.txt",app_dir); net_set_cookies(cookies);
    char cache_dir[256],cache[256];snprintf(cache_dir,sizeof(cache_dir),"%s.cache/",app_dir);
    int cache_ok=fs_mkdirs(cache_dir,NULL,NULL);snprintf(cache,sizeof(cache),"%s.cache/page.tmp",app_dir);
    home();
    if(text_fallback()) notice("No PSP fonts found (PPSSPP without a firmware): using a basic font. In PPSSPP, install a PSP firmware for nicer text.");
    if(!page || cache_ok<0 || browser_jobs_start(cache)<0) {
        if(page) { notice("Could not start the browser worker. Press Cancel to exit."); input_state in; do { input_update(&in); frame(); } while(!exit_requested && !(in.pressed&BTN_CANCEL)); }
    } else {
        input_state in;
        while(!exit_requested) {
            collect();
#ifdef PM_AUTOTEST
            input_autotest_hold(browser_jobs_busy());
#endif
            input_update(&in);
            if(browser_jobs_busy()) { if(in.pressed&BTN_CANCEL) browser_work.cancel=1; }
            else if(menu) {
                if(in.repeat&PSP_CTRL_DOWN) menu_selection=(menu_selection+1)%M_COUNT;
                if(in.repeat&PSP_CTRL_UP) menu_selection=(menu_selection+M_COUNT-1)%M_COUNT;
                if(in.pressed&(BTN_CANCEL|PSP_CTRL_START)) menu=0;
                else if(in.pressed&BTN_CONFIRM) menu_action(menu_selection);
            } else if(picker>=0) picker_input(&in);
            else {
                if(in.pressed) message[0]=0;
                if(in.pressed&PSP_CTRL_START) { menu=1; menu_selection=0; }
                else if(in.pressed&PSP_CTRL_TRIANGLE) open_address();
                else if(in.pressed&PSP_CTRL_SQUARE) download_file();
                else if(in.pressed&PSP_CTRL_SELECT) {
                    if(link_list) link_list=0;
                    else if(!page->view) notice("This page couldn't be laid out; only the reader view is available.");
                    else reader=!reader;
                }
                else if(in.pressed&BTN_CANCEL) { if(link_list) link_list=0; else back(); }
                else if(link_list||!view()) reader_input(&in);
                else page_input(&in);
            }
            /* Smooth scrolling toward the target. */
            float d=scroll_target-scroll_y;
            scroll_y=fabsf(d)<0.5f?scroll_target:scroll_y+d*0.35f;
            update_hover();
            frame();
        }
    }
    browser_work.cancel=1; browser_jobs_stop(); net_save_cookies(); net_term();
    if(page) { browser_document_free(page); free(page); }
    text_term(); gfx_term(); sceKernelExitGame(); return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/adopt.h"
#include "fs.h"
static int checks,failures;
#define CHECK(test) do{checks++;if(!(test)){failures++;fprintf(stderr,"%d: %s\n",__LINE__,#test);}}while(0)
#define OLD "ms0:/PSP/GAME/ARKBrowser/"
#define APP "ms0:/PSP/GAME/Flow/"
static char root[64];
static void fresh(void)
{
    char cmd[160];snprintf(cmd,sizeof(cmd),"rm -rf '%s/ms0' && mkdir '%s/ms0'",root,root);
    CHECK(system(cmd)==0);
    CHECK(fs_mkdirs(APP,NULL,NULL)==0);
}
static void put(const char *path,const char *text)
{
    char dir[256];snprintf(dir,sizeof(dir),"%s",path);*strrchr(dir,'/')=0;
    CHECK(fs_mkdirs(dir,NULL,NULL)==0);CHECK(fs_write_all(path,text,strlen(text))==0);
}
static int holds(const char *path,const char *text)
{
    char *data=fs_read_all(path,NULL,0);
    int same=data&&!strcmp(data,text);free(data);return same;
}
static void progress(void *ud,int index,int total)
{
    int *calls=ud;CHECK(index==*calls);CHECK(total==2);(*calls)++;
}
int main(void)
{
    snprintf(root,sizeof(root),"/tmp/flow_adopt_XXXXXX");
    if(!mkdtemp(root)) return 1;
    setenv("PM_FS_ROOT",root,1);
    adopt_result r;

    fresh(); /* no ARK Browser folder */
    CHECK(adopt_old_folder(APP,"ARKBrowser",NULL,NULL,&r)<0);CHECK(!r.downloads&&!r.cookies&&!r.removed);

    fresh(); /* ARK Browser is still installed: leave all of it */
    put(OLD "EBOOT.PBP","pbp");put(OLD "downloads/a.zip","a");put(OLD "cookies.txt","old");
    CHECK(adopt_old_folder(APP,"ARKBrowser",NULL,NULL,&r)<0);
    CHECK(holds(OLD "downloads/a.zip","a"));CHECK(holds(OLD "cookies.txt","old"));CHECK(!fs_exists(APP "cookies.txt"));

    fresh(); /* replaced by the Plugin Manager: take everything over */
    put(OLD "downloads/a.zip","a");put(OLD "downloads/b.txt","old b");put(OLD "cookies.txt","jar");
    put(OLD ".cache/page.tmp","cache");put(OLD "cacert.pem","ca");put(OLD "README.md","readme");
    put(APP "downloads/b.txt","new b");
    CHECK(adopt_old_folder(APP,"ARKBrowser",NULL,NULL,&r)==0);
    CHECK(r.downloads==2);CHECK(!r.failed);CHECK(r.cookies==1);CHECK(r.removed==1);
    CHECK(holds(APP "downloads/a.zip","a"));CHECK(holds(APP "downloads/b.txt","new b"));
    CHECK(holds(APP "downloads/b.txt.1","old b"));CHECK(holds(APP "cookies.txt","jar"));
    CHECK(!fs_exists(OLD));

    fresh(); /* Flow has its own cookies: keep both, so the old folder stays */
    put(OLD "downloads/a.zip","a");put(OLD "cookies.txt","old");put(APP "cookies.txt","new");
    CHECK(adopt_old_folder(APP,"ARKBrowser",NULL,NULL,&r)==0);
    CHECK(r.downloads==1);CHECK(!r.cookies);CHECK(!r.removed);
    CHECK(holds(APP "cookies.txt","new"));CHECK(holds(OLD "cookies.txt","old"));
    CHECK(!fs_exists(OLD "downloads"));CHECK(holds(APP "downloads/a.zip","a"));

    fresh(); /* no room: the download stays, and so does the folder */
    put(OLD "downloads/a.zip","a");put(OLD "cookies.txt","jar");
    fs_test_free_bytes(1024*1024,-2);
    CHECK(adopt_old_folder(APP,"ARKBrowser",NULL,NULL,&r)==0);
    fs_test_free_bytes(-2,-2);
    CHECK(!r.downloads);CHECK(r.failed==1);CHECK(!r.cookies);CHECK(!r.removed);
    CHECK(holds(OLD "downloads/a.zip","a"));CHECK(!fs_exists(APP "downloads/a.zip"));CHECK(holds(OLD "cookies.txt","jar"));

    fresh(); /* progress is reported for each download */
    put(OLD "downloads/a.zip","a");put(OLD "downloads/b.zip","b");
    int calls=0;
    CHECK(adopt_old_folder(APP,"ARKBrowser",progress,&calls,&r)==0);CHECK(calls==2);CHECK(r.downloads==2&&!r.failed);

    fresh(); /* files the user added are kept */
    put(OLD "notes.txt","mine");
    CHECK(adopt_old_folder(APP,"ARKBrowser",NULL,NULL,&r)==0);CHECK(!r.removed);CHECK(holds(OLD "notes.txt","mine"));

    fresh(); /* Flow itself installed in the old folder */
    put(OLD "downloads/a.zip","a");
    CHECK(adopt_old_folder("ms0:/PSP/GAME/arkbrowser/","ARKBrowser",NULL,NULL,&r)<0);CHECK(holds(OLD "downloads/a.zip","a"));
    CHECK(adopt_old_folder("ms0:/PSP/GAME/Flow","ARKBrowser",NULL,NULL,&r)<0);
    CHECK(adopt_old_folder("Flow/","ARKBrowser",NULL,NULL,&r)<0);

    char cmd[128];snprintf(cmd,sizeof(cmd),"rm -rf '%s'",root);CHECK(system(cmd)==0);
    printf("adopt: %d checks, %d failures\n",checks,failures);return failures?1:0;
}

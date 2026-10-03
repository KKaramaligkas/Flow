/* adopt.h: takes over the files ARK Browser, Flow's former name, kept in its
   own folder. */
#ifndef FLOW_ADOPT_H
#define FLOW_ADOPT_H

typedef struct {
    int downloads;  /* files moved into Flow's downloads folder */
    int failed;     /* downloads left in the old folder (no room, or an error) */
    int cookies;    /* 1 when the cookie file was moved */
    int removed;    /* 1 when the old folder is gone */
} adopt_result;

/* Called before each download is copied, with its position from 0 and the
   number of downloads. */
typedef void (*adopt_progress)(void *ud, int index, int total);

/* app_dir is Flow's folder with its trailing '/' ("ms0:/PSP/GAME/Flow/") and
   old_name the old app's folder beside it ("ARKBrowser"). Returns 0 when that
   folder was taken over, -1 when there is none or the old app is still in it.
   progress may be NULL. */
int adopt_old_folder(const char *app_dir, const char *old_name, adopt_progress progress, void *ud, adopt_result *out);

#endif

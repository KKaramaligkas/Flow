#ifndef FLOW_JOBS_H
#define FLOW_JOBS_H
#include <stdint.h>
#include "document.h"
#define BROWSER_JOB_PAGE 1
#define BROWSER_JOB_DOWNLOAD 2

typedef struct {
    volatile int running, finished, cancel, result;
    int type, navigation, javascript;
    char url[BROWSER_URL_MAX], destination[256], error[256];
    char *post;                 /* form data sent by POST, NULL for GET */
    volatile int64_t done, total;
    browser_document *page;
} browser_job;
extern browser_job browser_work;
int browser_jobs_start(const char *cache);
void browser_jobs_stop(void);
int browser_jobs_busy(void);
/* Lays out doc->view; the worker does this for every page it loads. */
void browser_build_view(browser_document *doc);
/* `post`: form data (application/x-www-form-urlencoded) to send by POST, or NULL. */
int browser_jobs_submit(int type, int navigation, const char *url, const char *destination,int javascript,const char *post);

/* The pictures of the page shown load between jobs, nearest to the screen
   first, and the page is laid out again as their sizes arrive. These are
   for the main thread. */
/* Starts on `doc`'s pictures (NULL: none). The worker lets go of the page
   it had at its next step: free that page only after browser_pictures_stop()
   returned 0 or a job it ran since has finished. */
void browser_pictures(browser_document *doc);
/* Stops picture work and waits up to `ms` for the worker to let go of the
   page: 0 when it did. */
int browser_pictures_stop(int ms);
/* Frees the pictures shown on `doc` (they load again if it's shown again),
   after browser_pictures_stop(): memory for the next page. */
void browser_pictures_release(browser_document *doc);
/* The page rows on screen. */
void browser_pictures_viewport(int top, int bottom);
/* A view of `doc` laid out again with its pictures' sizes, to replace
   doc->view, or NULL. */
struct browser_view *browser_pictures_view(browser_document *doc);
/* Drops a view not taken yet: call before freeing the page it's for. */
void browser_pictures_forget(void);
#endif

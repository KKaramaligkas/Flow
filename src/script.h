#ifndef ARKB_SCRIPT_H
#define ARKB_SCRIPT_H
#include "document.h"
/* Fetch returns malloc-owned UTF-8 bytes. Redirects must stay same-origin. */
typedef char *(*browser_fetch_fn)(void *,const char *,int,int *,char *,size_t);
int browser_script_run(browser_document *,browser_fetch_fn,void *,dom_cancel_fn,void *,unsigned,char *,size_t);
void browser_script_free(browser_document *);
int browser_same_origin(const char *,const char *);
#endif

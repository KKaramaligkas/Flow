#ifndef FLOW_SCRIPT_H
#define FLOW_SCRIPT_H
#include "document.h"
/* Fetch returns malloc-owned UTF-8 bytes. Redirects must stay same-origin. */
typedef char *(*browser_fetch_fn)(void *,const char *,int,int *,char *,size_t);
/* document.cookie: the cookies a page at `url` sees (0, or -1 for none),
   and one it sets. */
typedef int (*browser_cookie_get_fn)(const char *url,char *out,int size);
typedef int (*browser_cookie_set_fn)(const char *url,const char *cookie);
/* What scripts see of the browser: its user agent and cookies (NULL: none),
   and the memory a page's scripts may keep to handle clicks after it has
   loaded (0: they never stay). Set before pages load. */
void browser_script_setup(const char *agent,browser_cookie_get_fn get,browser_cookie_set_fn set,size_t keep);
/* Whether a page has scripts that may run: inline ones, or its own site's
   (others' aren't loaded). A page without is shown as without JavaScript,
   with its <noscript> content. */
int browser_scripts_present(const browser_document *);
/* Runs a page's scripts as it loads, then its load events and timers.
   Where they send the browser goes to doc->redirect. When they handle
   clicks and fit in memory they stay in doc->script, and the elements a
   click does something on get the attribute data-flow-click. */
int browser_script_run(browser_document *,browser_fetch_fn,void *,dom_cancel_fn,void *,unsigned,char *,size_t);
/* The user's click on the element whose DOM node has source_id `node`, on
   a page whose scripts stayed, after the values typed or chosen in its
   fields: JSON [[source_id, value, checked, selected option], ...] (null
   where unchanged), or NULL. The page itself isn't changed: *changed gets
   the DOM as the scripts left it (NULL when they didn't change it), and
   *to where they sent the browser. 0, or -1 when the scripts stopped:
   they're freed. */
int browser_script_click(browser_document *doc,int node,const char *values,browser_fetch_fn get,void *ud,dom_cancel_fn cancel,
                         void *cancel_ud,unsigned budget_ms,browser_dom **changed,browser_redirect *to,char *err,size_t errlen);
/* Moves the scripts of `from` to `to`, the page they changed. */
void browser_script_move(browser_document *to,browser_document *from);
void browser_script_free(browser_document *);
int browser_same_origin(const char *,const char *);
#endif

/* Bounded reader view of a streamed DOM, with CSS and optional JavaScript. */
#ifndef FLOW_DOCUMENT_H
#define FLOW_DOCUMENT_H
#include <stddef.h>
#include "url.h"
#include "dom.h"
#include "css.h"
#define BROWSER_PAGE_MAX (64 * 1024 * 1024)   /* downloaded; longer pages are cut there */
#define BROWSER_TEXT_MAX (256 * 1024)
#define BROWSER_LINKS_MAX 256
#define BROWSER_ANCHORS_MAX 128
#define BROWSER_HIDDEN_MAX 16

typedef struct { char url[BROWSER_URL_MAX], label[96]; size_t offset; int node, kind; } browser_link;
typedef struct { char id[96]; size_t offset; } browser_anchor;
/* Where a page sends the browser on its own, as it loads or when clicked:
   its scripts, or a <meta> refresh. `post`: form data to send (malloc'd). */
typedef struct { char url[BROWSER_URL_MAX]; char *post; int replace; } browser_redirect;
#define BROWSER_PARTS_MAX 64
/* A page too long for memory, read in parts from its copy on the memory
   stick (`spool`): where each part known so far starts, with the start
   tags of the elements open there (NULL for the first), and the page's
   title and styles as HTML (`head`), read before each later part. */
typedef struct browser_parts {
    char spool[256], type[64];
    char *head;
    int count;
    struct { size_t offset; char *context; } part[BROWSER_PARTS_MAX];
} browser_parts;
typedef struct {
    char *text;
    char title[128], url[BROWSER_URL_MAX];
    browser_link links[BROWSER_LINKS_MAX];
    int count, shortened, links_omitted;
    browser_dom *dom;
    browser_css css;
    browser_span *spans;
    int span_count, css_omitted, scripts_run, scripts_failed, assets_omitted;
    browser_anchor anchors[BROWSER_ANCHORS_MAX]; int anchor_count;
    size_t source_bytes;
    uint32_t paper;
    void *script;
    int scripting;              /* JavaScript is on: <noscript> is skipped */
    const browser_dom *rendered; /* the DOM the text and styles were last made from */
    struct browser_view *view;  /* the laid-out page, NULL until built */
    struct picture_table *pictures; /* its pictures, when they're loaded (see picture.h) */
    browser_redirect redirect;  /* set when the page sent the browser elsewhere as it loaded */
    browser_parts *parts;       /* a page shown in parts (NULL for most): `part` is the one shown */
    int part;
    /* Elements the user closed (their DOM source_id): notices whose buttons
       did nothing. Added to by the main thread while the page is laid out
       again, never removed. */
    int hidden[BROWSER_HIDDEN_MAX];
    volatile int hidden_count;
} browser_document;
int browser_document_parse(browser_document *doc, const char *data, size_t length,
                           const char *url, const char *content_type, char *err, size_t errlen);
/* Parses a page without rendering it: browser_document_read() is this and
   browser_document_render(). Loading assets first saves a render. */
int browser_document_load(browser_document *doc, dom_read_fn read, void *ud, const char *url, const char *type,
                          dom_cancel_fn cancel, void *cancel_ud, char *err, size_t errlen);
/* Fetches at most `max` bytes of `url`: malloc'd data and its length, and in
   `final` (BROWSER_URL_MAX bytes) the address it came from after redirects.
   NULL when the request fails or the response is larger. */
typedef char *(*browser_asset_fn)(void *ud, const char *url, int max, int *length, char *final);
/* Loads the page's stylesheets, and with `javascript` its same-origin
   scripts, into the DOM, before the page is rendered. */
void browser_assets(browser_document *doc, int javascript, browser_asset_fn fetch, void *ud,
                    dom_cancel_fn cancel, void *cancel_ud);
void browser_document_free(browser_document *doc);
int browser_document_read(browser_document *,dom_read_fn,void *,const char *,const char *,
                          dom_cancel_fn,void *,char *,size_t);
int browser_document_render(browser_document *,char *,size_t);
browser_style browser_style_at(const browser_document *,size_t);
/* A <meta http-equiv=refresh> sending the browser to another address
   within `seconds`: 1 and that address in `url`, else 0. */
int browser_meta_refresh(const browser_document *doc,int seconds,char *url,size_t size);

/* Pages in parts. A reader of part `part` (a dom_read_fn): the page's
   title and styles, the start tags of the elements open where the part
   starts, then the page from there, read with read_at() from its copy. */
typedef int (*browser_read_at_fn)(void *ud,size_t offset,char *out,size_t size);
typedef struct { const browser_parts *parts; int part; browser_read_at_fn read_at; void *ud; size_t position; } browser_part_reader;
void browser_part_reader_init(browser_part_reader *,const browser_parts *,int part,browser_read_at_fn,void *ud);
int browser_part_read(void *ud,char *out,size_t size);
/* The bytes a part's reader gives before the part itself. */
size_t browser_part_prefix(const browser_part_reader *);
/* After reading part `part` of a page (whose parts so far are `known`,
   NULL for a first read with `prefix` 0): doc->parts and doc->part, with
   where the next part starts when the page didn't fit. A page that fit has
   no parts. 0, or -1 without the memory. */
int browser_parts_note(browser_document *doc,const browser_parts *known,int part,size_t prefix);
/* Links to the parts before and after the one shown, added to its DOM:
   to the page's address with #flow-part-<number>. */
void browser_parts_links(browser_document *doc);
browser_parts *browser_parts_copy(const browser_parts *);
void browser_parts_free(browser_parts *);
#endif

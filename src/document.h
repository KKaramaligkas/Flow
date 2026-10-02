/* Bounded reader view of a streamed DOM, with CSS and optional JavaScript. */
#ifndef ARKB_DOCUMENT_H
#define ARKB_DOCUMENT_H
#include <stddef.h>
#include "url.h"
#include "dom.h"
#include "css.h"
#define BROWSER_PAGE_MAX DOM_SOURCE_MAX
#define BROWSER_TEXT_MAX (256 * 1024)
#define BROWSER_LINKS_MAX 256
#define BROWSER_ANCHORS_MAX 128

typedef struct { char url[BROWSER_URL_MAX], label[96]; size_t offset; int node, kind; } browser_link;
typedef struct { char id[96]; size_t offset; } browser_anchor;
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
} browser_document;
int browser_document_parse(browser_document *doc, const char *data, size_t length,
                           const char *url, const char *content_type, char *err, size_t errlen);
void browser_document_free(browser_document *doc);
int browser_document_read(browser_document *,dom_read_fn,void *,const char *,const char *,
                          dom_cancel_fn,void *,char *,size_t);
int browser_document_render(browser_document *,char *,size_t);
browser_style browser_style_at(const browser_document *,size_t);
#endif

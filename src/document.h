/* Text and hyperlinks only: no JavaScript, CSS, forms, or external resources. */
#ifndef ARKB_DOCUMENT_H
#define ARKB_DOCUMENT_H
#include <stddef.h>
#include "url.h"
#define BROWSER_PAGE_MAX (512 * 1024)
#define BROWSER_TEXT_MAX (64 * 1024)
#define BROWSER_LINKS_MAX 128

typedef struct { char url[BROWSER_URL_MAX], label[96]; size_t offset; } browser_link;
typedef struct {
    char *text;
    char title[128], url[BROWSER_URL_MAX];
    browser_link links[BROWSER_LINKS_MAX];
    int count, shortened, links_omitted;
} browser_document;
int browser_document_parse(browser_document *doc, const char *data, size_t length,
                           const char *url, const char *content_type, char *err, size_t errlen);
void browser_document_free(browser_document *doc);
#endif

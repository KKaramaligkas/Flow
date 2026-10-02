#ifndef ARKB_DOM_H
#define ARKB_DOM_H
#include <stddef.h>
#define DOM_NODES_MAX 2048
#define DOM_BYTES_MAX (768 * 1024)
#define DOM_DEPTH_MAX 64
#define DOM_SOURCE_MAX (8 * 1024 * 1024)
#define DOM_TOKEN_MAX 8192
typedef struct { char *name, *value; } dom_attribute;
typedef struct {
    char tag[32];
    char *text;
    dom_attribute *attributes;
    int attribute_count, parent, first, last, next, source_id;
} dom_node;
typedef struct {
    dom_node *nodes;
    int count, shortened, html;
    size_t bytes, source_bytes, script_bytes;
} browser_dom;
typedef int (*dom_read_fn)(void *, char *, size_t);
typedef int (*dom_cancel_fn)(void *);
int dom_parse(browser_dom *, dom_read_fn, void *, int html, int latin,
              dom_cancel_fn, void *, char *, size_t);
void dom_free(browser_dom *);
const char *dom_attr(const dom_node *, const char *);
int dom_set_attr(browser_dom *, int, const char *, const char *);
int dom_add(browser_dom *, int, const char *, const char *);
char *dom_json(const browser_dom *);
int dom_from_json(browser_dom *, const char *, size_t);
/* Decode entities and validate UTF-8, with a bounded output allocation. */
char *browser_decode(const char *, size_t, int entities, int latin);
#endif

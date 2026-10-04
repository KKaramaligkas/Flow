#ifndef FLOW_DOM_H
#define FLOW_DOM_H
#include <stddef.h>
#define DOM_NODES_MAX 16384     /* the node array grows to this as a page needs */
#define DOM_BYTES_MAX (768 * 1024)
#define DOM_DEPTH_MAX 64
#define DOM_SOURCE_MAX (72 * 1024 * 1024)
/* A page longer than memory allows is read in parts, each stopping before
   it holds this many nodes, this much text, or nearly DOM_BYTES_MAX. */
#ifndef DOM_PART_NODES
#define DOM_PART_NODES 12000
#endif
#define DOM_PART_TEXT (400 * 1024)
#define DOM_PART_MARGIN (32 * 1024)
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
    int count, capacity, shortened, html;
    size_t bytes, source_bytes, script_bytes, text_bytes;
    /* A page that needs another part: the source offset where its next
       token starts (0 when the page ended), and the elements open there. */
    size_t cut;
    int cut_stack[DOM_DEPTH_MAX], cut_depth;
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

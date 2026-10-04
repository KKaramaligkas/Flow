/* picture.h: a page's pictures: the source each <img> shows, decoding JPEG,
   PNG and GIF into small images the PSP draws, and the table the page view
   and the loader share. */
#ifndef FLOW_PICTURE_H
#define FLOW_PICTURE_H
#include <stddef.h>
#include "dom.h"

#define PICTURES_MAX 160                    /* per page */
#define PICTURE_BYTES_MAX (1536 * 1024)     /* one download */
#define PICTURE_SIDE_MAX 512                /* decoded: the PSP's largest texture */
#define PICTURE_DATA_MAX (32 * 1024)        /* a data: URI */
#define PICTURE_SCALE 0.6f                  /* page pixels per CSS pixel */

enum { PICTURE_WAITING, PICTURE_READY, PICTURE_FAILED };
/* How the page sizes a picture's box: not at all (its own size does, at
   PICTURE_SCALE), one side (its shape gives the other), or both. */
enum { PICTURE_FIT_OWN, PICTURE_FIT_BOX, PICTURE_FIT_STRETCH };
enum { PIXELS_5650, PIXELS_8888 };          /* the PSP's texture formats: opaque in 16 bits, or with alpha */

typedef struct {
    void *pixels;               /* `stride` pixels a row, 16-byte aligned, and one more row and
                                   column repeating the last, for smooth scaling at the edges */
    int w, h, stride, format;
} picture_image;

/* Decodes a JPEG, PNG or GIF (its first frame) to at most max_w x max_h
   (and PICTURE_SIDE_MAX), never enlarged: keeping its shape, or with
   `stretch` each side on its own, for a box it's stretched to. Its own size
   goes to *natural_w and *natural_h when the header can be read, even if
   decoding then fails. 0 on success. */
int picture_decode(const unsigned char *data, size_t length, int max_w, int max_h, int stretch,
                   picture_image *out, int *natural_w, int *natural_h);
/* Only the size from the header: 0 on success. */
int picture_size(const unsigned char *data, size_t length, int *w, int *h);
void picture_free(picture_image *);
size_t picture_bytes(const picture_image *);
/* The bytes of a base64 data: URI with a picture (malloc'd), NULL when it isn't one. */
unsigned char *picture_data(const char *uri, size_t *length);
/* Memory picture_decode() may need beyond its result, for a picture this size. */
size_t picture_decode_memory(const unsigned char *data, size_t length);

/* The source an <img> shows: its <picture>'s first <source> in a format this
   decodes, srcset (the smallest candidate at least `width` page pixels
   wide), src, or a lazy loader's data-src. Points into the DOM, `*length`
   bytes; NULL when there's none. *density: pixels per CSS pixel (2 for a
   "2x" candidate). */
const char *picture_source(const browser_dom *dom, int img, int width, size_t *length, float *density);

typedef struct {
    char *url;                  /* absolute, or a data: URI */
    int want_w, want_h;         /* the largest box it's shown in, in page pixels (0: any) */
    int sized;                  /* PICTURE_FIT_* */
    int top;                    /* where it first shows on the page */
    float density;              /* pixels per CSS pixel */
    int relayout;               /* the page is laid out again when it arrives (not only for its size) */
    volatile int state;
    int natural_w, natural_h;   /* its pixels, once decoded */
    picture_image image;        /* when PICTURE_READY */
} picture_entry;

/* Entries never move or go away while the page is shown: the drawing thread
   reads them while the loader adds more and fills them in. */
typedef struct picture_table {
    picture_entry entries[PICTURES_MAX];
    volatile int count;
    size_t bytes, budget;       /* decoded pixels kept, and their limit */
    volatile int learned;       /* sizes found since the page was last laid out */
    int nodes;
    short *of_node;             /* each DOM node's entry, -1 for none */
} picture_table;

picture_table *picture_table_new(size_t budget, int nodes);
void picture_table_free(picture_table *);
/* The entry for `url`, added when it's new, and now node's: its index, or -1
   when the table is full. */
int picture_add(picture_table *, int node, const char *url, int want_w, int want_h, int sized, int top, float density);
/* The node's entry, -1 when it has none. */
int picture_of(const picture_table *, int node);
/* The size the node's picture shows at on its own, in page pixels, once
   known: 1, else 0. */
int picture_known(const picture_table *, int node, int *w, int *h);
/* The waiting entry nearest to page rows [top, bottom), or -1. */
int picture_next(const picture_table *, int top, int bottom);
/* Decodes entry i's fetched bytes for the box it shows in, within the
   table's budget (shrunk to fit what's left). Sets its natural size and
   image, not its state. 0 on success. */
int picture_load(picture_table *, int i, const unsigned char *data, size_t length);
#endif

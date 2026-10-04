#ifndef FLOW_CSS_H
#define FLOW_CSS_H
#include <stdint.h>
#include "dom.h"
#define CSS_RULES_MAX 4096
#define CSS_TEXT_MAX (384 * 1024)       /* selectors and declarations kept for a page */
#define CSS_SHEET_MAX (2 * 1024 * 1024) /* one stylesheet as downloaded, before compacting */
#define CSS_SPANS_MAX 4096
/* Media queries see the page view's 472 pixels as 786 CSS pixels (a tablet). */
#define CSS_VIEWPORT_WIDTH 786
#define CSS_VIEWPORT_HEIGHT 453
#define CSS_BOLD 1
#define CSS_ITALIC 2
#define CSS_UNDERLINE 4
#define CSS_UPPERCASE 8
#define CSS_LOWERCASE 16
#define CSS_STRIKE 32
#define CSS_CLIP_TEXT 64    /* while computing: the background shows only through the text */
/* Inherited text style. link: the color of links; list_none: list-style none;
   text-transform is in flags. */
typedef struct { uint32_t color, background, link; float scale; int flags, hidden, block, pre, align, list_none; } browser_style;
/* A rule for one selector. selector, declarations: offsets in browser_css.text.
   kind and key index it by its subject's id, class or tag. */
typedef struct { int selector, declarations, specificity, order, compound; unsigned key; unsigned char kind, variables, compounds; } css_rule;
/* A selector's compound, compiled: hashes of its tag, id and classes, and
   whether it has more (attributes, pseudo-classes) to check by its text. */
typedef struct { unsigned tag, id, classes[3]; short start, end; unsigned char class_count, combinator, rest; } css_compound;
typedef struct css_features css_features;
typedef struct css_variable css_variable;
typedef struct {
    css_rule *rules;
    int count, capacity, omitted;
    char *text;                     /* selectors and declarations */
    size_t used, size, bytes;       /* bytes: stylesheet text added */
    int *index;                     /* rule numbers by (kind, key) */
    css_compound *compounds;        /* of every rule's selector */
    int compound_count, compound_capacity;
    const browser_dom *dom;         /* set before css_add(): rules that can't match it are dropped */
    css_features *features;
    css_variable *variables;        /* custom properties of the root and body elements */
    int variable_count;
} browser_css;
typedef struct { size_t offset; browser_style style; } browser_span;

/* Box properties of one element, in layout pixels (CSS pixels * 0.6, as the
   PSP's default text is about 0.6 times the size of a desktop's). Widths
   below zero are percentages of the containing block. Not inherited. */
#define BOX_AUTO (-32768)
enum { DISPLAY_DEFAULT, DISPLAY_BLOCK, DISPLAY_INLINE, DISPLAY_INLINE_BLOCK, DISPLAY_LIST_ITEM,
       DISPLAY_TABLE, DISPLAY_ROW, DISPLAY_CELL, DISPLAY_FLEX, DISPLAY_NONE, DISPLAY_GRID };
/* How an element lays out its children: display's inner part (inline-flex
   is an inline-block whose children are flex items). */
enum { INNER_FLOW, INNER_FLEX, INNER_GRID };
enum { TRACK_PX, TRACK_PERCENT, TRACK_FR, TRACK_AUTO };
#define GRID_TRACKS 8
#define FLEX_SHRINK_SET 1       /* browser_box.flex: flex-shrink given (else 1) */
#define FLEX_BASIS_SET 2        /* flex-basis given (else auto) */
#define MIN_WIDTH_SET 4         /* min-width given (else auto) */
#define SPAN_ROW 255            /* grid-column: 1 / -1 */
typedef struct {
    short margin[4], padding[4];        /* top, right, bottom, left */
    unsigned char border[4];
    uint32_t border_color[4];
    short width, max_width, height;     /* 0 when not set */
    unsigned char display, set, hide;   /* set: bit i = margin side i, bit 4+i = padding side i */
    unsigned char justify;              /* justify-content: 1 start, 2 center, 3 end, 4 space-between, 5 space-around, 6 space-evenly */
    short gap, row_gap;                 /* flex/grid gaps between columns and between rows */
    unsigned char plain;                /* 1 appearance:none, 2 no border, 4 no background (a button styled as text) */
    /* flex and grid containers */
    unsigned char inner, direction, wrap; /* direction: 0 row, 1 row-reverse, 2 column, 3 column-reverse */
    unsigned char align;                /* align-items: 0 stretch, 1 start, 2 center, 3 end */
    unsigned char tracks, track_kind[GRID_TRACKS];  /* grid-template-columns */
    short track[GRID_TRACKS];           /* layout pixels, percent, or fr in hundredths */
    short fill;                         /* repeat(auto-fill, minmax(fill, 1fr)): the narrowest column */
    /* flex and grid items, and floats */
    unsigned char flex, align_self;     /* flex: *_SET bits; align_self: 0 auto, else align + 1 */
    unsigned char span;                 /* grid columns spanned (0 or 1: one), SPAN_ROW: all */
    unsigned char float_side, clear;    /* float_side: 1 left, 2 right; clear: 1 left, 2 right, 3 both */
    unsigned char positioned;           /* position: absolute or fixed (out of a flex or grid's rows) */
    short grow, shrink;                 /* hundredths */
    short basis, min_width;             /* layout pixels, -percent, or BOX_AUTO */
    short order;
} browser_box;

void css_free(browser_css *);
int css_add(browser_css *,const char *,size_t);
int css_matches(const browser_dom *,int,const char *);
/* Whether a media query list applies to the page view. */
int css_media_matches(const char *);
/* A stylesheet reduced to the rules that can match `dom`, with @media,
   @supports and @layer resolved: malloc'd, its length in *length. */
char *css_compact(const char *,size_t,const browser_dom *,size_t *length);
/* The same for several sheets of a page: what its elements and scripts
   name, found once (NULL without the memory). */
css_features *css_page_features(const browser_dom *);
char *css_compact_for(const char *,size_t,css_features *,size_t *length);
void css_features_free(css_features *);
browser_style css_compute(const browser_css *,const browser_dom *,int,browser_style);
/* css_compute() and the element's box in one pass over the rules. */
browser_style css_compute_box(const browser_css *,const browser_dom *,int,browser_style,browser_box *);
int css_color(const char *,uint32_t *);
#endif

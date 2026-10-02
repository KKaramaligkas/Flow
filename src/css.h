#ifndef ARKB_CSS_H
#define ARKB_CSS_H
#include <stdint.h>
#include "dom.h"
#define CSS_RULES_MAX 256
#define CSS_SPANS_MAX 4096
#define CSS_BOLD 1
#define CSS_ITALIC 2
#define CSS_UNDERLINE 4
#define CSS_UPPERCASE 8
#define CSS_LOWERCASE 16
#define CSS_STRIKE 32
/* Inherited text style. link: the color of links; list_none: list-style none;
   text-transform is in flags. */
typedef struct { uint32_t color, background, link; float scale; int flags, hidden, block, pre, align, list_none; } browser_style;
typedef struct { char selector[192], declarations[512]; int specificity, order; } css_rule;
typedef struct { css_rule *rules; int count, omitted; size_t bytes; } browser_css;
typedef struct { size_t offset; browser_style style; } browser_span;

/* Box properties of one element, in layout pixels (CSS pixels * 0.6, as the
   PSP's default text is about 0.6 times the size of a desktop's). Widths
   below zero are percentages of the containing block. Not inherited. */
#define BOX_AUTO (-32768)
enum { DISPLAY_DEFAULT, DISPLAY_BLOCK, DISPLAY_INLINE, DISPLAY_INLINE_BLOCK, DISPLAY_LIST_ITEM,
       DISPLAY_TABLE, DISPLAY_ROW, DISPLAY_CELL, DISPLAY_FLEX, DISPLAY_NONE };
typedef struct {
    short margin[4], padding[4];        /* top, right, bottom, left */
    unsigned char border[4];
    uint32_t border_color[4];
    short width, max_width, height;     /* 0 when not set */
    unsigned char display, set, hide;   /* set: bit i = margin side i, bit 4+i = padding side i */
    unsigned char justify;              /* flex justify-content: 1 start, 2 center, 3 end */
} browser_box;

void css_free(browser_css *);
int css_add(browser_css *,const char *,size_t);
int css_matches(const browser_dom *,int,const char *);
browser_style css_compute(const browser_css *,const browser_dom *,int,browser_style);
/* css_compute() and the element's box in one pass over the rules. */
browser_style css_compute_box(const browser_css *,const browser_dom *,int,browser_style,browser_box *);
int css_color(const char *,uint32_t *);
#endif

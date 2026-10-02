#ifndef ARKB_CSS_H
#define ARKB_CSS_H
#include <stdint.h>
#include "dom.h"
#define CSS_RULES_MAX 256
#define CSS_SPANS_MAX 4096
#define CSS_BOLD 1
#define CSS_ITALIC 2
#define CSS_UNDERLINE 4
typedef struct { uint32_t color, background; float scale; int flags, hidden, block, pre, align; } browser_style;
typedef struct { char selector[192], declarations[512]; int specificity, order; } css_rule;
typedef struct { css_rule *rules; int count, omitted; size_t bytes; } browser_css;
typedef struct { size_t offset; browser_style style; } browser_span;
void css_free(browser_css *);
int css_add(browser_css *,const char *,size_t);
int css_matches(const browser_dom *,int,const char *);
browser_style css_compute(const browser_css *,const browser_dom *,int,browser_style);
#endif

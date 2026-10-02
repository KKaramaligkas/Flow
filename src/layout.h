#ifndef ARKB_LAYOUT_H
#define ARKB_LAYOUT_H
#include "document.h"
#define BROWSER_LINES_MAX 16384
typedef float (*browser_measure_fn)(void *,const char *,size_t,browser_style);
typedef struct {size_t start,length;float width,height;int align;} browser_line;
int browser_layout(const browser_document *,browser_line *,int,float,browser_measure_fn,void *);
#endif

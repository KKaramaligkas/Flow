/* ARK Browser page view: a laid-out page as a list of positioned items.
   Coordinates are page pixels: x from the left edge, y from the top. */
#ifndef ARKB_VIEW_H
#define ARKB_VIEW_H
#include <stddef.h>
#include <stdint.h>
#include "document.h"

#define VIEW_WIDTH          472     /* the PSP screen less the scroll bar */
#define VIEW_ITEMS_MAX      16384
#define VIEW_TEXT_MAX       (512 * 1024)
#define VIEW_LINKS_MAX      1024
#define VIEW_CONTROLS_MAX   256
#define VIEW_OPTIONS_MAX    1024
#define VIEW_FORMS_MAX      32
#define VIEW_ANCHORS_MAX    256
#define VIEW_VALUE_MAX      1024    /* bytes of one form field */

enum { ITEM_TEXT, ITEM_RECT, ITEM_FRAME, ITEM_CONTROL, ITEM_IMAGE, ITEM_BULLET };
#define CONTROL_PLAIN 1     /* view_item.flags of a button the page styles as text */
enum { CONTROL_TEXT, CONTROL_PASSWORD, CONTROL_TEXTAREA, CONTROL_CHECKBOX, CONTROL_RADIO, CONTROL_SELECT,
       CONTROL_SUBMIT, CONTROL_RESET, CONTROL_BUTTON, CONTROL_IMAGE, CONTROL_HIDDEN, CONTROL_FILE };

typedef struct {
    int x, y, w, h;
    uint32_t color;         /* text, fill, border or bullet color */
    int text, length;       /* ITEM_TEXT: UTF-8 bytes in view->text; ITEM_IMAGE: alt text */
    float scale;            /* text size */
    short flags;            /* CSS_* text flags; ITEM_FRAME: border width */
    unsigned char kind;
    short link, control;    /* -1 when the item isn't part of a link or form field */
} view_item;

typedef struct { int label, value; } view_option;      /* offsets in view->text */
typedef struct {
    int kind, form, item, name, label;      /* name, label: offsets in view->text */
    char *value, *initial;                  /* NUL-terminated, at most VIEW_VALUE_MAX */
    int checked, checked_initial, selected, selected_initial, option_first, option_count;
    int disabled, readonly, maxlength;
    int single;                             /* a one-line text field */
} view_control;
typedef struct { int action, post, multipart, fields; } view_form;   /* fields: text fields */
typedef struct { int url; } view_link;
typedef struct { int id, y; } view_anchor;

typedef struct browser_view {
    view_item *items;
    int item_count, item_capacity;
    int *reach, *rest;              /* culling: max bottom of items[0..i], min top of items[i..] */
    char *text;                     /* the text of ITEM_TEXT and ITEM_IMAGE items */
    size_t text_used, text_capacity;
    char *strings;                  /* addresses, names, labels: view_string() */
    size_t strings_used, strings_capacity;
    view_link *links;
    view_control *controls;
    view_option *options;
    view_form *forms;
    view_anchor *anchors;
    int link_count, control_count, option_count, form_count, anchor_count;
    int width, height, truncated, images;
    uint32_t paper, ink;
} browser_view;

/* Width of `len` bytes of UTF-8 text; flags are CSS_* flags. */
typedef float (*view_measure_fn)(const char *, int len, float scale, int flags);

int browser_view_build(browser_view *, const browser_document *, int width, view_measure_fn, char *err, size_t errlen);
void browser_view_free(browser_view *);
static inline const char *view_string(const browser_view *v, int offset) { return offset >= 0 ? v->strings + offset : ""; }

/* Items that may intersect page rows [top, bottom): call with i from
   browser_view_first() while browser_view_more() is true. */
int browser_view_first(const browser_view *, int top);
static inline int browser_view_more(const browser_view *v, int i, int bottom) { return i < v->item_count && v->rest[i] < bottom; }

/* The link and form field at a page point (-1 for none). Returns the item. */
int browser_view_hit(const browser_view *, int x, int y, int *link, int *control);
/* The next (direction 1) or previous (-1) clickable item in reading order
   from item `from` (or from the page point x, y when `from` is -1),
   skipping the rest of that item's link or field. */
int browser_view_step(const browser_view *, int from, int x, int y, int direction);
/* The page y of an id or name, -1 when unknown. */
int browser_view_anchor(const browser_view *, const char *id);

/* Form fields */
int browser_view_set_value(browser_view *, int control, const char *value);
void browser_view_toggle(browser_view *, int control);
void browser_view_reset(browser_view *, int form);
/* Non-zero when editing this field should submit its form (a search box:
   the form's only one-line text field). */
int browser_view_submits_on_enter(const browser_view *, int control);
/* Builds the request for submitting `form` with `submitter` (a submit
   button, or -1). GET: the URL with the form data as its query, *body NULL.
   POST: *body is malloc'd application/x-www-form-urlencoded data. */
int browser_view_submit(const browser_view *, int form, int submitter, char *url, size_t url_size,
                        char **body, char *err, size_t errlen);
#endif

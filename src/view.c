/* Flow page view: lays out the DOM with its CSS into positioned
   items, much like a desktop browser's normal flow, scaled for the PSP:
   blocks stack, inline text wraps into lines, tables split their width
   between columns, and form fields, images and list markers are boxes. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "view.h"
#include "url.h"
#include "picture.h"

#define PIECES_MAX  1024        /* pieces on the open lines */
#define ASCENT      13.0f       /* firmware font metrics at size 1.0 */
#define LINE        17.0f
#define FIELD_H     17
#define DEFAULT_LINK 0xffee0000u    /* #0000EE */
#define TEXT_SCALE  0.64f           /* body text (16 CSS pixels) */

/* A piece of the line being built: a run of text, or a box (form field,
   image) whose items were laid out at (0, 0) and move when the line ends. */
typedef struct {
    int box, text, length, first, count;
    float width, ascent, descent, scale;
    uint32_t color, background;     /* background: an inline element's, with `pad` above and below */
    int flags, link, control, pad;
} piece;

typedef struct {
    int x, width, align, first, count;
    float used;
    int space;                  /* collapsed whitespace waits for the next word */
    browser_style space_style;  /* ...with the style and link where it was */
    int space_link, space_control;
    int full, full_x, full_width;   /* the room without floats, once a line was shortened */
} line_state;

#define FLOATS_MAX 32
#define ROOM_MIN 60             /* narrower room beside floats is skipped */
typedef struct { int x, y, w, h, side; } float_area;

typedef struct {
    browser_view *v;
    const browser_document *doc;
    const browser_dom *dom;
    view_measure_fn measure;
    char base[BROWSER_URL_MAX];
    int *control_of;            /* DOM node -> control */
    piece pieces[PIECES_MAX];
    int piece_count;
    line_state *line;
    int *y;                     /* the y of the open line's block */
    int collapse;               /* bottom margin already added below the last block */
    int link, label;            /* the link and field that clicks go to */
    int consent;                /* the cookie consent notice laid out (its source_id), -1 outside one */
    int pad;                    /* vertical padding of the inline background */
    int list_kind, *list_number, marker, marker_number;
    float marker_scale;
    uint32_t marker_color;
    int depth, scripting, failed;
    const char *name;           /* an icon link's name, shown in place of its content */
    int naming;                 /* inside icon links showing their names */
    uint32_t backdrop;          /* the background drawn behind the open block (0: the paper) */
    int item_width;             /* >= 0: the next element is a flex or grid item this wide */
    int budget;                 /* elements content_widths() may still visit (0: no limit) */
    struct { int node, background, border[4]; } last;   /* the block laid out last, for stretching */
    float_area floats[FLOATS_MAX];  /* floated boxes, in page pixels with their margins */
    int float_count;
    struct measured { float minimum, maximum; } *measured;  /* inner_widths() by node, when known */
    unsigned char *known;
} builder;

static int eq(const char *a, const char *b)
{
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
static int is(const dom_node *n, const char *tag) { return eq(n->tag, tag); }
static int clampi(int v, int low, int high) { return v < low ? low : v > high ? high : v; }

/* ---- storage ---- */

static int grow(builder *b, char **buffer, size_t *used, size_t *capacity, const char *s, size_t n, int terminate)
{
    if (*used + n + 1 > VIEW_TEXT_MAX) { b->v->truncated = 1; return -1; }
    if (*used + n + 1 > *capacity) {
        size_t cap = *capacity ? *capacity : 16 * 1024;
        while (cap < *used + n + 1) cap *= 2;
        if (cap > VIEW_TEXT_MAX) cap = VIEW_TEXT_MAX;
        char *t = realloc(*buffer, cap);
        if (!t) { b->failed = 1; return -1; }
        *buffer = t; *capacity = cap;
    }
    int at = (int)*used;
    memcpy(*buffer + at, s, n);
    *used += n;
    if (terminate) (*buffer)[(*used)++] = 0;
    return at;
}
/* Text of items: runs stay contiguous so that they can grow. */
static int store(builder *b, const char *s, size_t n, int terminate)
{
    return grow(b, &b->v->text, &b->v->text_used, &b->v->text_capacity, s, n, terminate);
}
static int store_string(builder *b, const char *s)
{
    return grow(b, &b->v->strings, &b->v->strings_used, &b->v->strings_capacity, s, strlen(s), 1);
}

static int add_item(builder *b, view_item it)
{
    browser_view *v = b->v;
    if (v->item_count >= v->item_capacity) {
        /* Grows as needed: most pages need a small part of the maximum. */
        int capacity = v->item_capacity * 2;
        if (capacity > VIEW_ITEMS_MAX) capacity = VIEW_ITEMS_MAX;
        view_item *items = v->item_count < capacity ? realloc(v->items, (size_t)capacity * sizeof(*items)) : NULL;
        if (!items) { v->truncated = 1; return -1; }
        v->items = items;
        v->item_capacity = capacity;
    }
    v->items[v->item_count] = it;
    return v->item_count++;
}
static view_item item(int kind, int x, int y, int w, int h, uint32_t color)
{
    view_item it = { x, y, w, h, color, -1, 0, 0, 0, (unsigned char)kind, -1, -1 };
    return it;
}

/* ---- text measurement ---- */

static float width_of(builder *b, const char *s, int n, float scale, int flags)
{
    return n > 0 ? b->measure(s, n, scale, flags) : 0;
}
static float ascent(float scale) { return ASCENT * scale; }
static float descent(float scale) { return (LINE - ASCENT) * scale; }

/* ---- lines ---- */

/* Roughly how bright a colour looks (0..1000): gamma 2 in place of the
   sRGB curve, with the eye's weights for red, green and blue. */
static int brightness(uint32_t c)
{
    unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
    return (int)((2126u * r * r + 7152u * g * g + 722u * bl * bl) / 650250u);
}

/* Text in a colour that vanishes into what is behind it (white text meant
   for a picture we don't draw, transparent text over a gradient) is shown
   dark on a light background and light on a dark one. Links keep looking
   like links. */
static uint32_t readable(builder *b, uint32_t color, uint32_t background, int link)
{
    uint32_t under = background ? background : b->backdrop ? b->backdrop : b->v->paper;
    int text = brightness(color), back = brightness(under);
    int light = text > back ? text : back, dark = text > back ? back : text;
    if (color >> 24 && (light + 50) * 10 >= (dark + 50) * 15) return color;     /* contrast ratio 1.5 or more */
    if (back > 180) return link >= 0 ? DEFAULT_LINK : 0xff202020u;
    return link >= 0 ? 0xfff8b48au : 0xffe8e8e8u;
}

static void place_marker(builder *b, int line_x, int top, float asc)
{
    if (!b->marker) return;
    float s = b->marker_scale;
    if (b->marker == 1) {
        int size = s > 0.7f ? 5 : 4;
        view_item it = item(ITEM_BULLET, line_x - 10, top + (int)(asc - ascent(s) + (LINE * s - size) / 2), size, size, readable(b, b->marker_color, 0, -1));
        add_item(b, it);
    } else {
        char number[16];
        int n = snprintf(number, sizeof(number), "%d.", b->marker_number);
        int at = store(b, number, (size_t)n, 0);
        float w = width_of(b, number, n, s, 0);
        if (at >= 0) {
            view_item it = item(ITEM_TEXT, line_x - 4 - (int)w, top + (int)(asc - ascent(s)), (int)w + 1, (int)(LINE * s), readable(b, b->marker_color, 0, -1));
            it.text = at; it.length = n; it.scale = s;
            add_item(b, it);
        }
    }
    b->marker = 0;
}

/* The room at page row y between floats, within [x, x + width): the left and
   right edges, and the row where the first float there ends (-1: none). */
static int band(builder *b, int y, int x, int width, int *left, int *right)
{
    int next = -1;
    *left = x; *right = x + width;
    for (int i = 0; i < b->float_count; i++) {
        const float_area *f = &b->floats[i];
        if (y < f->y || y >= f->y + f->h || f->x >= x + width || f->x + f->w <= x) continue;
        if (f->side == 1 && f->x + f->w > *left) *left = f->x + f->w;
        if (f->side == 2 && f->x < *right) *right = f->x;
        if (next < 0 || f->y + f->h < next) next = f->y + f->h;
    }
    return next;
}

static void full_room(line_state *l)
{
    if (l->full) { l->x = l->full_x; l->width = l->full_width; }
}

/* Before a line's first piece: floats beside it take room from it, and a
   line that would be too narrow for `need` moves down below them. */
static void line_room(builder *b, float need)
{
    line_state *l = b->line;
    if (!l || l->count || !b->float_count) return;
    if (!l->full) { l->full = 1; l->full_x = l->x; l->full_width = l->width; }
    int wanted = need > ROOM_MIN ? (int)need : ROOM_MIN;
    if (wanted > l->full_width) wanted = l->full_width;
    for (int pass = 0; pass <= FLOATS_MAX; pass++) {
        int left, right, next = band(b, *b->y, l->full_x, l->full_width, &left, &right);
        l->x = left; l->width = right - left;
        if (next < 0 || l->width >= wanted) return;
        *b->y = next;
        b->collapse = 0;
    }
}

static void flush_line(builder *b)
{
    line_state *l = b->line;
    if (!l) return;
    if (!l->count) { l->space = 0; full_room(l); return; }
    piece *p = &b->pieces[l->first];
    /* A space at the end of a line takes no room. */
    piece *last = &p[l->count - 1];
    if (!last->box && last->length && b->v->text[last->text + last->length - 1] == ' ') {
        last->length--;
        last->width -= width_of(b, " ", 1, last->scale, last->flags);
        if (!last->length) l->count--;
    }
    float asc = 0, desc = 0, used = 0, text = 0;
    for (int i = 0; i < l->count; i++) {
        if (p[i].ascent > asc) asc = p[i].ascent;
        if (p[i].descent > desc) desc = p[i].descent;
        if (!p[i].box && p[i].scale > text) text = p[i].scale;
        used += p[i].width;
    }
    /* Leading: lines of text a little apart, as with CSS's normal line height. */
    int leading = (int)(LINE * text * 0.2f + 0.5f);
    int top = *b->y + leading / 2;
    float x = (float)l->x;
    if (l->align == 1 && used < l->width) x += (l->width - used) / 2;
    else if (l->align == 2 && used < l->width) x += l->width - used;
    place_marker(b, l->x, top, asc);
    for (int i = 0; i < l->count; i++) {
        int px = (int)(x + 0.5f), py = top + (int)(asc - p[i].ascent + 0.5f);
        if (p[i].background && p[i].width > 0) {
            int h = (int)(LINE * p[i].scale + 0.5f);
            view_item bg = item(ITEM_RECT, px, py - p[i].pad, (int)(p[i].width + 0.99f), h + 2 * p[i].pad, p[i].background);
            bg.link = (short)p[i].link; bg.control = (short)p[i].control;
            add_item(b, bg);
        }
        if (p[i].box) {
            for (int k = p[i].first; k < p[i].first + p[i].count && k < b->v->item_count; k++) {
                b->v->items[k].x += px;
                b->v->items[k].y += py;
            }
        } else if (p[i].length) {
            view_item it = item(ITEM_TEXT, px, py, (int)(p[i].width + 0.99f), (int)(LINE * p[i].scale + 0.5f), readable(b, p[i].color, p[i].background, p[i].link));
            it.text = p[i].text; it.length = p[i].length; it.scale = p[i].scale; it.flags = (short)p[i].flags;
            it.link = (short)p[i].link; it.control = (short)p[i].control;
            add_item(b, it);
        }
        x += p[i].width;
    }
    int height = (int)(asc + desc + 0.99f);
    *b->y = top + (height > 0 ? height : 1) + (leading - leading / 2);
    b->piece_count = l->first;
    l->count = 0; l->used = 0; l->space = 0;
    b->collapse = 0;
    full_room(l);
}

static piece *new_piece(builder *b)
{
    if (b->piece_count >= PIECES_MAX) flush_line(b);
    if (b->piece_count >= PIECES_MAX) return NULL;
    line_state *l = b->line;
    piece *p = &b->pieces[b->piece_count++];
    memset(p, 0, sizeof(*p));
    l->count++;
    return p;
}

/* Appends text to the line, joining it to the previous run of the same style. */
static void add_run(builder *b, const char *s, int n, float width, browser_style st, int link, int control)
{
    line_state *l = b->line;
    if (l->count) {
        piece *p = &b->pieces[l->first + l->count - 1];
        if (!p->box && p->scale == st.scale && p->color == st.color && p->flags == st.flags && p->link == link &&
            p->control == control && p->background == st.background && p->pad == b->pad &&
            p->text + p->length == (int)b->v->text_used) {
            if (store(b, s, (size_t)n, 0) < 0) return;
            p->length += n; p->width += width; l->used += width;
            return;
        }
    }
    int at = store(b, s, (size_t)n, 0);
    if (at < 0) return;
    piece *p = new_piece(b);
    if (!p) return;
    p->text = at; p->length = n; p->width = width; p->scale = st.scale; p->color = st.color; p->flags = st.flags;
    p->ascent = ascent(st.scale); p->descent = descent(st.scale); p->link = link; p->control = control;
    p->background = st.background; p->pad = b->pad;
    l->used += width;
}

/* Room for an inline element's margin or padding (with its background). */
static void add_gap(builder *b, int width, uint32_t background, browser_style st)
{
    line_state *l = b->line;
    if (width <= 0 || !l) return;
    if (l->space && l->count) {
        /* The space before the element comes before its padding. */
        browser_style gap = l->space_style;
        float space = width_of(b, " ", 1, gap.scale, gap.flags);
        if (l->used + space <= l->width) add_run(b, " ", 1, space, gap, l->space_link, l->space_control);
    }
    l->space = 0;
    if (l->count && l->used + width > l->width) flush_line(b);
    line_room(b, (float)width);
    piece *p = new_piece(b);
    if (!p) return;
    p->box = 1; p->first = b->v->item_count; p->count = 0; p->width = (float)width;
    p->background = background; p->pad = b->pad; p->scale = st.scale;
    if (background) { p->ascent = ascent(st.scale); p->descent = descent(st.scale); }
    p->link = b->link; p->control = b->label;
    l->used += width;
}

static int utf8_length(const char *s)
{
    unsigned char c = (unsigned char)*s;
    return c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
}

/* A word joined to the text before it ("<b>word</b>.") may only break where
   that text has a space: the part after the space moves to a new line with
   it. Fields, images and inline padding are break points too. Returns 0
   when there is no such point, or the joined words don't fit on a line. */
static int carry_word(builder *b, float width)
{
    line_state *l = b->line;
    piece *p = &b->pieces[l->first];
    int i = l->count - 1, cut = -1;
    if (i < 0 || p[i].box) return 0;
    float tail = 0;
    for (; i >= 0; i--) {
        if (p[i].box) { cut = 0; i++; break; }
        const char *s = b->v->text + p[i].text;
        int k = p[i].length;
        while (k > 0 && s[k - 1] != ' ') k--;
        if (k > 0) { cut = k; break; }
        tail += p[i].width;
    }
    if (cut < 0 || (i == 0 && cut == 0)) return 0;
    float rest = cut ? width_of(b, b->v->text + p[i].text + cut, p[i].length - cut, p[i].scale, p[i].flags) : 0;
    if (tail + rest + width > l->width) return 0;
    piece moved[16];
    int count = 0;
    if (cut && cut < p[i].length) {
        moved[count] = p[i];
        moved[count].text += cut; moved[count].length -= cut; moved[count].width = rest;
        count++;
    }
    int from = cut ? i + 1 : i;
    if (l->count - from + count > (int)(sizeof(moved) / sizeof(*moved))) return 0;
    for (int k = from; k < l->count; k++) moved[count++] = p[k];
    if (cut) { p[i].width -= rest; p[i].length = cut; l->count = i + 1; }
    else l->count = i;
    flush_line(b);
    float moving = 0;
    for (int k = 0; k < count; k++) moving += moved[k].width;
    line_room(b, moving);
    for (int k = 0; k < count; k++) {
        piece *q = new_piece(b);
        if (!q) return 1;
        *q = moved[k];
        l->used += q->width;
    }
    return 1;
}

static void transform(char *s, int n, int flags)
{
    if (!(flags & (CSS_UPPERCASE | CSS_LOWERCASE))) return;
    for (int i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x80) s[i] = (char)((flags & CSS_UPPERCASE) ? toupper((unsigned char)s[i]) : tolower((unsigned char)s[i]));
}

static void add_word(builder *b, const char *word, int n, browser_style st)
{
    line_state *l = b->line;
    char copy[512];
    if (n <= 0) return;
    if (n >= (int)sizeof(copy)) n = (int)sizeof(copy) - 1;
    memcpy(copy, word, (size_t)n);
    transform(copy, n, st.flags);
    if (!l->space && l->count && b->link >= 0) {
        /* Links that touch, as in a menu spaced out by CSS, keep a space apart. */
        const piece *last = &b->pieces[l->first + l->count - 1];
        if (!last->box && last->link >= 0 && last->link != b->link) {
            l->space = 1; l->space_style = st; l->space_style.flags &= ~(CSS_UNDERLINE | CSS_STRIKE);
            l->space_link = -1; l->space_control = -1;
        }
    }
    browser_style gap = l->space_style;
    float space = l->space && l->count ? width_of(b, " ", 1, gap.scale, gap.flags) : 0;
    float width = width_of(b, copy, n, st.scale, st.flags);
    if (l->count && l->used + space + width > l->width) {
        if (l->space || !carry_word(b, width)) flush_line(b);
        space = 0;
    }
    line_room(b, width);
    if (space > 0) add_run(b, " ", 1, space, gap, l->space_link, l->space_control);
    l->space = 0;
    if (width <= l->width || l->width < 8) { add_run(b, copy, n, width, st, b->link, b->label); return; }
    /* A word wider than the line, such as a long address: break it anywhere. */
    int start = 0;
    while (start < n) {
        int end = start;
        float w = 0;
        while (end < n) {
            int k = utf8_length(copy + end);
            if (end + k > n) k = n - end;
            float cw = width_of(b, copy + end, k, st.scale, st.flags);
            if (w + cw > l->width - l->used && end > start) break;
            w += cw; end += k;
        }
        add_run(b, copy + start, end - start, w, st, b->link, b->label);
        start = end;
        if (start < n) { flush_line(b); line_room(b, 0); }
    }
}

static void add_text(builder *b, const char *text, browser_style st)
{
    line_state *l = b->line;
    if (!text || !l) return;
    const char *p = text;
    if (st.pre) {
        while (*p) {
            if (*p == '\n') {
                if (!l->count) { int h = (int)(LINE * st.scale); *b->y += h; b->collapse = 0; }
                else flush_line(b);
                p++;
                continue;
            }
            const char *start = p;
            while (*p && *p != '\n' && *p != ' ' && *p != '\t') p++;
            if (p > start) add_word(b, start, (int)(p - start), st);
            while (*p == ' ' || *p == '\t') {
                int n = *p == '\t' ? 4 : 1;
                for (int i = 0; i < n; i++) {
                    float w = width_of(b, " ", 1, st.scale, st.flags);
                    line_room(b, w);
                    if (l->used + w <= l->width) add_run(b, " ", 1, w, st, b->link, b->label);
                }
                p++;
            }
        }
        return;
    }
    while (*p) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*p)) p++;
            if (l->count && !l->space) { l->space = 1; l->space_style = st; l->space_link = b->link; l->space_control = b->label; }
            continue;
        }
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        add_word(b, start, (int)(p - start), st);
    }
}

/* Adds a box of `w` x `h` whose items [first, item_count) were made at (0, 0). */
static void add_box(builder *b, int first, int w, int h, float base)
{
    line_state *l = b->line;
    int count = b->v->item_count - first;
    if (!l->space && l->count) {
        /* A field or picture right after a link or another field. */
        const piece *last = &b->pieces[l->first + l->count - 1];
        if ((!last->box && last->link >= 0) || (last->box && last->count > 0)) l->space = 1;
    }
    if (l->count && l->used + (l->space ? 4 : 0) + w > l->width) flush_line(b);
    line_room(b, (float)w);
    if (l->space && l->count) {
        piece *gap = new_piece(b);
        if (gap) { gap->box = 1; gap->first = first; gap->count = 0; gap->width = 4; l->used += 4; }
    }
    l->space = 0;
    piece *p = new_piece(b);
    if (!p) return;
    p->box = 1; p->first = first; p->count = count; p->width = (float)w;
    p->ascent = base; p->descent = h - base > 0 ? h - base : 0;
    l->used += w;
}

static void line_break(builder *b, float scale)
{
    line_state *l = b->line;
    if (!l) return;
    if (l->count) flush_line(b);
    else { *b->y += (int)(LINE * scale); b->collapse = 0; }
}

/* ---- forms ---- */

static void text_content(const browser_dom *dom, int node, char *out, size_t size, int depth)
{
    const dom_node *n = &dom->nodes[node];
    if (depth > 16) return;
    if (n->text && !strcmp(n->tag, "#text")) {
        size_t used = strlen(out), len = strlen(n->text);
        if (len > size - used - 1) len = size - used - 1;
        memcpy(out + used, n->text, len);
        out[used + len] = 0;
    }
    for (int c = n->first; c >= 0; c = dom->nodes[c].next) text_content(dom, c, out, size, depth + 1);
}
static void collapse_spaces(char *s)
{
    char *w = s;
    int space = 1;
    for (char *r = s; *r; r++) {
        if (isspace((unsigned char)*r)) { if (!space) *w++ = ' '; space = 1; }
        else { *w++ = *r; space = 0; }
    }
    if (w > s && w[-1] == ' ') w--;
    *w = 0;
}
static char *copy_value(const char *s)
{
    size_t n = strlen(s);
    if (n > VIEW_VALUE_MAX) n = VIEW_VALUE_MAX;
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    char *p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

static int control_kind(const dom_node *n)
{
    if (is(n, "textarea")) return CONTROL_TEXTAREA;
    if (is(n, "select")) return CONTROL_SELECT;
    if (is(n, "button")) {
        const char *t = dom_attr(n, "type");
        return eq(t, "reset") ? CONTROL_RESET : eq(t, "button") ? CONTROL_BUTTON : CONTROL_SUBMIT;
    }
    const char *t = dom_attr(n, "type");
    static const struct { const char *type; int kind; } kinds[] = {
        {"password", CONTROL_PASSWORD}, {"checkbox", CONTROL_CHECKBOX}, {"radio", CONTROL_RADIO},
        {"submit", CONTROL_SUBMIT}, {"reset", CONTROL_RESET}, {"button", CONTROL_BUTTON},
        {"image", CONTROL_IMAGE}, {"hidden", CONTROL_HIDDEN}, {"file", CONTROL_FILE}};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) if (eq(t, kinds[i].type)) return kinds[i].kind;
    return CONTROL_TEXT;    /* text, search, email, url, tel, number and unknown types */
}

static void add_options(builder *b, view_control *c, int node, int depth)
{
    const browser_dom *dom = b->dom;
    browser_view *v = b->v;
    for (int i = dom->nodes[node].first; i >= 0 && depth < 4; i = dom->nodes[i].next) {
        const dom_node *o = &dom->nodes[i];
        if (is(o, "optgroup")) { add_options(b, c, i, depth + 1); continue; }
        if (!is(o, "option") || v->option_count >= VIEW_OPTIONS_MAX) continue;
        char label[192] = "";
        if (*dom_attr(o, "label")) snprintf(label, sizeof(label), "%s", dom_attr(o, "label"));
        else text_content(dom, i, label, sizeof(label), 0);
        collapse_spaces(label);
        int has_value = 0;
        for (int k = 0; k < o->attribute_count; k++) if (eq(o->attributes[k].name, "value")) has_value = 1;
        view_option *opt = &v->options[v->option_count];
        opt->label = store_string(b, label);
        opt->value = store_string(b, has_value ? dom_attr(o, "value") : label);
        if (opt->label < 0 || opt->value < 0) return;
        for (int k = 0; k < o->attribute_count; k++)
            if (eq(o->attributes[k].name, "selected")) c->selected = c->selected_initial = v->option_count - c->option_first;
        v->option_count++;
        c->option_count++;
    }
}

/* aria-label, title, or the alt or aria-label of a picture inside. */
static void accessible_name(const browser_dom *dom, int node, char *out, size_t size)
{
    const dom_node *n = &dom->nodes[node];
    const char *name = *dom_attr(n, "aria-label") ? dom_attr(n, "aria-label") : dom_attr(n, "title");
    snprintf(out, size, "%s", name);
    for (int c = n->first; c >= 0 && !*out; c = dom->nodes[c].next) {
        const dom_node *k = &dom->nodes[c];
        if (is(k, "img")) snprintf(out, size, "%s", dom_attr(k, "alt"));
        else if (is(k, "svg")) snprintf(out, size, "%s", *dom_attr(k, "aria-label") ? dom_attr(k, "aria-label") : dom_attr(k, "title"));
        else if (k->tag[0] != '#') accessible_name(dom, c, out, size);
    }
    collapse_spaces(out);
}
/* Whether an element shows anything of its own: text, a picture with a
   description, a field. */
static int has_content(const browser_dom *dom, int node, int depth)
{
    for (int c = dom->nodes[node].first; c >= 0 && depth < 16; c = dom->nodes[c].next) {
        const dom_node *k = &dom->nodes[c];
        if (!strcmp(k->tag, "#text")) { for (const char *p = k->text ? k->text : ""; *p; p++) if (!isspace((unsigned char)*p)) return 1; continue; }
        if ((is(k, "img") && *dom_attr(k, "alt")) || is(k, "input") || is(k, "select") || is(k, "textarea") || is(k, "button")) return 1;
        if (has_content(dom, c, depth + 1)) return 1;
    }
    return 0;
}
/* An icon's name is text, not as big as the icon its font size made. */
static browser_style name_style(browser_style st)
{
    if (st.scale > TEXT_SCALE) st.scale = TEXT_SCALE;
    return st;
}
static int has_attr(const dom_node *n, const char *name)
{
    for (int i = 0; i < n->attribute_count; i++) if (eq(n->attributes[i].name, name)) return 1;
    return 0;
}
/* Whether an element holds a picture that has arrived: an icon link shows
   that rather than its name. */
static int has_picture(builder *b, int node, int depth)
{
    const picture_table *t = b->doc->pictures;
    if (!t) return 0;
    for (int c = b->dom->nodes[node].first; c >= 0 && depth < 8; c = b->dom->nodes[c].next) {
        const dom_node *k = &b->dom->nodes[c];
        int i = is(k, "img") ? picture_of(t, k->source_id) : -1;
        if (i >= 0 && t->entries[i].state == PICTURE_READY) return 1;
        if (k->tag[0] != '#' && has_picture(b, c, depth + 1)) return 1;
    }
    return 0;
}

/* Every form and field in document order, before layout: hidden fields
   are submitted too, and labels can name fields that come after them. */
static void register_forms(builder *b)
{
    const browser_dom *dom = b->dom;
    browser_view *v = b->v;
    if (dom->count <= 0) return;
    int *form_of = calloc((size_t)dom->count, sizeof(int));
    if (!form_of) { b->failed = 1; return; }
    for (int i = 0; i < dom->count; i++) {
        const dom_node *n = &dom->nodes[i];
        int parent = n->parent;
        form_of[i] = parent >= 0 ? form_of[parent] : -1;
        b->control_of[i] = -1;
        if (is(n, "form") && v->form_count < VIEW_FORMS_MAX) {
            view_form *f = &v->forms[v->form_count];
            char action[BROWSER_URL_MAX];
            const char *a = dom_attr(n, "action");
            if (!*a || browser_url_resolve(b->base, a, action, sizeof(action)) < 0) strcpy(action, b->doc->url);
            action[strcspn(action, "#")] = 0;
            f->action = store_string(b, action);
            f->post = eq(dom_attr(n, "method"), "post");
            f->multipart = eq(dom_attr(n, "enctype"), "multipart/form-data");
            f->fields = 0;
            f->scripted = has_attr(n, "data-flow-click");
            form_of[i] = v->form_count++;
        }
        if (!(is(n, "input") || is(n, "select") || is(n, "textarea") || is(n, "button"))) continue;
        if (v->control_count >= VIEW_CONTROLS_MAX) { v->truncated = 1; continue; }
        view_control *c = &v->controls[v->control_count];
        memset(c, 0, sizeof(*c));
        c->kind = control_kind(n);
        c->form = form_of[i];
        c->item = -1;
        c->node = n->source_id;
        c->dismiss = -1;
        /* A button's click, or a form's submission, the page's scripts handle */
        c->scripted = has_attr(n, "data-flow-click") ||
                      (c->form >= 0 && v->forms[c->form].scripted && (c->kind == CONTROL_SUBMIT || c->kind == CONTROL_IMAGE));
        c->name = store_string(b, dom_attr(n, "name"));
        c->disabled = has_attr(n, "disabled");
        c->readonly = has_attr(n, "readonly");
        c->maxlength = atoi(dom_attr(n, "maxlength"));
        char label[256] = "";
        if (c->kind == CONTROL_TEXTAREA) {
            char text[VIEW_VALUE_MAX + 1] = "";
            text_content(dom, i, text, sizeof(text), 0);
            c->value = copy_value(text);
        } else if (c->kind == CONTROL_SELECT) {
            c->option_first = v->option_count;
            add_options(b, c, i, 0);
            c->value = copy_value("");
        } else c->value = copy_value(dom_attr(n, "value"));
        if (c->kind == CONTROL_SUBMIT || c->kind == CONTROL_RESET || c->kind == CONTROL_BUTTON) {
            if (is(n, "button")) { text_content(dom, i, label, sizeof(label), 0); collapse_spaces(label); }
            else snprintf(label, sizeof(label), "%s", dom_attr(n, "value"));
            /* An icon button: its accessible name. A script's button
               without one isn't shown (label left empty). */
            if (!*label) accessible_name(dom, i, label, sizeof(label));
            if (!*label && c->kind != CONTROL_BUTTON) snprintf(label, sizeof(label), "%s", c->kind == CONTROL_RESET ? "Reset" : "Submit");
        } else if (c->kind == CONTROL_IMAGE) snprintf(label, sizeof(label), "%s", *dom_attr(n, "alt") ? dom_attr(n, "alt") : "Submit");
        else if (c->kind == CONTROL_FILE) snprintf(label, sizeof(label), "Choose file (not supported)");
        else snprintf(label, sizeof(label), "%s", *dom_attr(n, "placeholder") ? dom_attr(n, "placeholder") : dom_attr(n, "aria-label"));
        c->label = store_string(b, label);
        c->checked = c->checked_initial = has_attr(n, "checked");
        c->initial = copy_value(c->value ? c->value : "");
        if (!c->value || !c->initial) { b->failed = 1; free(c->value); free(c->initial); c->value = c->initial = NULL; continue; }
        c->single = c->kind == CONTROL_TEXT || c->kind == CONTROL_PASSWORD || (c->kind == CONTROL_TEXTAREA && atoi(dom_attr(n, "rows")) == 1);
        if (c->single && c->form >= 0 && !c->disabled) v->forms[c->form].fields++;
        b->control_of[i] = v->control_count++;
    }
    free(form_of);
}

static int find_id(const browser_dom *dom, const char *id)
{
    if (!*id) return -1;
    for (int i = 0; i < dom->count; i++) if (!strcmp(dom_attr(&dom->nodes[i], "id"), id)) return i;
    return -1;
}
static int first_control(builder *b, int node, int depth)
{
    if (depth > 16) return -1;
    if (b->control_of[node] >= 0) return b->control_of[node];
    for (int c = b->dom->nodes[node].first; c >= 0; c = b->dom->nodes[c].next) {
        int found = first_control(b, c, depth + 1);
        if (found >= 0) return found;
    }
    return -1;
}

/* The size of a form field (0 when it isn't shown); percentages are of
   `room`, the line's width, or ignored when it's 0. */
static int field_size(builder *b, int node, const browser_box *box, int room, int *height)
{
    browser_view *v = b->v;
    int index = b->control_of[node];
    if (index < 0) return 0;
    view_control *c = &v->controls[index];
    const dom_node *n = &b->dom->nodes[node];
    int w, h = FIELD_H;
    float s = 0.6f;
    if (c->kind == CONTROL_BUTTON && !*view_string(v, c->label)) return 0;
    switch (c->kind) {
    case CONTROL_HIDDEN: return 0;
    case CONTROL_CHECKBOX: case CONTROL_RADIO: w = h = 11; break;
    case CONTROL_TEXTAREA: {
        int cols = atoi(dom_attr(n, "cols")), rows = atoi(dom_attr(n, "rows"));
        w = (cols > 0 ? cols : 30) * 6;
        h = rows == 1 ? FIELD_H : (rows > 0 ? clampi(rows, 1, 12) : 3) * 11 + 6;
        break;
    }
    case CONTROL_SELECT: {
        float widest = 30;
        for (int i = 0; i < c->option_count; i++) {
            const char *label = view_string(v, v->options[c->option_first + i].label);
            float lw = width_of(b, label, (int)strlen(label), s, 0);
            if (lw > widest) widest = lw;
        }
        w = (int)widest + 22;
        break;
    }
    case CONTROL_SUBMIT: case CONTROL_RESET: case CONTROL_BUTTON: case CONTROL_IMAGE: case CONTROL_FILE: {
        const char *label = view_string(v, c->label);
        w = (int)width_of(b, label, (int)strlen(label), s, 0) + 14;
        break;
    }
    default: {
        int size = atoi(dom_attr(n, "size"));
        w = (size > 0 ? size : 22) * 6;
        break;
    }
    }
    int label_width = w;
    if (box->width > 0) w = box->width;
    else if (box->width < 0 && room > 0) w = room * -box->width / 100;
    /* A button sized by CSS for an icon still shows its whole name. */
    if ((c->kind == CONTROL_SUBMIT || c->kind == CONTROL_RESET || c->kind == CONTROL_BUTTON || c->kind == CONTROL_IMAGE) && w < label_width) w = label_width;
    if (box->height > 0 && c->kind != CONTROL_CHECKBOX && c->kind != CONTROL_RADIO) h = clampi(box->height, 11, 200);
    *height = h;
    return w > 11 ? w : 11;
}

static void field(builder *b, int node, browser_style st, browser_box *box)
{
    browser_view *v = b->v;
    line_state *l = b->line;
    int h, w = field_size(b, node, box, l->width, &h), index = b->control_of[node];
    if (w <= 0) return;
    float s = 0.6f;
    w = clampi(w, 11, l->width > 11 ? l->width : 11);
    view_control *c = &v->controls[index];
    c->dismiss = b->consent;
    int plain = (c->kind == CONTROL_SUBMIT || c->kind == CONTROL_RESET || c->kind == CONTROL_BUTTON) &&
                ((box->plain & 1) || ((box->plain & 2) && (box->plain & 4)));
    if (plain) {
        /* A button styled as text (code tokens, tabs, menus): sized and drawn as its text. */
        const char *label = view_string(v, c->label);
        if (is(&b->dom->nodes[node], "button") && !has_content(b->dom, node, 0)) st = name_style(st);
        s = st.scale;
        w = clampi((int)width_of(b, label, (int)strlen(label), s, st.flags) + 2, 4, l->width > 4 ? l->width : 4);
        h = (int)(LINE * s + 0.5f);
    }
    int first = v->item_count;
    view_item it = item(ITEM_CONTROL, 0, 0, w, h, st.color);
    it.control = (short)index; it.link = (short)b->link; it.scale = s;
    if (plain) { it.flags = CONTROL_PLAIN | (short)(st.flags & CSS_BOLD ? 2 : 0); it.color = readable(b, st.color, 0, b->link); }
    int at = add_item(b, it);
    if (at < 0) return;
    c->item = at;
    add_box(b, first, w, h, h - 3 > 0 ? (float)(h - 3) : (float)h);
}

/* Whether an <img> shows a picture: the page's pictures are loaded, and it
   has a source this decodes. Its own size in page pixels goes to *nw x *nh
   once known (0 before). */
static int image_picture(builder *b, int node, int *nw, int *nh)
{
    picture_table *t = b->doc->pictures;
    size_t n;
    float density;
    *nw = *nh = 0;
    int key = b->dom->nodes[node].source_id;
    if (!t || (picture_of(t, key) < 0 && (t->count >= PICTURES_MAX || !picture_source(b->dom, node, VIEW_WIDTH, &n, &density)))) return 0;
    picture_known(t, key, nw, nh);
    return 1;
}

/* The size of an image's box (0 when it isn't shown), fitted into `room`. */
static int image_size(builder *b, int node, const browser_box *box, int room, int *height)
{
    const dom_node *n = &b->dom->nodes[node];
    const char *alt = dom_attr(n, "alt");
    int w = box->width > 0 ? box->width : box->width < 0 && room > 0 ? room * -box->width / 100 : 0;
    int h = box->height > 0 ? box->height : 0;
    int nw, nh;
    if (image_picture(b, node, &nw, &nh)) {
        /* The picture's box: the page's size, or the picture's own, keeping
           its shape. Until its size is known, a description holds its place. */
        if (nw && nw <= 2 && nh <= 2) return 0;     /* tracking pixel */
        if (!w && !h) {
            if (nw) { w = nw; h = nh; }
            else if (*alt) { w = (int)width_of(b, alt, (int)strlen(alt), 0.55f, 0) + 8; h = FIELD_H; }
            else return 0;
        } else if (!h) h = nw ? (int)((long long)w * nh / nw) : w * 3 / 4;
        else if (!w) w = nw ? (int)((long long)h * nw / nh) : h;
        if (w <= 2 && h <= 2) return 0;
        int most = box->max_width > 0 ? box->max_width : box->max_width < 0 && room > 0 ? room * -box->max_width / 100 : 0;
        if (most > 0 && w > most) { h = (int)((long long)h * most / w); w = most; }
        if (room > 0 && w > room) { h = (int)((long long)h * room / w); w = room; }
        if (h > 600) { w = (int)((long long)w * 600 / h); h = 600; }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        *height = h;
        return w;
    }
    /* Without the picture, decoration is noise: alt="" marks it, and an
       icon (up to 25 CSS pixels) without a description is one. */
    if (!*alt && (has_attr(n, "alt") || (w <= 15 && h <= 15))) return 0;
    if (!w && !h) {
        w = (int)width_of(b, alt, (int)strlen(alt), 0.55f, 0) + 8;
        h = FIELD_H;
    }
    if (w <= 2 && h <= 2) return 0;     /* tracking pixel */
    if (!h) h = w * 3 / 4;
    if (!w) w = h;
    if (room > 0 && w > room) { h = h * room / w; w = room; }
    if (h > 360) h = 360;
    if (w < 4 || h < 4) return 0;
    *height = h;
    return w;
}

/* The address of the picture an <img> shows in a box `width` page pixels
   wide, as browsers send it (malloc'd), or NULL. */
static char *picture_url(builder *b, int node, int width, float *density)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n;
    const char *src = picture_source(b->dom, node, width, &n, density);
    if (!src) return NULL;
    if (n >= 5 && !strncasecmp(src, "data:", 5)) return strndup(src, n);
    char raw[BROWSER_URL_MAX], absolute[BROWSER_URL_MAX];
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t' || c == '\n' || c == '\r') continue;
        int escape = c <= 32 || c >= 127 || c == '"' || c == '<' || c == '>' ||
                     (c == '%' && !(i + 2 < n && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])));
        if (used + 4 > sizeof(raw)) return NULL;
        if (c == '\\') raw[used++] = '/';
        else if (escape) { raw[used++] = '%'; raw[used++] = hex[c >> 4]; raw[used++] = hex[c & 15]; }
        else raw[used++] = (char)c;
    }
    raw[used] = 0;
    if (browser_url_resolve(b->base, raw, absolute, sizeof(absolute)) < 0) return NULL;
    return strdup(absolute);
}

/* Puts the picture an <img> shows in a w x h box (0 x 0 while its size is
   unknown) in the page's table, for the loader: its entry, or -1. */
static int add_picture(builder *b, int node, const browser_box *box, int w, int h)
{
    picture_table *t = b->doc->pictures;
    int key = b->dom->nodes[node].source_id;
    int nw, nh, given_w = box->width != 0, given_h = box->height > 0, known = picture_of(t, key);
    if (!t || (w <= 0 && (known >= 0 || given_w || given_h)) || !image_picture(b, node, &nw, &nh)) return -1;
    int fit = given_w && given_h ? PICTURE_FIT_STRETCH : given_w || given_h ? PICTURE_FIT_BOX : PICTURE_FIT_OWN;
    /* the size it's decoded at: its box, or for a picture shown at its own
       size, at most as wide as the line */
    int want_w = fit == PICTURE_FIT_OWN ? b->line->width : given_w ? w : 0, want_h = given_h ? h : 0;
    if (known >= 0) {
        picture_add(t, key, t->entries[known].url, want_w, want_h, fit, *b->y, t->entries[known].density);
        if (b->naming) t->entries[known].relayout = 1;
        return known;
    }
    float density;
    char *url = picture_url(b, node, want_w > 0 ? want_w : b->line->width, &density);
    if (!url) return -1;
    int index = picture_add(t, key, url, want_w, want_h, fit, *b->y, density);
    free(url);
    /* an icon link shows its name until its picture arrives */
    if (index >= 0 && b->naming) t->entries[index].relayout = 1;
    return index;
}

static void image(builder *b, int node, browser_style st, browser_box *box)
{
    const dom_node *n = &b->dom->nodes[node];
    const char *alt = dom_attr(n, "alt");
    int h = 0, w = image_size(b, node, box, b->line->width, &h), picture = add_picture(b, node, box, w, h);
    if (w <= 0) return;
    int first = b->v->item_count;
    view_item it = item(ITEM_IMAGE, 0, 0, w, h, st.color);
    it.link = (short)b->link; it.control = (short)b->label;
    it.flags = (short)(picture + 1);
    if (*alt) { it.text = store(b, alt, strlen(alt), 0); it.length = it.text >= 0 ? (int)strlen(alt) : 0; }
    if (add_item(b, it) < 0) return;
    b->v->images++;
    add_box(b, first, w, h, (float)h);
}

/* ---- blocks ---- */

static void element(builder *b, int node, browser_style parent);
static void children(builder *b, int node, browser_style st)
{
    if (b->depth >= DOM_DEPTH_MAX) return;
    b->depth++;
    for (int c = b->dom->nodes[node].first; c >= 0 && !b->failed; c = b->dom->nodes[c].next) element(b, c, st);
    b->depth--;
}

static int skipped(builder *b, const dom_node *n)
{
    static const char *tags[] = {"head", "script", "style", "template", "title", "link", "meta", "base", "svg", "math",
                                 "datalist", "param", "source", "track", "area", "map"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++) if (is(n, tags[i])) return 1;
    if (is(n, "dialog") && !has_attr(n, "open")) return 1;
    /* notices the user closed */
    for (int i = 0, count = b->doc->hidden_count; i < count && i < BROWSER_HIDDEN_MAX; i++) if (b->doc->hidden[i] == n->source_id) return 1;
    /* With JavaScript on, <noscript> is skipped like in other browsers;
       its styles often hide the whole page. */
    return b->scripting && is(n, "noscript");
}

static int block_level(const dom_node *n, browser_style st, const browser_box *box)
{
    if (box->display == DISPLAY_INLINE || box->display == DISPLAY_INLINE_BLOCK) return 0;
    if (box->display != DISPLAY_DEFAULT) return 1;
    return st.block && !is(n, "br");
}

static void default_box(const dom_node *n, browser_box *box)
{
    int m[4] = {0, 0, 0, 0}, p[4] = {0, 0, 0, 0};
    if (is(n, "p") || is(n, "dl") || is(n, "pre") || is(n, "fieldset") || is(n, "listing")) m[0] = m[2] = 7;
    else if (is(n, "h1")) m[0] = m[2] = 9;
    else if (is(n, "h2") || is(n, "h3")) m[0] = m[2] = 8;
    else if (is(n, "h4") || is(n, "h5") || is(n, "h6")) m[0] = m[2] = 7;
    else if (is(n, "ul") || is(n, "ol") || is(n, "menu") || is(n, "dir")) { m[0] = m[2] = 5; p[3] = 18; }
    else if (is(n, "blockquote") || is(n, "figure")) { m[0] = m[2] = 7; m[1] = m[3] = 18; }
    else if (is(n, "dd")) m[3] = 18;
    else if (is(n, "hr")) m[0] = m[2] = 5;
    if (is(n, "fieldset")) { p[0] = p[2] = 4; p[1] = p[3] = 6; }
    for (int i = 0; i < 4; i++) {
        if (!(box->set & (1 << i))) box->margin[i] = (short)m[i];
        if (!(box->set & (16 << i))) box->padding[i] = (short)p[i];
    }
    if (is(n, "fieldset") && !box->border[0]) for (int i = 0; i < 4; i++) { box->border[i] = 1; box->border_color[i] = 0xffc0c0c0; }
}

static void table(builder *b, int node, browser_style st, int x, int width);
static void flex(builder *b, int node, browser_style st, const browser_box *box, int x, int width);
static void grid(builder *b, int node, browser_style st, const browser_box *box, int x, int width);
static void float_box(builder *b, int node, browser_style parent, browser_style st, browser_box box);

static void block(builder *b, int node, browser_style st, browser_box *box, int cx, int cwidth)
{
    const dom_node *n = &b->dom->nodes[node];
    browser_view *v = b->v;
    int *y = b->y;
    default_box(n, box);
    int mt = box->margin[0] == BOX_AUTO ? 0 : box->margin[0], mb = box->margin[2] == BOX_AUTO ? 0 : box->margin[2];
    int ml = box->margin[3] == BOX_AUTO ? 0 : box->margin[3], mr = box->margin[1] == BOX_AUTO ? 0 : box->margin[1];
    if (mt < 0) mt = 0;
    if (mb < 0) mb = 0;
    if (box->clear) {
        /* below the floats on that side */
        for (int i = 0; i < b->float_count; i++)
            if ((box->clear & b->floats[i].side) && b->floats[i].y + b->floats[i].h > *y) { *y = b->floats[i].y + b->floats[i].h; b->collapse = 0; }
    }
    int floats = b->float_count;
    if (mt > b->collapse) *y += mt - b->collapse;
    b->collapse = mt > b->collapse ? mt : b->collapse;
    int avail = cwidth - ml - mr, w = avail;
    if (box->width > 0 && box->width < w) w = box->width;
    else if (box->width < 0) w = avail * -box->width / 100;
    if (box->max_width > 0 && box->max_width < w) w = box->max_width;
    else if (box->max_width < 0 && avail * -box->max_width / 100 < w) w = avail * -box->max_width / 100;
    if (w < 16) w = avail < 16 ? avail : 16;
    int x = cx + ml;
    if (w < avail && box->margin[3] == BOX_AUTO && box->margin[1] == BOX_AUTO) x += (avail - w) / 2;
    else if (w < avail && box->margin[3] == BOX_AUTO) x += avail - w;
    else if (w < avail && (is(n, "table") || box->display == DISPLAY_INLINE_BLOCK) && st.align == 1) x += (avail - w) / 2;
    else if (w < avail && box->display == DISPLAY_INLINE_BLOCK && st.align == 2) x += avail - w;
    if (is(n, "hr")) {
        uint32_t c = box->border_color[0] ? box->border_color[0] : 0xffb0b0b0;
        int t = box->height > 0 ? clampi(box->height, 1, 8) : box->border[0] > 1 ? box->border[0] : 1;
        add_item(b, item(ITEM_RECT, x, *y, w, t, st.background ? st.background : c));
        *y += t + mb;
        b->collapse = mb;
        return;
    }
    int top = *y, background = -1;
    uint32_t backdrop = b->backdrop;
    if (st.background) { background = add_item(b, item(ITEM_RECT, x, top, w, 0, st.background)); b->backdrop = st.background; }
    int bt = box->border[0], bb = box->border[2], bl = box->border[3], br = box->border[1];
    int pt = box->padding[0], pb = box->padding[2], pl = box->padding[3], pr = box->padding[1];
    if (bt + pt > 0) { *y += bt + pt; b->collapse = 0; }
    int inner_x = x + bl + pl, inner_w = w - bl - br - pl - pr;
    if (inner_w < 8) inner_w = 8;
    int content_top = *y;

    /* The content's own line context. */
    /* justify-content aligns a flex container's inline content */
    int align = box->justify == 2 || box->justify == 5 || box->justify == 6 ? 1 : box->justify == 3 ? 2 : box->justify ? 0 : st.align;
    line_state line = { .x = inner_x, .width = inner_w, .align = align, .first = b->piece_count };
    st.background = 0;      /* drawn above: inline content doesn't repeat it */
    line_state *outer = b->line;
    b->line = &line;
    int list_kind = b->list_kind, *list_number = b->list_number, number = 1;
    if (is(n, "ul") || is(n, "ol") || is(n, "menu") || is(n, "dir")) {
        b->list_kind = is(n, "ol") ? 2 : 1;
        if (is(n, "ol") && *dom_attr(n, "start")) number = atoi(dom_attr(n, "start"));
        b->list_number = &number;
    }
    if ((is(n, "li") || box->display == DISPLAY_LIST_ITEM) && !st.list_none && b->list_kind) {
        b->marker = b->list_kind;
        b->marker_number = b->list_number ? (*b->list_number)++ : 1;
        b->marker_color = st.color; b->marker_scale = st.scale;
    }
    if (b->name) {
        /* An icon's box is sized for the icon: its name may run past it
           rather than break up, while it fits on the screen. */
        browser_style ns = name_style(st);
        int need = (int)width_of(b, b->name, (int)strlen(b->name), ns.scale, ns.flags) + 1;
        if (need > line.width && inner_x + need <= v->width - 4) line.width = need;
        add_text(b, b->name, ns);
        b->name = NULL;
    }
    if (is(n, "table") || box->display == DISPLAY_TABLE) table(b, node, st, inner_x, inner_w);
    else if (box->inner == INNER_FLEX) flex(b, node, st, box, inner_x, inner_w);
    else if (box->inner == INNER_GRID) grid(b, node, st, box, inner_x, inner_w);
    else children(b, node, st);
    flush_line(b);
    if (b->marker && is(n, "li")) { place_marker(b, inner_x, *y, ascent(st.scale)); *y += (int)(LINE * st.scale); }
    /* The block holds the floats in it (as with a clearfix), and they
       don't reach past it. */
    for (int i = floats; i < b->float_count; i++) if (b->floats[i].y + b->floats[i].h > *y) *y = b->floats[i].y + b->floats[i].h;
    if (b->float_count > floats) { b->float_count = floats; b->collapse = 0; }
    b->backdrop = backdrop;
    b->line = outer;
    b->list_kind = list_kind; b->list_number = list_number;
    if (box->height > 0 && *y - content_top < box->height) *y = content_top + box->height;
    if (bb + pb > 0) { *y += bb + pb; b->collapse = 0; }
    int bottom = *y;
    if (background >= 0) v->items[background].h = bottom - top;
    uint32_t c;
    int border[4] = {-1, -1, -1, -1};
    if (bt) { c = box->border_color[0] ? box->border_color[0] : st.color; border[0] = add_item(b, item(ITEM_RECT, x, top, w, bt, c)); }
    if (bb) { c = box->border_color[2] ? box->border_color[2] : st.color; border[2] = add_item(b, item(ITEM_RECT, x, bottom - bb, w, bb, c)); }
    if (bl) { c = box->border_color[3] ? box->border_color[3] : st.color; border[3] = add_item(b, item(ITEM_RECT, x, top, bl, bottom - top, c)); }
    if (br) { c = box->border_color[1] ? box->border_color[1] : st.color; border[1] = add_item(b, item(ITEM_RECT, x + w - br, top, br, bottom - top, c)); }
    b->last.node = node; b->last.background = background;
    memcpy(b->last.border, border, sizeof(border));
    *y += mb;
    b->collapse = mb;
}

/* An inline-block holding blocks (a menu, a card) is laid out as a block as
   wide as its content: inline, its boxes would break up. */
static int holds_blocks(builder *b, int node, browser_style st)
{
    for (int c = b->dom->nodes[node].first; c >= 0; c = b->dom->nodes[c].next) {
        const dom_node *k = &b->dom->nodes[c];
        if (k->tag[0] == '#' || skipped(b, k)) continue;
        browser_box box;
        browser_style ks = css_compute_box(&b->doc->css, b->dom, c, st, &box);
        if (!ks.hidden && !box.hide && box.display != DISPLAY_NONE && block_level(k, ks, &box)) return 1;
    }
    return 0;
}

static void content_widths(builder *b, int node, browser_style st, float *minimum, float *maximum, float *line, int depth);
static void inner_widths(builder *b, int node, browser_style st, const browser_box *box, float *minimum, float *maximum, int depth);
static int shrink_width(builder *b, int node, browser_style st, browser_box *box)
{
    float lo = 0, hi = 0;
    inner_widths(b, node, st, box, &lo, &hi, 1);
    default_box(&b->dom->nodes[node], box);
    int w = (int)hi + (hi > (int)hi ? 1 : 0) + box->padding[1] + box->padding[3] + box->border[1] + box->border[3];
    return w > 16 ? w : 16;
}

static void anchor(builder *b, const dom_node *n)
{
    const char *id = dom_attr(n, "id");
    if (!*id && is(n, "a")) id = dom_attr(n, "name");
    browser_view *v = b->v;
    if (!*id || v->anchor_count >= VIEW_ANCHORS_MAX || strlen(id) > 95) return;
    int at = store_string(b, id);
    if (at < 0) return;
    v->anchors[v->anchor_count].id = at;
    v->anchors[v->anchor_count].y = *b->y;
    v->anchor_count++;
}

static int replaced(const dom_node *n)
{
    static const char *tags[] = {"img", "input", "select", "textarea", "button", "iframe", "video", "audio", "embed", "object", "canvas"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++) if (is(n, tags[i])) return 1;
    return 0;
}

static void element_styled(builder *b, int node, browser_style parent, browser_style st, browser_box box);
static void element(builder *b, int node, browser_style parent)
{
    const browser_dom *dom = b->dom;
    const dom_node *n = &dom->nodes[node];
    if (b->failed) return;
    if (!strcmp(n->tag, "#text")) {
        if (!parent.hidden) add_text(b, n->text, parent);
        return;
    }
    if (n->tag[0] == '#' && strcmp(n->tag, "#document")) return;
    if (skipped(b, n)) return;
    browser_box box;
    browser_style st = css_compute_box(&b->doc->css, dom, node, parent, &box);
    if (st.hidden || box.hide || box.display == DISPLAY_NONE) return;
    element_styled(b, node, parent, st, box);
}

/* ---- cookie consent notices ---- */

/* The names sites and consent tools give a notice asking for consent to
   cookies: its id, a class, its label. */
static int consent_name(const char *name, size_t length)
{
    static const char *tools[] = {"consent", "gdpr", "onetrust", "didomi", "qc-cmp", "truste", "usercentrics", "cookiebot",
                                  "tarteaucitron", "iubenda", "cmplz", "borlabs", "cookieyes", "cc-window", "cc-banner",
                                  "sp-message", "fc-consent", "cookielaw", "cookie-law"};
    static const char *notices[] = {"banner", "bar", "notice", "notif", "popup", "pop-up", "modal", "message", "alert", "wall",
                                    "layer", "overlay", "dialog", "warning", "policy", "law", "hint", "disclaimer", "prompt", "accept"};
    char s[128];
    if (length >= sizeof(s)) length = sizeof(s) - 1;
    for (size_t i = 0; i < length; i++) s[i] = name[i] == '_' ? '-' : (char)tolower((unsigned char)name[i]);
    s[length] = 0;
    for (size_t i = 0; i < sizeof(tools) / sizeof(*tools); i++) if (strstr(s, tools[i])) return 1;
    int cookie = strstr(s, "cookie") != NULL, privacy = strstr(s, "privacy") != NULL;
    if (cookie || privacy)
        for (size_t i = 0; i < sizeof(notices) / sizeof(*notices); i++)
            if (strstr(s, notices[i]) && (cookie || (strcmp(notices[i], "policy") && strcmp(notices[i], "law")))) return 1;
    return 0;
}
/* Whether the text of an element (its first few hundred nodes) speaks of
   cookies or consent: in most languages, the word "cookie" itself. */
static int mentions_cookies(const browser_dom *dom, int node)
{
    int stack[64], top = 0, seen = 0;
    stack[top++] = node;
    while (top && seen++ < 300) {
        const dom_node *n = &dom->nodes[stack[--top]];
        if (n->text && !strcmp(n->tag, "#text"))
            for (const char *p = n->text; *p; p++)
                if (!strncasecmp(p, "cookie", 6) || !strncasecmp(p, "consent", 7)) return 1;
        for (int c = n->first; c >= 0 && top < 64; c = dom->nodes[c].next) stack[top++] = c;
    }
    return 0;
}
/* A notice asking for consent to cookies: named so, or a dialog about them. */
static int consent_box(const browser_dom *dom, int node)
{
    const dom_node *n = &dom->nodes[node];
    if (n->tag[0] == '#' || is(n, "html") || is(n, "body") || is(n, "main")) return 0;
    const char *id = dom_attr(n, "id"), *label = dom_attr(n, "aria-label"), *role = dom_attr(n, "role");
    if ((*id && consent_name(id, strlen(id))) || (*label && consent_name(label, strlen(label)))) return 1;
    for (const char *c = dom_attr(n, "class"); *c;) {
        while (isspace((unsigned char)*c)) c++;
        const char *end = c;
        while (*end && !isspace((unsigned char)*end)) end++;
        if (end > c && consent_name(c, (size_t)(end - c))) return 1;
        c = end;
    }
    if (is(n, "dialog") || eq(role, "dialog") || eq(role, "alertdialog") || eq(dom_attr(n, "aria-modal"), "true"))
        return mentions_cookies(dom, node);
    return 0;
}
/* Whether an element of a notice looks like one of its buttons. */
static int button_like(const dom_node *n)
{
    const char *role = dom_attr(n, "role"), *cls = dom_attr(n, "class");
    if (is(n, "a") || eq(role, "button") || eq(role, "link") || eq(role, "menuitem") || eq(role, "switch") ||
        has_attr(n, "tabindex") || has_attr(n, "onclick")) return 1;
    for (const char *p = cls; *p; p++)
        if (!strncasecmp(p, "btn", 3) || !strncasecmp(p, "button", 6)) return 1;
    return 0;
}
enum { CLICK_LINK = 1, CLICK_SCRIPT = 2, CLICK_DISMISS = 4 };
/* What a click on an element does: follow its link, run the page's scripts
   (data-flow-click), or close the consent notice it's a button of when
   nothing else happens. */
static int clickable(builder *b, int node)
{
    const dom_node *n = &b->dom->nodes[node];
    const char *href = dom_attr(n, "href");
    int what = 0;
    if (is(n, "a") && *href && !(b->consent >= 0 && (*href == '#' || !strncasecmp(href, "javascript:", 11)))) what |= CLICK_LINK;
    if (has_attr(n, "data-flow-click") && !is(n, "form") && !is(n, "body") && !is(n, "html")) what |= CLICK_SCRIPT;
    if (b->consent >= 0 && !(what & CLICK_LINK) && button_like(n)) what |= CLICK_DISMISS;
    return what;
}

static void element_box(builder *b, int node, browser_style parent, browser_style st, browser_box box);
/* An element whose style the caller computed. */
static void element_styled(builder *b, int node, browser_style parent, browser_style st, browser_box box)
{
    int consent = b->consent;
    if (consent < 0 && consent_box(b->dom, node)) b->consent = b->dom->nodes[node].source_id;
    element_box(b, node, parent, st, box);
    b->consent = consent;
}
static void element_box(builder *b, int node, browser_style parent, browser_style st, browser_box box)
{
    const browser_dom *dom = b->dom;
    const dom_node *n = &dom->nodes[node];
    /* A flex or grid item is a block of the width its container gave it. */
    int item = b->item_width >= 0;
    if (item) { box.width = (short)(b->item_width > 16 ? b->item_width : 16); box.max_width = 0; b->item_width = -1; }
    if (is(n, "body") || is(n, "html")) {
        if (st.background) b->v->paper = st.background;
        if (st.color != parent.color) b->v->ink = st.color;
        st.background = 0;   /* drawn as the page's paper */
    }
    anchor(b, n);
    if (is(n, "br")) { line_break(b, st.scale); return; }
    if (is(n, "input") || is(n, "select") || is(n, "textarea") || is(n, "button")) { field(b, node, st, &box); return; }
    if (is(n, "img")) {
        if (box.float_side && !item) float_box(b, node, parent, st, box);
        else image(b, node, st, &box);
        return;
    }
    if (is(n, "iframe") || is(n, "video") || is(n, "audio") || is(n, "embed") || is(n, "object") || is(n, "canvas")) {
        if (is(n, "canvas")) return;
        browser_style label = st;
        label.color = 0xff808080;
        label.flags |= CSS_ITALIC;
        char text[64];
        snprintf(text, sizeof(text), "[%s]", is(n, "iframe") ? "frame" : n->tag);
        add_text(b, text, label);
        return;
    }
    int link = b->link, label = b->label, what = clickable(b, node);
    char icon[96] = "";
    if (what && b->v->link_count < VIEW_LINKS_MAX) {
        view_link *l = &b->v->links[b->v->link_count];
        l->url = -1;
        l->node = what & CLICK_SCRIPT ? n->source_id : -1;
        l->dismiss = b->consent;
        if (what & CLICK_LINK) {
            char target[BROWSER_URL_MAX];
            if (browser_url_resolve(b->base, dom_attr(n, "href"), target, sizeof(target)) == 0) l->url = store_string(b, target);
        }
        if (l->url >= 0 || l->node >= 0 || (what & CLICK_DISMISS)) b->link = b->v->link_count++;
        /* An icon link (a logo, GitHub, Menu) shows its name, unless its picture shows. */
        if (!has_content(dom, node, 0) && !has_picture(b, node, 0)) accessible_name(dom, node, icon, sizeof(icon));
    }
    if (is(n, "label")) {
        int target = find_id(dom, dom_attr(n, "for"));
        int c = target >= 0 ? b->control_of[target] : first_control(b, node, 0);
        if (c >= 0) b->label = c;
    }
    int inline_block = box.display == DISPLAY_INLINE_BLOCK && holds_blocks(b, node, st);
    b->naming += *icon != 0;
    if (box.float_side && !item && !is(n, "body") && !is(n, "html")) {
        if (*icon) b->name = icon;
        float_box(b, node, parent, st, box);
        b->name = NULL;
    } else if (block_level(n, st, &box) || inline_block || item) {
        flush_line(b);
        int x = b->line->x, width = b->line->width;
        if (inline_block && !box.width && !item) box.width = (short)shrink_width(b, node, st, &box);
        if (*icon) b->name = icon;
        block(b, node, st, &box, x, width);
        b->name = NULL;
    } else {
        /* An inline element: its margins and padding take room in the line,
           its background goes behind its text, and an inline parent's
           background shows through. */
        int pad = b->pad;
        if (!st.background) st.background = parent.background;
        else b->pad = clampi(box.padding[0] > box.padding[2] ? box.padding[0] : box.padding[2], 0, 4);
        int ml = box.margin[3] == BOX_AUTO ? 0 : clampi(box.margin[3], 0, 30), mr = box.margin[1] == BOX_AUTO ? 0 : clampi(box.margin[1], 0, 30);
        int pl = clampi(box.padding[3] + box.border[3], 0, 30), pr = clampi(box.padding[1] + box.border[1], 0, 30);
        add_gap(b, ml, parent.background, st);
        add_gap(b, pl, st.background, st);
        if (*icon) add_text(b, icon, name_style(st));
        children(b, node, st);
        add_gap(b, pr, st.background, st);
        add_gap(b, mr, parent.background, st);
        b->pad = pad;
    }
    b->naming -= *icon != 0;
    b->link = link; b->label = label;
}

/* ---- tables ---- */

#define COLUMNS_MAX 16
typedef struct { int node, column, span; } cell;

/* The width of the name an icon link shows in place of its content (see
   element()), or -1 when the element isn't one. */
static float icon_width(builder *b, int node, browser_style st)
{
    if (!clickable(b, node) || has_content(b->dom, node, 0) || has_picture(b, node, 0)) return -1;
    char name[96] = "";
    accessible_name(b->dom, node, name, sizeof(name));
    browser_style ns = name_style(st);
    return width_of(b, name, (int)strlen(name), ns.scale, ns.flags) + 1;
}

/* Narrowest (longest word) and widest (everything on one line) widths of a cell's content. */
static void content_widths(builder *b, int node, browser_style st, float *minimum, float *maximum, float *line, int depth)
{
    const dom_node *n = &b->dom->nodes[node];
    if (depth > 24) return;
    if (!strcmp(n->tag, "#text")) {
        const char *p = n->text ? n->text : "";
        char copy[512];
        while (*p) {
            if (isspace((unsigned char)*p)) { while (isspace((unsigned char)*p)) p++; *line += width_of(b, " ", 1, st.scale, st.flags); continue; }
            const char *s = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            int length = (int)(p - s) < (int)sizeof(copy) ? (int)(p - s) : (int)sizeof(copy) - 1;
            memcpy(copy, s, (size_t)length);
            transform(copy, length, st.flags);
            float w = width_of(b, copy, length, st.scale, st.flags);
            if (w > *minimum) *minimum = w;
            *line += w;
        }
        if (*line > *maximum) *maximum = *line;
        return;
    }
    if (n->tag[0] == '#' || skipped(b, n)) return;
    /* A flex or grid container measures its items with a budget: past it,
       an item is taken as wide as there's room. */
    if (b->budget > 0 && --b->budget == 0) b->budget = -1;
    if (b->budget < 0) return;
    browser_box box;
    browser_style cs = css_compute_box(&b->doc->css, b->dom, node, st, &box);
    if (cs.hidden || box.hide || box.display == DISPLAY_NONE) return;
    if (is(n, "br")) { *line = 0; return; }
    float w = icon_width(b, node, cs);
    if (w >= 0) {
        if (w > *minimum) *minimum = w;
        *line += w;
        if (*line > *maximum) *maximum = *line;
        if (block_level(n, cs, &box)) *line = 0;
        return;
    }
    int h, fixed = -1, compressible = 0;
    if (is(n, "img")) { fixed = image_size(b, node, &box, 0, &h); compressible = box.width < 0 || box.max_width < 0; }
    else if (is(n, "input") || is(n, "select") || is(n, "button") || is(n, "textarea")) fixed = field_size(b, node, &box, 0, &h);
    if (fixed >= 0) {
        /* a picture sized by a percentage shrinks with its container */
        if (fixed > *minimum && !compressible) *minimum = (float)fixed;
        *line += (float)fixed;
        if (*line > *maximum) *maximum = *line;
        return;
    }
    if (block_level(n, cs, &box) || (box.display == DISPLAY_INLINE_BLOCK && holds_blocks(b, node, cs))) {
        /* a block: its lines, plus its margins, borders and padding */
        default_box(n, &box);
        float lo = 0, hi = 0;
        inner_widths(b, node, cs, &box, &lo, &hi, depth + 1);
        int margins = (box.margin[1] == BOX_AUTO ? 0 : clampi(box.margin[1], 0, 200)) + (box.margin[3] == BOX_AUTO ? 0 : clampi(box.margin[3], 0, 200));
        int sides = box.padding[1] + box.padding[3] + box.border[1] + box.border[3];
        if (box.width > 0) lo = hi = (float)(box.width + margins);
        else {
            lo += margins + sides; hi += margins + sides;
            if (box.max_width > 0 && hi > box.max_width + margins) hi = (float)(box.max_width + margins);
        }
        if (lo > *minimum) *minimum = lo;
        if (box.float_side) {
            /* floats sit side by side */
            *line += hi;
            if (*line > *maximum) *maximum = *line;
            return;
        }
        if (hi > *maximum) *maximum = hi;
        *line = 0;
        return;
    }
    /* an inline element's margins and padding, as element() adds them */
    int before = (box.margin[3] == BOX_AUTO ? 0 : clampi(box.margin[3], 0, 30)) + clampi(box.padding[3] + box.border[3], 0, 30);
    int after = (box.margin[1] == BOX_AUTO ? 0 : clampi(box.margin[1], 0, 30)) + clampi(box.padding[1] + box.border[1], 0, 30);
    *line += before;
    for (int c = n->first; c >= 0; c = b->dom->nodes[c].next) content_widths(b, c, cs, minimum, maximum, line, depth + 1);
    *line += after;
    if (*line > *maximum) *maximum = *line;
}

/* The narrowest and widest widths of an element's content, laid out as its
   display says: a flex row's items side by side, a grid's in its columns. */
static void measure_inner(builder *b, int node, browser_style st, const browser_box *box, float *minimum, float *maximum, int depth);
static void inner_widths(builder *b, int node, browser_style st, const browser_box *box, float *minimum, float *maximum, int depth)
{
    /* Nested flex containers measure the same content again: once is enough. */
    float lo = 0, hi = 0;
    if (b->known && b->known[node]) { lo = b->measured[node].minimum; hi = b->measured[node].maximum; }
    else {
        int partial = b->budget < 0;
        measure_inner(b, node, st, box, &lo, &hi, depth);
        if (b->known && !partial && b->budget >= 0) { b->measured[node].minimum = lo; b->measured[node].maximum = hi; b->known[node] = 1; }
    }
    if (lo > *minimum) *minimum = lo;
    if (hi > *maximum) *maximum = hi;
}
static void measure_inner(builder *b, int node, browser_style st, const browser_box *box, float *minimum, float *maximum, int depth)
{
    const browser_dom *dom = b->dom;
    float line = 0;
    if (box->inner == INNER_FLOW || depth > 24) {
        for (int c = dom->nodes[node].first; c >= 0; c = dom->nodes[c].next) content_widths(b, c, st, minimum, maximum, &line, depth);
        return;
    }
    int columns = box->inner == INNER_GRID ? (box->tracks ? box->tracks : box->fill ? 2 : 1) : box->direction >= 2 ? 1 : 0;
    float sum = 0, widest = 0;
    int items = 0;
    for (int c = dom->nodes[node].first; c >= 0; c = dom->nodes[c].next) {
        float lo = 0, hi = 0;
        line = 0;
        content_widths(b, c, st, &lo, &hi, &line, depth);
        if (hi <= 0) continue;
        if (lo > *minimum) *minimum = lo;
        if (hi > widest) widest = hi;
        sum += hi + (items ? box->gap : 0);
        items++;
    }
    float wide = columns ? widest * (columns < items ? columns : items) + box->gap * ((columns < items ? columns : items) - 1) : sum;
    if (wide > *maximum) *maximum = wide;
}

/* ---- flex and grid ---- */

#define ITEMS_MAX 96
#define MEASURE_BUDGET 3000     /* elements measured for one container's items */

typedef struct {
    int node, text;             /* text: an anonymous item for a run of text */
    int out;                    /* positioned: laid out after the rows, in the normal flow */
    browser_style st;
    browser_box box;
    float base, size, min, max, grow, shrink;   /* border-box widths */
    int ml, mr, auto_left, auto_right, frozen;
    int height, first, last;    /* laid out: height with margins, its items */
    int record, background, border[4];          /* the item's own box, to stretch */
} flex_item;

static int has_text(const char *s)
{
    for (; s && *s; s++) if (!isspace((unsigned char)*s)) return 1;
    return 0;
}

/* A container's items in `order`: its element children, and runs of text,
   from *child on, at most ITEMS_MAX; *child becomes the next one (-1: none). */
static int collect_items(builder *b, browser_style st, flex_item *items, int *child)
{
    const browser_dom *dom = b->dom;
    int n = 0, c = *child;
    for (; c >= 0 && n < ITEMS_MAX; c = dom->nodes[c].next) {
        const dom_node *k = &dom->nodes[c];
        flex_item *it = &items[n];
        memset(it, 0, sizeof(*it));
        it->node = c;
        it->st = st;
        if (!strcmp(k->tag, "#text")) {
            if (st.hidden || !has_text(k->text)) continue;
            it->text = 1;
        } else {
            if (k->tag[0] == '#' || skipped(b, k)) continue;
            it->st = css_compute_box(&b->doc->css, dom, c, st, &it->box);
            if (it->st.hidden || it->box.hide || it->box.display == DISPLAY_NONE) continue;
            it->out = it->box.positioned;
        }
        /* insertion by order, keeping document order for equal ones;
           positioned children go last */
        flex_item moving = *it;
        int i = n;
        while (i > 0 && (items[i - 1].out > moving.out || (items[i - 1].out == moving.out && items[i - 1].box.order > moving.box.order))) { items[i] = items[i - 1]; i--; }
        items[i] = moving;
        n++;
    }
    *child = c;
    return n;
}

static void measure_item(builder *b, flex_item *it, int width, int column);
static int place_item(builder *b, flex_item *it, browser_style parent, float x, int y);
static int resolve(int value, int width) { return value >= 0 ? value : width * -value / 100; }
static float clampf(float v, float low, float high) { return v < low ? low : v > high ? high : v; }

/* Positioned children of a flex or grid container, below its rows: as wide as
   their content, within the page rather than the container (a narrow one
   would break their words up), from the container's left edge where they fit. */
static void out_of_flow(builder *b, flex_item *items, int from, int n, browser_style st, int x)
{
    int page = b->v->width - 8;
    for (int i = from; i < n && !b->failed; i++) {
        flex_item *it = &items[i];
        measure_item(b, it, page, 0);
        float room = (float)(page - it->ml - it->mr);
        it->size = it->box.width ? clampf((float)resolve(it->box.width, page), 8, room) : clampf(it->base, 8, room);
        float outer = it->size + it->ml + it->mr, left = (float)x;
        if (left + outer > page + 4) left = page + 4 - outer;
        it->auto_left = it->auto_right = 0;
        *b->y += place_item(b, it, st, left + it->ml, *b->y);
    }
}
static int in_flow(const flex_item *items, int n)
{
    while (n > 0 && items[n - 1].out) n--;
    return n;
}

/* An item's margins, and the narrowest, preferred and widest widths of its
   border box in a container `width` wide. */
static void measure_item(builder *b, flex_item *it, int width, int column)
{
    browser_box *box = &it->box;
    const dom_node *n = &b->dom->nodes[it->node];
    it->grow = it->text ? 0 : box->grow / 100.0f;
    it->shrink = it->text || !(box->flex & FLEX_SHRINK_SET) ? 1 : box->shrink / 100.0f;
    it->max = 100000;
    if (it->text) {
        float lo = 0, hi = 0, line = 0;
        content_widths(b, it->node, it->st, &lo, &hi, &line, 1);
        it->min = lo; it->base = hi;
        return;
    }
    default_box(n, box);
    it->auto_left = box->margin[3] == BOX_AUTO; it->auto_right = box->margin[1] == BOX_AUTO;
    it->ml = it->auto_left ? 0 : clampi(box->margin[3], 0, 120);
    it->mr = it->auto_right ? 0 : clampi(box->margin[1], 0, 120);
    int sized = box->width ? resolve(box->width, width) : -1, specified = sized;
    if (!column && (box->flex & FLEX_BASIS_SET) && box->basis != BOX_AUTO) specified = resolve(box->basis, width);
    if (box->max_width) it->max = (float)resolve(box->max_width, width);
    float lo = 0, hi = 0;
    int h;
    if (is(n, "img")) {
        hi = (float)image_size(b, it->node, box, 0, &h);
        lo = box->width < 0 || box->max_width < 0 ? 0 : hi;
    }
    else if (is(n, "input") || is(n, "select") || is(n, "textarea") || is(n, "button")) lo = hi = (float)field_size(b, it->node, box, 0, &h);
    else if ((hi = icon_width(b, it->node, it->st)) >= 0) {
        lo = hi;
        lo += box->padding[1] + box->padding[3] + box->border[1] + box->border[3];
        hi = lo;
    } else {
        hi = 0;
        int budget = b->budget;
        if (budget <= 0) b->budget = MEASURE_BUDGET;
        inner_widths(b, it->node, it->st, box, &lo, &hi, 1);
        if (b->budget < 0) { hi = (float)width; if (lo > width / 2) lo = (float)(width / 2); }
        b->budget = budget > 0 ? b->budget : 0;
        int sides = box->padding[1] + box->padding[3] + box->border[1] + box->border[3];
        lo += sides; hi += sides;
    }
    it->base = specified >= 0 ? (float)specified : hi;
    /* min-width: auto is the narrowest content, or the width when narrower
       (a flex-basis of 0 doesn't let an item shrink below its content). A
       smaller min-width (0, to let long text shrink) would break words into
       letters here, where a computer lets them overflow: text keeps its
       longest word, while pictures and fields shrink. */
    it->min = sized >= 0 && sized < lo ? (float)sized : lo;
    if (box->flex & MIN_WIDTH_SET) {
        float given = (float)resolve(box->min_width, width);
        if (given > it->min || replaced(n)) it->min = given;
    }
    if (it->max < it->min) it->max = it->min;
}

/* Lays out an item as a block `it->size` wide at (x, y): its height with margins. */
static int place_item(builder *b, flex_item *it, browser_style parent, float x, int y)
{
    int *outer_y = b->y, iy = y;
    int ml = it->auto_left ? 0 : it->ml, mr = it->auto_right ? 0 : it->mr;
    int box_item = !it->text && !replaced(&b->dom->nodes[it->node]);
    /* a block takes its margins off the room it's given; text and fields don't */
    /* whole pixels, rounded up so that the widest line still fits */
    line_state line = { .x = (int)(x + 0.5f) - (box_item ? ml : 0), .width = (int)(it->size + 0.99f) + (box_item ? ml + mr : 0),
                        .align = parent.align, .first = b->piece_count };
    if (line.width < 8) line.width = 8;
    line_state *outer = b->line;
    b->line = &line; b->y = &iy; b->collapse = 0;
    it->first = b->v->item_count;
    b->last.node = -1;
    if (it->text) add_text(b, b->dom->nodes[it->node].text, parent);
    else { b->item_width = (int)(it->size + 0.99f); element_styled(b, it->node, parent, it->st, it->box); b->item_width = -1; }
    flush_line(b);
    it->last = b->v->item_count;
    it->record = b->last.node == it->node;
    if (it->record) { it->background = b->last.background; memcpy(it->border, b->last.border, sizeof(it->border)); }
    b->line = outer; b->y = outer_y;
    it->height = iy - y;
    return it->height;
}

/* Items in a row share its height: stretched (their background and borders
   reach the bottom) or placed at its top, middle or bottom. */
static void align_row(builder *b, flex_item *items, int count, int align, int row_height)
{
    browser_view *v = b->v;
    for (int i = 0; i < count; i++) {
        flex_item *it = &items[i];
        int delta = row_height - it->height, how = it->box.align_self ? it->box.align_self - 1 : align;
        if (delta <= 0) continue;
        if (how == 0 && it->record && !it->box.height) {
            if (it->background >= 0) v->items[it->background].h += delta;
            if (it->border[2] >= 0) v->items[it->border[2]].y += delta;
            if (it->border[1] >= 0) v->items[it->border[1]].h += delta;
            if (it->border[3] >= 0) v->items[it->border[3]].h += delta;
        } else if (how == 2 || how == 3) {
            int shift = how == 2 ? delta / 2 : delta;
            for (int k = it->first; k < it->last; k++) v->items[k].y += shift;
        }
    }
}

static int takes_room(const flex_item *it)
{
    return it->text || it->base + it->ml + it->mr > 0.5f || it->min > 0.5f || it->grow > 0 || it->box.width || it->box.height > 0;
}


/* A column: one item per row, as wide as the container, or as its content
   when it's aligned to a side or centered. */
static void flex_column(builder *b, flex_item *items, int n, browser_style st, const browser_box *box, int x, int width)
{
    int *y = b->y;
    for (int i = 0; i < n && !b->failed; i++) {
        flex_item *it = &items[i];
        int how = it->box.align_self ? it->box.align_self - 1 : box->align, room = width - it->ml - it->mr;
        float w = it->box.width ? clampf((float)resolve(it->box.width, width), it->min, it->max) : how == 0 ? (float)room : clampf(it->base, it->min, it->max);
        it->size = clampf(w, 8, room > 8 ? (float)room : 8);
        float free = (float)room - it->size, left = how == 2 ? free / 2 : how == 3 ? free : 0;
        if (it->auto_left && it->auto_right) left = free / 2;
        else if (it->auto_left) left = free;
        if (i) *y += box->row_gap;
        place_item(b, it, st, (float)(x + it->ml) + left, *y);
        *y += it->height;
    }
}

/* Rows across `width`, wrapping when items don't fit even at their narrowest. */
static void flex_rows(builder *b, flex_item *items, int n, browser_style st, const browser_box *box, int x, int width)
{
    /* gaps separate the items that take room; ones that show nothing
       (an icon button without a name) don't get one */
    float need = 0;
    for (int i = 0, any = 0; i < n; i++)
        if (takes_room(&items[i])) { need += items[i].min + items[i].ml + items[i].mr + (any ? box->gap : 0); any = 1; }
    /* nowrap would push items off the screen, which doesn't scroll sideways */
    int wrap = box->wrap || need > width;
    int top = *b->y;
    for (int start = 0; start < n && !b->failed;) {
        int end = start, gaps = -1;
        float used = 0;
        for (; end < n; end++) {
            flex_item *it = &items[end];
            if (!takes_room(it)) continue;
            float outer = clampf(it->base, it->min, it->max) + it->ml + it->mr;
            if (wrap && gaps >= 0 && used + box->gap + outer > width) break;
            used += (gaps >= 0 ? box->gap : 0) + outer;
            gaps++;
        }
        if (gaps < 0) gaps = 0;
        int count = end - start;
        flex_item *row = &items[start];
        /* grow into the free space, or shrink to fit, from the items' flex
           base sizes: their sizes within min and max only decide which,
           and hold the items that can't flex that way */
        float hypothetical = (float)box->gap * gaps;
        for (int i = 0; i < count; i++) hypothetical += clampf(row[i].base, row[i].min, row[i].max) + row[i].ml + row[i].mr;
        int growing = hypothetical < (float)width;
        for (int i = 0; i < count; i++) {
            flex_item *it = &row[i];
            float held = clampf(it->base, it->min, it->max);
            it->frozen = growing ? it->grow <= 0 || it->base > held : it->shrink <= 0 || it->base < held;
            it->size = it->frozen ? held : it->base;
        }
        for (int pass = 0; pass < 4; pass++) {
            float free = (float)width - box->gap * gaps, weight = 0;
            for (int i = 0; i < count; i++) free -= row[i].size + row[i].ml + row[i].mr;
            if (free > 0.5f) for (int i = 0; i < count; i++) weight += row[i].frozen ? 0 : row[i].grow;
            else if (free < -0.5f) for (int i = 0; i < count; i++) weight += row[i].frozen ? 0 : row[i].shrink * row[i].size;
            if (weight <= 0) break;
            for (int i = 0; i < count; i++) {
                flex_item *it = &row[i];
                if (it->frozen) continue;
                float share = free > 0 ? free * it->grow / weight : free * it->shrink * it->size / weight, size = it->size + share;
                if (size < it->min || size > it->max) { size = clampf(size, it->min, it->max); it->frozen = 1; }
                it->size = size;
            }
        }
        for (int i = 0; i < count; i++) row[i].size = clampf(row[i].size, row[i].min, row[i].max);
        /* never wider than the container: a long word breaks instead */
        for (int i = 0; i < count; i++) {
            float limit = (float)(width - row[i].ml - row[i].mr);
            if (row[i].size > limit) row[i].size = limit > 8 ? limit : 8;
        }
        /* what's left goes to auto margins, else justify-content spreads it */
        float free = (float)width - box->gap * gaps;
        int autos = 0;
        for (int i = 0; i < count; i++) { free -= row[i].size + row[i].ml + row[i].mr; autos += row[i].auto_left + row[i].auto_right; }
        float offset = 0, between = 0, margin = 0;
        if (free > 0 && autos) margin = free / autos;
        else if (free > 0) {
            switch (box->justify) {
            case 2: offset = free / 2; break;
            case 3: offset = free; break;
            case 4: if (gaps > 0) between = free / gaps; break;
            case 5: between = free / (gaps + 1); offset = between / 2; break;
            case 6: between = free / (gaps + 2); offset = between; break;
            }
        }
        float cursor = (float)x + offset;
        int height = 0, placed = 0;
        for (int i = 0; i < count && !b->failed; i++) {
            flex_item *it = &row[i];
            int room = takes_room(it);
            if (room && placed) cursor += box->gap + between;
            cursor += it->ml + (it->auto_left ? margin : 0);
            place_item(b, it, st, cursor, top);
            if (it->height > height) height = it->height;
            cursor += it->size + it->mr + (it->auto_right ? margin : 0);
            placed |= room;
        }
        align_row(b, row, count, box->align, height);
        top += height;
        start = end;
        if (start < n) top += box->row_gap;
    }
    *b->y = top;
}

/* A flex container: its items in rows, or stacked when it's a column. More
   children than ITEMS_MAX are laid out in turns. */
static void flex(builder *b, int node, browser_style st, const browser_box *box, int x, int width)
{
    /* too narrow to share: the normal flow */
    if (width < 24 || b->depth >= DOM_DEPTH_MAX) { children(b, node, st); return; }
    flex_item *items = malloc(ITEMS_MAX * sizeof(*items));
    if (!items) { b->failed = 1; return; }
    b->depth++;
    int column = box->direction >= 2, laid = 0;
    for (int child = b->dom->nodes[node].first; child >= 0 && !b->failed;) {
        int all = collect_items(b, st, items, &child), n = in_flow(items, all);
        if (box->direction == 1 || box->direction == 3)
            for (int i = 0; i < n / 2; i++) { flex_item t = items[i]; items[i] = items[n - 1 - i]; items[n - 1 - i] = t; }
        for (int i = 0; i < n; i++) {
            flex_item *it = &items[i];
            /* a stretched item of a column is as wide as the container: its
               content needn't be measured */
            int how = it->box.align_self ? it->box.align_self - 1 : box->align;
            if (column && !it->text && how == 0 && it->box.margin[1] != BOX_AUTO && it->box.margin[3] != BOX_AUTO) {
                default_box(&b->dom->nodes[it->node], &it->box);
                it->ml = clampi(it->box.margin[3], 0, 120); it->mr = clampi(it->box.margin[1], 0, 120);
                it->min = 0; it->max = it->box.max_width ? (float)resolve(it->box.max_width, width) : 100000;
                it->base = (float)width;
                continue;
            }
            measure_item(b, it, width, column);
        }
        if (n && laid) *b->y += box->row_gap;
        if (column) flex_column(b, items, n, st, box, x, width);
        else flex_rows(b, items, n, st, box, x, width);
        laid |= n > 0;
        b->collapse = 0;
        out_of_flow(b, items, n, all, st, x);
    }
    b->collapse = 0;
    b->depth--;
    free(items);
}

/* A grid container: its items in rows of the columns grid-template-columns
   gives, each spanning one or more. */
static void grid(builder *b, int node, browser_style st, const browser_box *box, int x, int width)
{
    if (width < 24 || b->depth >= DOM_DEPTH_MAX) { children(b, node, st); return; }
    flex_item *items = malloc(ITEMS_MAX * sizeof(*items));
    if (!items) { b->failed = 1; return; }
    b->depth++;
    int gap = box->gap;
    /* the columns */
    unsigned char kind[GRID_TRACKS];
    short value[GRID_TRACKS];
    int count = box->tracks;
    memcpy(kind, box->track_kind, sizeof(kind));
    memcpy(value, box->track, sizeof(value));
    if (box->fill > 0) {
        count = (width + gap) / (box->fill + gap);
        count = clampi(count, 1, GRID_TRACKS);
        for (int i = 0; i < count; i++) { kind[i] = TRACK_FR; value[i] = 100; }
    }
    if (count <= 0) { count = 1; kind[0] = TRACK_FR; value[0] = 100; }
    int shares = 1;
    for (int i = 0; i < count; i++) shares &= kind[i] == TRACK_FR || kind[i] == TRACK_AUTO;
    /* columns of equal shares narrower than a few words are merged */
    while (shares && count > 1 && (width - gap * (count - 1)) / count < 56) count--;
    float col[GRID_TRACKS], fixed = 0, fr = 0;
    for (int i = 0; i < count; i++) {
        if (kind[i] == TRACK_PX) fixed += value[i];
        else if (kind[i] == TRACK_PERCENT) fixed += (float)width * value[i] / 100;
        else fr += kind[i] == TRACK_FR ? value[i] : 100;
    }
    float room = (float)(width - gap * (count - 1)), left = room - fixed > 0 ? room - fixed : 0;
    float scale = fixed > room && fixed > 0 ? room / fixed : 1;
    for (int i = 0; i < count; i++) {
        if (kind[i] == TRACK_PX) col[i] = value[i] * scale;
        else if (kind[i] == TRACK_PERCENT) col[i] = (float)width * value[i] / 100 * scale;
        else col[i] = fr > 0 ? left * (kind[i] == TRACK_FR ? value[i] : 100) / fr : 0;
    }
    int *y = b->y, top = *y, laid = 0;
    for (int child = b->dom->nodes[node].first; child >= 0 && !b->failed;) {
    int all = collect_items(b, st, items, &child), n = in_flow(items, all);
    if (n && laid) top += box->row_gap;
    laid |= n > 0;
    for (int start = 0; start < n && !b->failed;) {
        int end = start, used = 0;
        float position[ITEMS_MAX];
        for (; end < n; end++) {
            flex_item *it = &items[end];
            int span = it->box.span == SPAN_ROW ? count : clampi(it->box.span ? it->box.span : 1, 1, count);
            if (used + span > count && end > start) break;
            float area_x = (float)x, area_w = (float)(gap * (span - 1));
            for (int k = 0; k < used; k++) area_x += col[k] + gap;
            for (int k = used; k < used + span; k++) area_w += col[k];
            if (!it->text) {
                default_box(&b->dom->nodes[it->node], &it->box);
                it->auto_left = it->box.margin[3] == BOX_AUTO; it->auto_right = it->box.margin[1] == BOX_AUTO;
                it->ml = it->auto_left ? 0 : clampi(it->box.margin[3], 0, 120);
                it->mr = it->auto_right ? 0 : clampi(it->box.margin[1], 0, 120);
            }
            it->size = area_w - it->ml - it->mr;
            if (it->box.width > 0 && it->box.width < it->size) it->size = it->box.width;
            if (it->size < 8) it->size = 8;
            position[end - start] = area_x + it->ml;
            used += span;
        }
        int height = 0;
        for (int i = start; i < end && !b->failed; i++) {
            place_item(b, &items[i], st, position[i - start], top);
            if (items[i].height > height) height = items[i].height;
        }
        align_row(b, &items[start], end - start, box->align, height);
        top += height;
        start = end;
        if (start < n) top += box->row_gap;
    }
    *y = top;
    b->collapse = 0;
    out_of_flow(b, items, n, all, st, x);
    top = *y;
    }
    b->collapse = 0;
    b->depth--;
    free(items);
}

/* A floated box: as wide as it says or as its content, at the left or right
   of the room where it fits, with the following lines beside it. */
static void float_box(builder *b, int node, browser_style parent, browser_style st, browser_box box)
{
    line_state *l = b->line;
    if (b->float_count >= FLOATS_MAX) {
        /* too many: laid out where it is, as a block */
        box.float_side = 0;
        int x = l->full ? l->full_x : l->x, width = l->full ? l->full_width : l->width;
        flush_line(b);
        block(b, node, st, &box, x, width);
        return;
    }
    flush_line(b);
    int room_x = l->full ? l->full_x : l->x, room = l->full ? l->full_width : l->width;
    flex_item it;
    memset(&it, 0, sizeof(it));
    it.node = node; it.st = st; it.box = box;
    measure_item(b, &it, room, 0);
    int outer_room = room - it.ml - it.mr;
    it.size = box.width ? clampf((float)resolve(box.width, room), 8, (float)outer_room) : clampf(it.base, it.min < outer_room ? it.min : 8, (float)outer_room);
    if (it.size < 8) it.size = 8;
    int outer = (int)(it.size + 0.5f) + it.ml + it.mr, y = *b->y, left, right;
    for (int pass = 0; pass <= FLOATS_MAX; pass++) {
        int next = band(b, y, room_x, room, &left, &right);
        if (next < 0 || right - left >= outer) break;
        y = next;
    }
    float x = box.float_side == 1 ? (float)(left + it.ml) : (float)(right - it.mr) - it.size;
    int collapse = b->collapse, link = b->link, label = b->label;
    it.auto_left = it.auto_right = 0;
    place_item(b, &it, parent, x, y);
    b->collapse = collapse; b->link = link; b->label = label;
    float_area *f = &b->floats[b->float_count++];
    f->x = (int)(x + 0.5f) - it.ml; f->y = y; f->w = outer; f->h = it.height; f->side = box.float_side;
}

static int collect_rows(builder *b, int node, int *rows, int count, int max, int depth)
{
    for (int c = b->dom->nodes[node].first; c >= 0 && count < max; c = b->dom->nodes[c].next) {
        const dom_node *n = &b->dom->nodes[c];
        if (is(n, "tr")) rows[count++] = c;
        else if ((is(n, "thead") || is(n, "tbody") || is(n, "tfoot")) && depth < 2) count = collect_rows(b, c, rows, count, max, depth + 1);
    }
    return count;
}

static void table(builder *b, int node, browser_style st, int x, int width)
{
    const browser_dom *dom = b->dom;
    const dom_node *t = &dom->nodes[node];
    int *y = b->y;
    int spacing = *dom_attr(t, "cellspacing") ? clampi((int)(atoi(dom_attr(t, "cellspacing")) * 0.6f + 0.5f), 0, 10) : 1;
    int padding = *dom_attr(t, "cellpadding") ? clampi((int)(atoi(dom_attr(t, "cellpadding")) * 0.6f + 0.5f), 0, 12) : 2;
    int borders = atoi(dom_attr(t, "border")) > 0;
    for (int c = t->first; c >= 0; c = dom->nodes[c].next)
        if (is(&dom->nodes[c], "caption")) { browser_box box; browser_style cs = css_compute_box(&b->doc->css, dom, c, st, &box); if (!cs.hidden) { cs.align = 1; block(b, c, cs, &box, x, width); } }
    int rows[512], row_count = collect_rows(b, node, rows, 0, 512, 0);
    if (!row_count) { children(b, node, st); return; }
    int columns = 0;
    float minimum[COLUMNS_MAX] = {0}, maximum[COLUMNS_MAX] = {0}, fixed[COLUMNS_MAX] = {0};
    for (int r = 0; r < row_count; r++) {
        int column = 0;
        browser_box rbox;
        browser_style rs = css_compute_box(&b->doc->css, dom, rows[r], st, &rbox);
        for (int c = dom->nodes[rows[r]].first; c >= 0 && column < COLUMNS_MAX; c = dom->nodes[c].next) {
            const dom_node *n = &dom->nodes[c];
            if (!is(n, "td") && !is(n, "th")) continue;
            int span = clampi(atoi(dom_attr(n, "colspan")), 1, COLUMNS_MAX - column);
            browser_box box;
            browser_style cs = css_compute_box(&b->doc->css, dom, c, rs, &box);
            if (cs.hidden || box.display == DISPLAY_NONE) continue;
            float lo = 0, hi = 0, line = 0;
            if (r < 64) content_widths(b, c, cs, &lo, &hi, &line, 0);
            int pad = (box.set & 0xF0 ? 0 : 2 * padding) + box.padding[1] + box.padding[3];
            lo += pad; hi += pad;
            if (span == 1) {
                if (lo > minimum[column]) minimum[column] = lo;
                if (hi > maximum[column]) maximum[column] = hi;
                if (box.width > 0 && box.width > fixed[column]) fixed[column] = box.width;
                else if (box.width < 0 && fixed[column] == 0) fixed[column] = (float)(width * -box.width / 100);
            }
            column += span;
        }
        if (column > columns) columns = column;
    }
    if (!columns) return;
    /* The auto table layout: columns get at least their longest word, and
       share what's left in proportion to their widest line. */
    float avail = (float)(width - spacing * (columns + 1)), lo = 0, hi = 0, col[COLUMNS_MAX];
    for (int i = 0; i < columns; i++) {
        if (fixed[i] > minimum[i]) minimum[i] = fixed[i];
        if (fixed[i] > maximum[i]) maximum[i] = fixed[i];
        if (maximum[i] < minimum[i]) maximum[i] = minimum[i];
        if (maximum[i] < 8) maximum[i] = 8;
        lo += minimum[i]; hi += maximum[i];
    }
    int full = *dom_attr(t, "width") || (st.align == 1);
    browser_box tbox; css_compute_box(&b->doc->css, dom, node, st, &tbox);
    if (tbox.width) full = 1;
    int columns_width = 0, cols[COLUMNS_MAX];
    for (int i = 0; i < columns; i++) {
        if (hi <= avail) col[i] = full ? maximum[i] + (avail - hi) * maximum[i] / hi : maximum[i];
        else if (lo < avail) col[i] = minimum[i] + (avail - lo) * (maximum[i] - minimum[i]) / (hi - lo > 0 ? hi - lo : 1);
        else col[i] = minimum[i] * avail / (lo > 0 ? lo : 1);
        /* whole pixels, rounded up so that the widest line still fits */
        cols[i] = (int)col[i] + (col[i] > (int)col[i] ? 1 : 0);
        columns_width += cols[i];
    }
    for (int i = columns - 1; i >= 0 && columns_width > avail; i--)
        if (cols[i] > (int)col[i]) { cols[i]--; columns_width--; }
    *y += spacing;
    for (int r = 0; r < row_count; r++) {
        browser_box rbox;
        browser_style rs = css_compute_box(&b->doc->css, dom, rows[r], st, &rbox);
        if (rs.hidden || rbox.hide || rbox.display == DISPLAY_NONE) continue;
        int row_top = *y, row_h = 0, cx = x + spacing, column = 0;
        int row_background = rs.background ? add_item(b, item(ITEM_RECT, x, row_top, width, 0, rs.background)) : -1;
        uint32_t backdrop = b->backdrop;
        if (rs.background) b->backdrop = rs.background;
        struct { int background, first, last, height, valign, frame; } cells[COLUMNS_MAX];
        int cell_count = 0;
        for (int c = dom->nodes[rows[r]].first; c >= 0 && column < columns; c = dom->nodes[c].next) {
            const dom_node *n = &dom->nodes[c];
            if (!is(n, "td") && !is(n, "th")) continue;
            int span = clampi(atoi(dom_attr(n, "colspan")), 1, columns - column);
            browser_box box;
            browser_style cs = css_compute_box(&b->doc->css, dom, c, rs, &box);
            if (cs.hidden || box.display == DISPLAY_NONE) continue;
            cs.background = cs.background ? cs.background : 0;
            int cw = spacing * (span - 1);
            for (int k = column; k < column + span; k++) cw += cols[k];
            int pad = box.set & 0xF0 ? 0 : padding;
            int background = cs.background ? add_item(b, item(ITEM_RECT, cx, row_top, cw, 0, cs.background)) : -1;
            int first = b->v->item_count, cell_y = row_top + pad + box.padding[0];
            line_state line = { .x = cx + pad + box.padding[3], .width = cw - 2 * pad - box.padding[1] - box.padding[3], .align = cs.align, .first = b->piece_count };
            if (line.width < 8) line.width = 8;
            line_state *outer = b->line;
            int *outer_y = b->y;
            b->line = &line; b->y = &cell_y; b->collapse = 1000;   /* no margin above the first block */
            uint32_t row_backdrop = b->backdrop;
            if (cs.background) b->backdrop = cs.background;
            children(b, c, cs);
            flush_line(b);
            b->backdrop = row_backdrop;
            b->line = outer; b->y = outer_y;
            int height = cell_y + pad + box.padding[2] - row_top;
            if (box.height > height) height = box.height;
            if (cell_count < COLUMNS_MAX) {
                const char *valign = dom_attr(n, "valign");
                cells[cell_count].background = background; cells[cell_count].first = first;
                cells[cell_count].last = b->v->item_count; cells[cell_count].height = height;
                cells[cell_count].valign = eq(valign, "top") ? 0 : eq(valign, "bottom") ? 2 : 1;
                cells[cell_count].frame = borders ? (cx | (cw << 16)) : -1;
                cell_count++;
            }
            if (height > row_h) row_h = height;
            cx += cw + spacing;
            column += span;
        }
        for (int i = 0; i < cell_count; i++) {
            if (cells[i].background >= 0) b->v->items[cells[i].background].h = row_h;
            int shift = cells[i].valign == 1 ? (row_h - cells[i].height) / 2 : cells[i].valign == 2 ? row_h - cells[i].height : 0;
            if (shift > 0) for (int k = cells[i].first; k < cells[i].last; k++) b->v->items[k].y += shift;
            if (cells[i].frame >= 0) {
                int fx = cells[i].frame & 0xFFFF, fw = cells[i].frame >> 16;
                view_item frame = item(ITEM_FRAME, fx, row_top, fw, row_h, 0xff909090);
                frame.flags = 1;
                add_item(b, frame);
            }
        }
        if (row_background >= 0) b->v->items[row_background].h = row_h;
        b->backdrop = backdrop;
        *y = row_top + row_h + spacing;
    }
    b->collapse = 0;
}

/* ---- building ---- */

void browser_view_free(browser_view *v)
{
    for (int i = 0; v->controls && i < v->control_count; i++) { free(v->controls[i].value); free(v->controls[i].initial); }
    free(v->items); free(v->reach); free(v->rest); free(v->text); free(v->strings); free(v->links);
    free(v->controls); free(v->options); free(v->forms); free(v->anchors);
    memset(v, 0, sizeof(*v));
}

int browser_view_build(browser_view *v, const browser_document *doc, int width, view_measure_fn measure, char *err, size_t errlen)
{
    memset(v, 0, sizeof(*v));
    if (!doc->dom || !doc->dom->count) { snprintf(err, errlen, "Nothing to show."); return -1; }
    builder *b = calloc(1, sizeof(*b));
    v->item_capacity = 512;
    v->items = malloc((size_t)v->item_capacity * sizeof(*v->items));
    v->links = calloc(VIEW_LINKS_MAX, sizeof(*v->links));
    v->controls = calloc(VIEW_CONTROLS_MAX, sizeof(*v->controls));
    v->options = calloc(VIEW_OPTIONS_MAX, sizeof(*v->options));
    v->forms = calloc(VIEW_FORMS_MAX, sizeof(*v->forms));
    v->anchors = calloc(VIEW_ANCHORS_MAX, sizeof(*v->anchors));
    int *control_of = malloc((size_t)doc->dom->count * sizeof(int));
    if (b) {
        /* optional: without it, content is measured again where needed */
        b->measured = malloc((size_t)doc->dom->count * sizeof(*b->measured));
        b->known = calloc((size_t)doc->dom->count, 1);
        if (!b->measured || !b->known) { free(b->measured); free(b->known); b->measured = NULL; b->known = NULL; }
    }
    if (!b || !v->items || !v->links || !v->controls || !v->options || !v->forms || !v->anchors || !control_of) {
        if (b) { free(b->measured); free(b->known); }
        free(b); free(control_of); browser_view_free(v);
        snprintf(err, errlen, "Not enough memory to lay out the page.");
        return -1;
    }
    v->width = width;
    v->paper = 0xffffffff;
    v->ink = 0xff000000;
    b->v = v; b->doc = doc; b->dom = doc->dom; b->measure = measure; b->control_of = control_of;
    b->link = b->label = b->consent = -1; b->scripting = doc->scripting;
    b->item_width = -1; b->last.node = -1;
    strcpy(b->base, doc->url);
    for (int i = 0; i < doc->dom->count; i++)
        if (is(&doc->dom->nodes[i], "base") && *dom_attr(&doc->dom->nodes[i], "href")) {
            char resolved[BROWSER_URL_MAX];
            if (browser_url_resolve(doc->url, dom_attr(&doc->dom->nodes[i], "href"), resolved, sizeof(resolved)) == 0) strcpy(b->base, resolved);
            break;
        }
    register_forms(b);
    int y = 0;
    /* The page's margin: <body> has none of its own here, as pages without
       a <body> element need one too. */
    line_state line = { .x = 4, .width = width - 8 };
    b->line = &line; b->y = &y;
    browser_style initial = { .color = 0xff000000, .link = DEFAULT_LINK, .scale = TEXT_SCALE, .pre = !doc->dom->html };
    if (!b->failed) {
        if (doc->dom->html) children(b, 0, initial);
        else {
            for (int i = 0; i < doc->dom->count; i++) if (!strcmp(doc->dom->nodes[i].tag, "#text")) add_text(b, doc->dom->nodes[i].text, initial);
        }
        flush_line(b);
    }
    int failed = b->failed;
    free(control_of); free(b->measured); free(b->known); free(b);
    if (failed) { browser_view_free(v); snprintf(err, errlen, "Not enough memory to lay out the page."); return -1; }
    v->height = y + 8;
    v->reach = malloc(((size_t)v->item_count + 1) * sizeof(int));
    v->rest = malloc(((size_t)v->item_count + 1) * sizeof(int));
    if (!v->reach || !v->rest) { browser_view_free(v); snprintf(err, errlen, "Not enough memory to lay out the page."); return -1; }
    int reach = -1000000;
    for (int i = 0; i < v->item_count; i++) {
        view_item *it = &v->items[i];
        if (it->y + it->h > reach) reach = it->y + it->h;
        if (reach > v->height) v->height = reach;
        v->reach[i] = reach;
    }
    int rest = 1000000000;
    v->rest[v->item_count] = rest;
    for (int i = v->item_count - 1; i >= 0; i--) {
        if (v->items[i].y < rest) rest = v->items[i].y;
        v->rest[i] = rest;
    }
    return 0;
}

/* ---- queries ---- */

int browser_view_first(const browser_view *v, int top)
{
    int low = 0, high = v->item_count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        if (v->reach[mid] < top) low = mid + 1; else high = mid;
    }
    return low;
}

int browser_view_hit(const browser_view *v, int x, int y, int *link, int *control)
{
    int found = -1;
    *link = *control = -1;
    for (int i = browser_view_first(v, y); browser_view_more(v, i, y + 1); i++) {
        const view_item *it = &v->items[i];
        if (it->link < 0 && it->control < 0) continue;
        /* A little slack: a cursor is less precise than a mouse. */
        if (x >= it->x - 1 && x < it->x + it->w + 1 && y >= it->y - 1 && y < it->y + it->h + 1) {
            found = i; *link = it->link; *control = it->control;
        }
    }
    return found;
}

/* Reading order: items sharing most of a row go left to right. */
static int before(const view_item *a, const view_item *b)
{
    int top = a->y > b->y ? a->y : b->y, bottom = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    int shorter = a->h < b->h ? a->h : b->h;
    if (bottom - top >= (shorter + 1) / 2) return a->x < b->x;
    return a->y * 2 + a->h < b->y * 2 + b->h;
}

int browser_view_step(const browser_view *v, int from, int x, int y, int direction)
{
    view_item point = { x, y, 1, 1, 0, -1, 0, 0, 0, ITEM_RECT, -1, -1 };
    const view_item *here = from >= 0 && from < v->item_count ? &v->items[from] : &point;
    int best = -1;
    for (int i = 0; i < v->item_count; i++) {
        const view_item *it = &v->items[i];
        if (i == from || (it->link < 0 && it->control < 0)) continue;
        if (it->kind == ITEM_TEXT && it->link < 0) continue;     /* a label: its field is a stop */
        if (here->link >= 0 && it->link == here->link) continue;
        if (here->control >= 0 && it->control == here->control) continue;
        if (it->kind == ITEM_CONTROL && v->controls[it->control].disabled) continue;
        if (direction > 0 ? !before(here, it) : !before(it, here)) continue;
        if (best < 0 || (direction > 0 ? before(it, &v->items[best]) : before(&v->items[best], it))) best = i;
    }
    return best;
}

int browser_view_anchor(const browser_view *v, const char *id)
{
    for (int i = 0; i < v->anchor_count; i++) if (!strcmp(view_string(v, v->anchors[i].id), id)) return v->anchors[i].y;
    return -1;
}

int browser_view_set_value(browser_view *v, int control, const char *value)
{
    if (control < 0 || control >= v->control_count) return -1;
    view_control *c = &v->controls[control];
    char *copy = copy_value(value);
    if (!copy) return -1;
    if (c->maxlength > 0) {
        /* maxlength counts characters. */
        int chars = 0;
        for (char *p = copy; *p; p += utf8_length(p)) if (++chars > c->maxlength) { *p = 0; break; }
    }
    free(c->value);
    c->value = copy;
    return 0;
}

void browser_view_toggle(browser_view *v, int control)
{
    if (control < 0 || control >= v->control_count) return;
    view_control *c = &v->controls[control];
    if (c->kind == CONTROL_CHECKBOX) c->checked = !c->checked;
    else if (c->kind == CONTROL_RADIO) {
        const char *name = view_string(v, c->name);
        for (int i = 0; i < v->control_count; i++) {
            view_control *o = &v->controls[i];
            if (o->kind == CONTROL_RADIO && o->form == c->form && *name && !strcmp(view_string(v, o->name), name)) o->checked = 0;
        }
        c->checked = 1;
    }
}

void browser_view_reset(browser_view *v, int form)
{
    for (int i = 0; i < v->control_count; i++) {
        view_control *c = &v->controls[i];
        if (c->form != form) continue;
        char *copy = copy_value(c->initial);
        if (copy) { free(c->value); c->value = copy; }
        c->checked = c->checked_initial;
        c->selected = c->selected_initial;
    }
}

int browser_view_submits_on_enter(const browser_view *v, int control)
{
    if (control < 0 || control >= v->control_count) return 0;
    const view_control *c = &v->controls[control];
    return c->single && c->form >= 0 && v->forms[c->form].fields == 1;
}

static int encode(char *out, size_t size, size_t *used, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char chunk[3];
        size_t n = 1;
        if (isalnum(*p) || *p == '*' || *p == '-' || *p == '.' || *p == '_') chunk[0] = (char)*p;
        else if (*p == ' ') chunk[0] = '+';
        else if (*p == '\n') { if (*used + 6 >= size) return -1; memcpy(out + *used, "%0D%0A", 6); *used += 6; continue; }
        else if (*p == '\r') continue;
        else { chunk[0] = '%'; chunk[1] = hex[*p >> 4]; chunk[2] = hex[*p & 15]; n = 3; }
        if (*used + n >= size) return -1;
        memcpy(out + *used, chunk, n);
        *used += n;
    }
    out[*used] = 0;
    return 0;
}
static int pair(char *out, size_t size, size_t *used, const char *name, const char *value)
{
    if (*used) {
        if (*used + 1 >= size) return -1;
        out[(*used)++] = '&';
    }
    if (encode(out, size, used, name) < 0 || *used + 1 >= size) return -1;
    out[(*used)++] = '=';
    return encode(out, size, used, value);
}

/* Appends a JSON string to a buffer with room for it. */
static size_t json_string(char *out, const char *s)
{
    size_t used = 0;
    out[used++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[used++] = '\\'; out[used++] = (char)c; }
        else if (c < 0x20) used += (size_t)sprintf(out + used, "\\u%04x", c);
        else out[used++] = (char)c;
    }
    out[used++] = '"';
    return used;
}
char *browser_view_values(const browser_view *v)
{
    size_t capacity = 0, used = 0;
    char *out = NULL;
    for (int i = 0; i < v->control_count; i++) {
        const view_control *c = &v->controls[i];
        int text = (c->kind == CONTROL_TEXT || c->kind == CONTROL_PASSWORD || c->kind == CONTROL_TEXTAREA) && strcmp(c->value, c->initial);
        int checked = (c->kind == CONTROL_CHECKBOX || c->kind == CONTROL_RADIO) && c->checked != c->checked_initial;
        int selected = c->kind == CONTROL_SELECT && c->selected != c->selected_initial;
        if ((!text && !checked && !selected) || c->node < 0) continue;
        size_t need = used + (text ? strlen(c->value) * 6 : 0) + 96;
        if (need > capacity) {
            capacity = need * 2;
            char *grown = realloc(out, capacity);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        used += (size_t)sprintf(out + used, "%s[%d,", used ? "," : "[", c->node);
        if (text) used += json_string(out + used, c->value);
        else used += (size_t)sprintf(out + used, "null");
        used += (size_t)sprintf(out + used, checked ? (c->checked ? ",true," : ",false,") : ",null,");
        used += (size_t)(selected ? sprintf(out + used, "%d]", c->selected) : sprintf(out + used, "null]"));
    }
    if (out) { out[used++] = ']'; out[used] = 0; }
    return out;
}

int browser_view_submit(const browser_view *v, int form, int submitter, char *url, size_t url_size,
                        char **body, char *err, size_t errlen)
{
    *body = NULL;
    if (form < 0 || form >= v->form_count) { snprintf(err, errlen, "This field isn't part of a form."); return -1; }
    const view_form *f = &v->forms[form];
    size_t size = 64 * 1024, used = 0;
    char *data = malloc(size);
    if (!data) { snprintf(err, errlen, "Not enough memory to send the form."); return -1; }
    data[0] = 0;
    int ok = 1;
    for (int i = 0; i < v->control_count && ok; i++) {
        const view_control *c = &v->controls[i];
        const char *name = view_string(v, c->name);
        if (c->form != form || c->disabled || !*name) continue;
        switch (c->kind) {
        case CONTROL_CHECKBOX: case CONTROL_RADIO:
            if (c->checked) ok = pair(data, size, &used, name, *c->value ? c->value : "on") == 0;
            break;
        case CONTROL_SELECT:
            if (c->option_count) ok = pair(data, size, &used, name, view_string(v, v->options[c->option_first + c->selected].value)) == 0;
            break;
        case CONTROL_SUBMIT: case CONTROL_BUTTON:
            if (i == submitter) ok = pair(data, size, &used, name, c->value) == 0;
            break;
        case CONTROL_IMAGE:
            if (i == submitter) {
                char coordinate[160];
                snprintf(coordinate, sizeof(coordinate), "%.150s.x", name);
                ok = pair(data, size, &used, coordinate, "0") == 0;
                snprintf(coordinate, sizeof(coordinate), "%.150s.y", name);
                if (ok) ok = pair(data, size, &used, coordinate, "0") == 0;
            }
            break;
        case CONTROL_RESET: case CONTROL_FILE: break;
        default: ok = pair(data, size, &used, name, c->value) == 0;
        }
    }
    if (!ok) { free(data); snprintf(err, errlen, "The form is too large to send."); return -1; }
    const char *action = view_string(v, f->action);
    if (f->post) {
        if (strlen(action) >= url_size) { free(data); snprintf(err, errlen, "The form's address is too long."); return -1; }
        strcpy(url, action);
        *body = data;
        return 0;
    }
    size_t base = strcspn(action, "?#");
    if (base + 1 + used >= url_size) { free(data); snprintf(err, errlen, "The search is too long to send."); return -1; }
    memcpy(url, action, base);
    url[base] = '?';
    memcpy(url + base + 1, data, used + 1);
    free(data);
    return 0;
}

/* ARK Browser page view: lays out the DOM with its CSS into positioned
   items, much like a desktop browser's normal flow, scaled for the PSP:
   blocks stack, inline text wraps into lines, tables split their width
   between columns, and form fields, images and list markers are boxes. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "view.h"
#include "url.h"

#define PIECES_MAX  1024        /* pieces on the open lines */
#define ASCENT      13.0f       /* firmware font metrics at size 1.0 */
#define LINE        17.0f
#define FIELD_H     17
#define DEFAULT_LINK 0xffee0000u    /* #0000EE */

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
} line_state;

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
    int pad;                    /* vertical padding of the inline background */
    int list_kind, *list_number, marker, marker_number;
    float marker_scale;
    uint32_t marker_color;
    int depth, scripting, failed;
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

static void place_marker(builder *b, int line_x, int top, float asc)
{
    if (!b->marker) return;
    float s = b->marker_scale;
    if (b->marker == 1) {
        int size = s > 0.7f ? 5 : 4;
        view_item it = item(ITEM_BULLET, line_x - 10, top + (int)(asc - ascent(s) + (LINE * s - size) / 2), size, size, b->marker_color);
        add_item(b, it);
    } else {
        char number[16];
        int n = snprintf(number, sizeof(number), "%d.", b->marker_number);
        int at = store(b, number, (size_t)n, 0);
        float w = width_of(b, number, n, s, 0);
        if (at >= 0) {
            view_item it = item(ITEM_TEXT, line_x - 4 - (int)w, top + (int)(asc - ascent(s)), (int)w + 1, (int)(LINE * s), b->marker_color);
            it.text = at; it.length = n; it.scale = s;
            add_item(b, it);
        }
    }
    b->marker = 0;
}

static void flush_line(builder *b)
{
    line_state *l = b->line;
    if (!l) return;
    if (!l->count) { l->space = 0; return; }
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
            view_item it = item(ITEM_TEXT, px, py, (int)(p[i].width + 0.99f), (int)(LINE * p[i].scale + 0.5f), p[i].color);
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
    browser_style gap = l->space_style;
    float space = l->space && l->count ? width_of(b, " ", 1, gap.scale, gap.flags) : 0;
    float width = width_of(b, copy, n, st.scale, st.flags);
    if (l->count && l->used + space + width > l->width) {
        if (l->space || !carry_word(b, width)) flush_line(b);
        space = 0;
    }
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
        if (start < n) flush_line(b);
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
    if (l->count && l->used + (l->space ? 4 : 0) + w > l->width) flush_line(b);
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

static int has_attr(const dom_node *n, const char *name)
{
    for (int i = 0; i < n->attribute_count; i++) if (eq(n->attributes[i].name, name)) return 1;
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
            form_of[i] = v->form_count++;
        }
        if (!(is(n, "input") || is(n, "select") || is(n, "textarea") || is(n, "button"))) continue;
        if (v->control_count >= VIEW_CONTROLS_MAX) { v->truncated = 1; continue; }
        view_control *c = &v->controls[v->control_count];
        memset(c, 0, sizeof(*c));
        c->kind = control_kind(n);
        c->form = form_of[i];
        c->item = -1;
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
            if (!*label) snprintf(label, sizeof(label), "%s", c->kind == CONTROL_RESET ? "Reset" : c->kind == CONTROL_SUBMIT ? "Submit" : "Button");
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
    if (box->width > 0) w = box->width;
    else if (box->width < 0 && room > 0) w = room * -box->width / 100;
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
    int first = v->item_count;
    view_item it = item(ITEM_CONTROL, 0, 0, w, h, st.color);
    it.control = (short)index; it.link = (short)b->link; it.scale = s;
    int at = add_item(b, it);
    if (at < 0) return;
    c->item = at;
    add_box(b, first, w, h, h - 3 > 0 ? (float)(h - 3) : (float)h);
}

/* The size of an image's box (0 when it isn't shown), fitted into `room`. */
static int image_size(builder *b, int node, const browser_box *box, int room, int *height)
{
    const char *alt = dom_attr(&b->dom->nodes[node], "alt");
    int w = box->width > 0 ? box->width : box->width < 0 && room > 0 ? room * -box->width / 100 : 0;
    int h = box->height > 0 ? box->height : 0;
    if (!w && !h) {
        if (!*alt) return 0;    /* size unknown and nothing to say: decoration */
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

static void image(builder *b, int node, browser_style st, browser_box *box)
{
    const dom_node *n = &b->dom->nodes[node];
    const char *alt = dom_attr(n, "alt");
    int h, w = image_size(b, node, box, b->line->width, &h);
    if (w <= 0) return;
    int first = b->v->item_count;
    view_item it = item(ITEM_IMAGE, 0, 0, w, h, st.color);
    it.link = (short)b->link; it.control = (short)b->label;
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
                                 "datalist", "param", "source", "track", "area", "map", "dialog"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++) if (is(n, tags[i])) return 1;
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
    if (st.background) background = add_item(b, item(ITEM_RECT, x, top, w, 0, st.background));
    int bt = box->border[0], bb = box->border[2], bl = box->border[3], br = box->border[1];
    int pt = box->padding[0], pb = box->padding[2], pl = box->padding[3], pr = box->padding[1];
    if (bt + pt > 0) { *y += bt + pt; b->collapse = 0; }
    int inner_x = x + bl + pl, inner_w = w - bl - br - pl - pr;
    if (inner_w < 8) inner_w = 8;
    int content_top = *y;

    /* The content's own line context. */
    line_state line = { .x = inner_x, .width = inner_w, .align = box->justify ? box->justify - 1 : st.align, .first = b->piece_count };
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
    if (is(n, "table") || box->display == DISPLAY_TABLE) table(b, node, st, inner_x, inner_w);
    else children(b, node, st);
    flush_line(b);
    if (b->marker && is(n, "li")) { place_marker(b, inner_x, *y, ascent(st.scale)); *y += (int)(LINE * st.scale); }
    b->line = outer;
    b->list_kind = list_kind; b->list_number = list_number;
    if (box->height > 0 && *y - content_top < box->height) *y = content_top + box->height;
    if (bb + pb > 0) { *y += bb + pb; b->collapse = 0; }
    int bottom = *y;
    if (background >= 0) v->items[background].h = bottom - top;
    uint32_t c;
    if (bt) { c = box->border_color[0] ? box->border_color[0] : st.color; add_item(b, item(ITEM_RECT, x, top, w, bt, c)); }
    if (bb) { c = box->border_color[2] ? box->border_color[2] : st.color; add_item(b, item(ITEM_RECT, x, bottom - bb, w, bb, c)); }
    if (bl) { c = box->border_color[3] ? box->border_color[3] : st.color; add_item(b, item(ITEM_RECT, x, top, bl, bottom - top, c)); }
    if (br) { c = box->border_color[1] ? box->border_color[1] : st.color; add_item(b, item(ITEM_RECT, x + w - br, top, br, bottom - top, c)); }
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
static int shrink_width(builder *b, int node, browser_style st, browser_box *box)
{
    float lo = 0, hi = 0, line = 0;
    for (int c = b->dom->nodes[node].first; c >= 0; c = b->dom->nodes[c].next) content_widths(b, c, st, &lo, &hi, &line, 1);
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
    if (is(n, "body") || is(n, "html")) {
        if (st.background) b->v->paper = st.background;
        if (st.color != parent.color) b->v->ink = st.color;
        st.background = 0;   /* drawn as the page's paper */
    }
    anchor(b, n);
    if (is(n, "br")) { line_break(b, st.scale); return; }
    if (is(n, "input") || is(n, "select") || is(n, "textarea") || is(n, "button")) { field(b, node, st, &box); return; }
    if (is(n, "img")) { image(b, node, st, &box); return; }
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
    int link = b->link, label = b->label;
    if (is(n, "a") && *dom_attr(n, "href") && b->v->link_count < VIEW_LINKS_MAX) {
        char target[BROWSER_URL_MAX];
        if (browser_url_resolve(b->base, dom_attr(n, "href"), target, sizeof(target)) == 0) {
            int at = store_string(b, target);
            if (at >= 0) { b->v->links[b->v->link_count].url = at; b->link = b->v->link_count++; }
        }
    }
    if (is(n, "label")) {
        int target = find_id(dom, dom_attr(n, "for"));
        int c = target >= 0 ? b->control_of[target] : first_control(b, node, 0);
        if (c >= 0) b->label = c;
    }
    int inline_block = box.display == DISPLAY_INLINE_BLOCK && holds_blocks(b, node, st);
    if (block_level(n, st, &box) || inline_block) {
        flush_line(b);
        int x = b->line->x, width = b->line->width;
        if (inline_block && !box.width) box.width = (short)shrink_width(b, node, st, &box);
        block(b, node, st, &box, x, width);
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
        children(b, node, st);
        add_gap(b, pr, st.background, st);
        add_gap(b, mr, parent.background, st);
        b->pad = pad;
    }
    b->link = link; b->label = label;
}

/* ---- tables ---- */

#define COLUMNS_MAX 16
typedef struct { int node, column, span; } cell;

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
    browser_box box;
    browser_style cs = css_compute_box(&b->doc->css, b->dom, node, st, &box);
    if (cs.hidden || box.hide || box.display == DISPLAY_NONE) return;
    if (is(n, "br")) { *line = 0; return; }
    int h, fixed = -1;
    if (is(n, "img")) fixed = image_size(b, node, &box, 0, &h);
    else if (is(n, "input") || is(n, "select") || is(n, "button") || is(n, "textarea")) fixed = field_size(b, node, &box, 0, &h);
    if (fixed >= 0) {
        if (fixed > *minimum) *minimum = (float)fixed;
        *line += (float)fixed;
        if (*line > *maximum) *maximum = *line;
        return;
    }
    if (block_level(n, cs, &box) || (box.display == DISPLAY_INLINE_BLOCK && holds_blocks(b, node, cs))) {
        /* a block: its lines, plus its margins, borders and padding */
        default_box(n, &box);
        float lo = 0, hi = 0, inner = 0;
        for (int c = n->first; c >= 0; c = b->dom->nodes[c].next) content_widths(b, c, cs, &lo, &hi, &inner, depth + 1);
        int margins = (box.margin[1] == BOX_AUTO ? 0 : clampi(box.margin[1], 0, 200)) + (box.margin[3] == BOX_AUTO ? 0 : clampi(box.margin[3], 0, 200));
        int sides = box.padding[1] + box.padding[3] + box.border[1] + box.border[3];
        if (box.width > 0) lo = hi = (float)(box.width + margins);
        else {
            lo += margins + sides; hi += margins + sides;
            if (box.max_width > 0 && hi > box.max_width + margins) hi = (float)(box.max_width + margins);
        }
        if (lo > *minimum) *minimum = lo;
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
            children(b, c, cs);
            flush_line(b);
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
    if (!b || !v->items || !v->links || !v->controls || !v->options || !v->forms || !v->anchors || !control_of) {
        free(b); free(control_of); browser_view_free(v);
        snprintf(err, errlen, "Not enough memory to lay out the page.");
        return -1;
    }
    v->width = width;
    v->paper = 0xffffffff;
    v->ink = 0xff000000;
    b->v = v; b->doc = doc; b->dom = doc->dom; b->measure = measure; b->control_of = control_of;
    b->link = b->label = -1; b->scripting = doc->scripting;
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
    browser_style initial = { .color = 0xff000000, .link = DEFAULT_LINK, .scale = 0.64f, .pre = !doc->dom->html };
    if (!b->failed) {
        if (doc->dom->html) children(b, 0, initial);
        else {
            for (int i = 0; i < doc->dom->count; i++) if (!strcmp(doc->dom->nodes[i].tag, "#text")) add_text(b, doc->dom->nodes[i].text, initial);
        }
        flush_line(b);
    }
    int failed = b->failed;
    free(control_of); free(b);
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

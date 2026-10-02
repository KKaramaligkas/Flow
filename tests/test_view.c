/* Page view: layout, forms, hit testing and culling, with a fixed-width font. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/document.h"
#include "../src/view.h"
static int checks, failures;
#define CHECK(test) do{checks++;if(!(test)){failures++;fprintf(stderr,"%d: %s\n",__LINE__,#test);}}while(0)
#define SAME(a,b) CHECK(strcmp((a),(b))==0)

/* 6 pixels per character at the default size, like the 8x8 fallback font. */
static float measure(const char *s, int len, float scale, int flags)
{
    (void)flags;
    int n = 0;
    for (int i = 0; i < len; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n * 6 * scale / 0.64f;
}

typedef struct { browser_document doc; browser_view view; } page;
static page *load(const char *html, const char *url, int scripting)
{
    page *p = calloc(1, sizeof(*p));
    char err[256];
    CHECK(browser_document_parse(&p->doc, html, strlen(html), url, "text/html", err, sizeof(err)) == 0);
    if (scripting) { p->doc.scripting = 1; CHECK(browser_document_render(&p->doc, err, sizeof(err)) == 0); }
    CHECK(browser_view_build(&p->view, &p->doc, VIEW_WIDTH, measure, err, sizeof(err)) == 0);
    return p;
}
static void drop(page *p) { browser_view_free(&p->view); browser_document_free(&p->doc); free(p); }

static const view_item *find_text(const browser_view *v, const char *text)
{
    for (int i = 0; i < v->item_count; i++)
        if (v->items[i].kind == ITEM_TEXT && v->items[i].length >= (int)strlen(text) &&
            !strncmp(v->text + v->items[i].text, text, strlen(text))) return &v->items[i];
    return NULL;
}
static int count_kind(const browser_view *v, int kind)
{
    int n = 0;
    for (int i = 0; i < v->item_count; i++) n += v->items[i].kind == kind;
    return n;
}
static void sane(const browser_view *v)
{
    for (int i = 0; i < v->item_count; i++) {
        const view_item *it = &v->items[i];
        CHECK(it->x >= 0 && it->x + it->w <= v->width + 1);
        CHECK(it->y >= 0 && it->h >= 0 && it->y + it->h <= v->height);
        if (it->kind == ITEM_TEXT) CHECK(it->text >= 0 && (size_t)(it->text + it->length) <= v->text_used);
        CHECK(it->link < v->link_count && it->control < v->control_count);
    }
}

static void flow(void)
{
    page *p = load("<title>T</title><h1>Heading</h1><p>one two three four five six seven eight nine ten eleven twelve "
                   "thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty twentyone twentytwo</p>"
                   "<p>Read <a href='next?x=1'>the next page</a> now.</p>", "https://example.org/a/b", 0);
    browser_view *v = &p->view;
    sane(v);
    const view_item *h = find_text(v, "Heading"), *one = find_text(v, "one two");
    CHECK(h && one && h->y < one->y && h->scale > one->scale && (h->flags & CSS_BOLD));
    /* The long paragraph wraps into several lines inside the page. */
    int lines = 0;
    for (int i = 0; i < v->item_count; i++) if (v->items[i].kind == ITEM_TEXT && v->items[i].y >= one->y && v->items[i].y < one->y + 60) lines++;
    CHECK(lines >= 2);
    CHECK(v->link_count == 1);
    SAME(view_string(v, v->links[0].url), "https://example.org/a/next?x=1");
    const view_item *a = find_text(v, "the next page");
    CHECK(a && a->link == 0 && (a->flags & CSS_UNDERLINE) && a->color == 0xffee0000);
    const view_item *read = find_text(v, "Read ");
    CHECK(read && read->link < 0 && read->y == a->y && read->x + read->w <= a->x + 1);
    int link, control;
    CHECK(browser_view_hit(v, a->x + 2, a->y + 2, &link, &control) >= 0 && link == 0 && control < 0);
    CHECK(browser_view_hit(v, 2, v->height - 2, &link, &control) < 0 && link < 0);
    CHECK(v->paper == 0xffffffff);
    drop(p);
    /* A word longer than the line breaks anywhere. */
    char html[1200] = "<p>";
    for (int i = 0; i < 150; i++) strcat(html, "x");
    strcat(html, "</p>");
    p = load(html, "https://example.org/", 0);
    sane(&p->view);
    CHECK(count_kind(&p->view, ITEM_TEXT) >= 2);
    drop(p);
    /* Text joined to an element ("bb<b>cc</b>dd") breaks only at a space:
       the whole word moves to the next line. */
    p = load("<p style='width:100px'>aaaa bb<b>cc</b>dd</p><p style='width:100px'>xxxxxxxx<b>yyyy</b></p>", "https://example.org/", 0);
    v = &p->view;
    sane(v);
    const view_item *aaaa = find_text(v, "aaaa"), *bb = find_text(v, "bb"), *cc = find_text(v, "cc"), *dd = find_text(v, "dd");
    CHECK(aaaa && bb && cc && dd);
    if (aaaa && bb && cc && dd) {
        CHECK(aaaa->length == 4 && bb->y > aaaa->y && bb->x == aaaa->x);
        CHECK(cc->y == bb->y && dd->y == bb->y && cc->x == bb->x + 12 && dd->x == cc->x + 12 && (cc->flags & CSS_BOLD));
    }
    /* A page with more pieces than the view holds is cut off, not lost. */
    drop(p);
    size_t size = 20000 * 2 + 32;
    char *long_page = malloc(size);
    strcpy(long_page, "<pre>");
    for (int i = 0; i < 20000; i++) strcat(long_page + 5 + i * 2, "x\n");
    strcat(long_page, "</pre>");
    p = load(long_page, "https://example.org/", 0);
    v = &p->view;
    sane(v);
    CHECK(v->truncated && v->item_count == VIEW_ITEMS_MAX && v->item_capacity == VIEW_ITEMS_MAX);
    free(long_page);
    drop(p);
    p = load("<p style='width:100px'>aaaa bb<b>cc</b>dd</p><p style='width:100px'>xxxxxxxx<b>yyyy</b></p>", "https://example.org/", 0);
    v = &p->view;
    /* ...and where the line has no space at all, between the elements. */
    const view_item *xs = find_text(v, "xxxxxxxx"), *ys = find_text(v, "yyyy");
    CHECK(xs && ys && ys->y > xs->y && ys->x == xs->x);
    drop(p);
}

static void boxes(void)
{
    page *p = load("<body style='background:#102030;color:white'><div style='background:red;padding:10px;border:2px solid blue;margin:5px'>Box</div>"
                   "<ul><li>first</li><li>second</li></ul><ol start=3><li>three</li></ol><hr>"
                   "<p style='display:none'>secret</p><span class=sr style='position:absolute;width:1px;height:1px;overflow:hidden'>skip</span>"
                   "<p style='text-align:center'>mid</p><pre>a  b\nc</pre></body>", "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    CHECK(v->paper == 0xff302010);
    CHECK(!find_text(v, "secret") && !find_text(v, "skip"));
    const view_item *box = find_text(v, "Box");
    CHECK(box && box->color == 0xffffffff);
    int red = 0, bullets = count_kind(v, ITEM_BULLET);
    for (int i = 0; i < v->item_count; i++) if (v->items[i].kind == ITEM_RECT && v->items[i].color == 0xff0000ff && v->items[i].h > 15) red = 1;
    CHECK(red && bullets == 2);
    CHECK(find_text(v, "3."));
    const view_item *mid = find_text(v, "mid");
    CHECK(mid && mid->x > 150);
    const view_item *first = find_text(v, "first"), *second = find_text(v, "second");
    CHECK(first && second && second->y > first->y && first->x >= 18);
    CHECK(find_text(v, "a  b") && find_text(v, "c"));
    drop(p);
    /* An inline-block holding blocks (a table of contents) is a box as wide
       as its content, with its border and background around all of it. */
    p = load("<div style='display:inline-block;background:#f8f9fa;border:1px solid #a2a9b1;padding:7px'><b>Contents</b>"
             "<ol><li><a href='#h'>History</a></li><li>Hardware</li></ol></div><p>after</p>"
             "<div style='text-align:center'><div style='display:inline-block;background:#ff0000'><p>mid</p></div></div>",
             "https://example.org/", 0);
    v = &p->view;
    sane(v);
    const view_item *contents = find_text(v, "Contents"), *history = find_text(v, "History"), *hardware = find_text(v, "Hardware");
    const view_item *after = find_text(v, "after"), *panel = NULL;
    for (int i = 0; i < v->item_count; i++) if (v->items[i].kind == ITEM_RECT && v->items[i].color == 0xfffaf9f8) panel = &v->items[i];
    CHECK(contents && history && hardware && after && panel);
    if (contents && history && hardware && after && panel) {
        CHECK(panel->x <= contents->x && panel->y <= contents->y && panel->y + panel->h >= hardware->y + hardware->h);
        CHECK(history->x + history->w <= panel->x + panel->w && history->x >= panel->x + 18);
        CHECK(panel->w < 160 && after->y >= panel->y + panel->h);
    }
    int frame = 0;
    for (int i = 0; i < v->item_count; i++) frame += v->items[i].kind == ITEM_RECT && v->items[i].color == 0xffb1a9a2;
    CHECK(frame == 4);
    mid = find_text(v, "mid");
    CHECK(mid && mid->x > 200 && mid->x < 260);
    drop(p);
    /* List indents count in a table column's width. */
    p = load("<table><tr><td><ul><li>item one</li></ul></td><td>x</td></tr></table>", "https://example.org/", 0);
    v = &p->view;
    const view_item *item_one = find_text(v, "item one");
    CHECK(item_one && item_one->length == 8);
    drop(p);
}

static void tables(void)
{
    page *p = load("<table width=100% border=1><tr><td>left cell text</td><td>right</td></tr>"
                   "<tr><td colspan=2>wide</td></tr></table><p>after</p>", "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    const view_item *left = find_text(v, "left"), *right = find_text(v, "right"), *wide = find_text(v, "wide"), *after = find_text(v, "after");
    CHECK(left && right && wide && after);
    if (left && right && wide && after) {
        CHECK(right->x > left->x + left->w && right->y == left->y);
        CHECK(wide->y > left->y && after->y > wide->y);
    }
    CHECK(count_kind(v, ITEM_FRAME) == 3);
    drop(p);
    /* Columns are as wide as their widest line, rounded up to whole pixels,
       with the inline padding of links: nothing wraps. */
    p = load("<table><tr><td valign=top>1.</td><td style='font-size:13px'>A description for <b>psp homebrew</b>.</td></tr>"
             "<tr><td></td><td><a href=a style='padding:0 15px'>About</a><a href=b style='padding:0 15px'>Ads</a></td></tr></table>",
             "https://example.org/", 0);
    v = &p->view;
    sane(v);
    const view_item *text = find_text(v, "A description"), *bold = find_text(v, "psp homebrew"), *dot = find_text(v, ".");
    CHECK(text && bold && dot && bold->y == text->y && dot->y == text->y && dot->x >= bold->x + bold->w - 1);
    const view_item *about = find_text(v, "About"), *ads = find_text(v, "Ads");
    CHECK(about && ads && ads->y == about->y && ads->x >= about->x + about->w + 18);
    drop(p);
}

static void forms(void)
{
    /* Like google.com: a GET form whose search box is a one-row textarea. */
    page *p = load("<form action='/search' role=search><input type=hidden name=hl value=el>"
                   "<textarea name=q rows=1 title='Search'></textarea><input type=submit name=btnK value='Google Search'>"
                   "<input type=submit name=btnI value='Lucky'></form>", "https://www.google.com/", 0);
    browser_view *v = &p->view;
    sane(v);
    CHECK(v->form_count == 1 && v->control_count == 4);
    int q = -1, search = -1;
    for (int i = 0; i < v->control_count; i++) {
        if (!strcmp(view_string(v, v->controls[i].name), "q")) q = i;
        if (!strcmp(view_string(v, v->controls[i].name), "btnK")) search = i;
    }
    CHECK(q >= 0 && search >= 0 && v->controls[q].item >= 0 && v->controls[search].item >= 0);
    CHECK(browser_view_submits_on_enter(v, q));
    CHECK(browser_view_set_value(v, q, "psp homebrew & more") == 0);
    char url[BROWSER_URL_MAX], err[256], *body;
    CHECK(browser_view_submit(v, 0, search, url, sizeof(url), &body, err, sizeof(err)) == 0 && !body);
    SAME(url, "https://www.google.com/search?hl=el&q=psp+homebrew+%26+more&btnK=Google+Search");
    const view_item *field = &v->items[v->controls[q].item];
    int link, control;
    CHECK(browser_view_hit(v, field->x + 3, field->y + 3, &link, &control) >= 0 && control == q);
    drop(p);

    p = load("<form method=post action='login?next=/'><label for=u>User</label><input id=u name=user>"
             "<input type=password name=pass><label><input type=checkbox name=keep value=yes> Keep me</label>"
             "<input type=radio name=r value=a checked><input type=radio name=r value=b>"
             "<select name=lang><option value=en>English<option selected>Ελληνικά</select>"
             "<input name=off disabled value=x><button>Sign in</button><button type=reset>Clear</button></form>", "https://example.org/a/", 0);
    v = &p->view;
    sane(v);
    CHECK(v->form_count == 1 && v->forms[0].post && v->forms[0].fields == 2);
    CHECK(!browser_view_submits_on_enter(v, 0));
    const view_item *user = find_text(v, "User");
    CHECK(user && user->control == 0);
    browser_view_set_value(v, 0, "kostas");
    browser_view_set_value(v, 1, "p@ss word");
    browser_view_toggle(v, 2);
    browser_view_toggle(v, 4);
    int submit = -1;
    for (int i = 0; i < v->control_count; i++) if (v->controls[i].kind == CONTROL_SUBMIT) submit = i;
    CHECK(submit >= 0 && !strcmp(view_string(v, v->controls[submit].label), "Sign in"));
    CHECK(browser_view_submit(v, 0, submit, url, sizeof(url), &body, err, sizeof(err)) == 0 && body);
    SAME(url, "https://example.org/a/login?next=/");
    if (body) SAME(body, "user=kostas&pass=p%40ss+word&keep=yes&r=b&lang=%CE%95%CE%BB%CE%BB%CE%B7%CE%BD%CE%B9%CE%BA%CE%AC");
    free(body);
    browser_view_reset(v, 0);
    CHECK(!v->controls[2].checked && v->controls[3].checked && !*v->controls[0].value);
    drop(p);

    /* Typing a comment doesn't send a form whose one-line field is the name;
       a form with only a password field is sent once it's typed. */
    p = load("<form method=post action=/comment><input name=name><textarea name=text rows=4></textarea><input type=submit></form>"
             "<form method=post action=/unlock><input type=password name=pin></form>", "https://example.org/", 0);
    v = &p->view;
    CHECK(v->control_count == 4 && v->forms[0].fields == 1);
    CHECK(browser_view_submits_on_enter(v, 0) && !browser_view_submits_on_enter(v, 1) && !browser_view_submits_on_enter(v, 2));
    CHECK(browser_view_submits_on_enter(v, 3));
    drop(p);

    /* With JavaScript on, <noscript> and its styles are skipped, as Google's would hide the form. */
    p = load("<noscript><style>table,div,span,p{display:none}</style><p>Enable JavaScript</p></noscript>"
             "<div><form action=/search><input name=q></form></div>", "https://www.google.com/", 1);
    v = &p->view;
    CHECK(v->control_count == 1 && v->controls[0].item >= 0 && !find_text(v, "Enable JavaScript"));
    drop(p);
    p = load("<noscript><p>Enable JavaScript</p></noscript>", "https://www.google.com/", 0);
    CHECK(find_text(&p->view, "Enable JavaScript") != NULL);
    drop(p);
}

static void navigation(void)
{
    char html[8192] = "<p><a href=/1>one</a> <a href=/2>two</a></p>";
    for (int i = 0; i < 60; i++) strcat(html, "<p>filler text line</p>");
    strcat(html, "<h2 id=end>End</h2><p><a href=/3>three</a> <input name=x></p>");
    page *p = load(html, "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    CHECK(v->height > 600);
    int first = browser_view_step(v, -1, 0, 0, 1);
    CHECK(first >= 0 && v->items[first].link == 0);
    int second = browser_view_step(v, first, 0, 0, 1);
    CHECK(second >= 0 && v->items[second].link == 1);
    CHECK(browser_view_step(v, second, 0, 0, -1) == first);
    int last = browser_view_step(v, -1, 0, v->height, -1);
    CHECK(last >= 0 && v->items[last].control == 0);
    int three = browser_view_step(v, last, 0, 0, -1);
    CHECK(three >= 0 && v->items[three].link == 2);
    CHECK(browser_view_step(v, last, 0, 0, 1) < 0);
    int end = browser_view_anchor(v, "end");
    const view_item *e = find_text(v, "End");
    CHECK(end > 500 && e && end <= e->y);
    CHECK(browser_view_anchor(v, "missing") < 0);
    /* Culling finds exactly the items that intersect each band. */
    for (int top = -50; top < v->height; top += 37) {
        int seen = 0, expected = 0;
        for (int i = browser_view_first(v, top); browser_view_more(v, i, top + 120); i++)
            if (v->items[i].y < top + 120 && v->items[i].y + v->items[i].h > top) seen++;
        for (int i = 0; i < v->item_count; i++) if (v->items[i].y < top + 120 && v->items[i].y + v->items[i].h > top) expected++;
        CHECK(seen == expected);
    }
    drop(p);
}

static void plain_text(void)
{
    page *p = calloc(1, sizeof(*p));
    char err[256];
    const char *text = "line one\nline two < & >";
    CHECK(browser_document_parse(&p->doc, text, strlen(text), "https://example.org/a.txt", "text/plain", err, sizeof(err)) == 0);
    CHECK(browser_view_build(&p->view, &p->doc, VIEW_WIDTH, measure, err, sizeof(err)) == 0);
    CHECK(find_text(&p->view, "line one") && find_text(&p->view, "line two < & >"));
    drop(p);
}

int main(void)
{
    flow(); boxes(); tables(); forms(); navigation(); plain_text();
    printf("view: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

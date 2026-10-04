/* Page view: layout, forms, hit testing and culling, with a fixed-width font. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/document.h"
#include "../src/view.h"
#include "../src/picture.h"
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

/* What real sites need to stay readable without their scripts. */
static void readability(void)
{
    page *p = load("<p><a href=/a>Sign in</a><a href=/b>Sign up</a> and<a href=/c>more</a></p><p><a href=/x>Home</a><input type=submit value=Go></p>"
                   "<form><button aria-label='Open menu' style='width:20px'><svg></svg></button><button type=button><svg></svg></button><button><img alt=Search></button></form>"
                   "<p><a href='https://github.com/x' aria-label=GitHub><svg></svg></a> <a href=/home title=Home><img src=logo.png></a>"
                   " <a href=/blank><img src=x.png></a><a style='display:block' href=/d aria-label=Docs><svg></svg></a></p>", "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    const view_item *in = find_text(v, "Sign in"), *up = find_text(v, "Sign up"), *and_text = find_text(v, " and"), *more = find_text(v, "more");
    CHECK(in && up && in->y == up->y && up->x >= in->x + in->w + 5);
    CHECK(and_text && more && more->x <= and_text->x + and_text->w + 1);   /* text joined to a link stays joined */
    const view_item *home = find_text(v, "Home");
    int go = -1, menu = -1, script_button = -1, search = -1;
    for (int i = 0; i < v->control_count; i++) {
        const char *label = view_string(v, v->controls[i].label);
        if (!strcmp(label, "Go")) go = i; else if (!strcmp(label, "Open menu")) menu = i; else if (!strcmp(label, "Search")) search = i;
        else if (v->controls[i].kind == CONTROL_BUTTON) script_button = i;
    }
    CHECK(home && go >= 0 && v->controls[go].item >= 0 && v->items[v->controls[go].item].x >= home->x + home->w + 3);
    CHECK(menu >= 0 && v->controls[menu].item >= 0 && search >= 0 && v->controls[search].item >= 0);
    if (menu >= 0 && v->controls[menu].item >= 0) CHECK(v->items[v->controls[menu].item].w >= 50);   /* not the 12 pixels its CSS gives the icon */
    CHECK(script_button >= 0 && v->controls[script_button].item < 0);
    const view_item *github = find_text(v, "GitHub"), *home_logo = NULL, *docs = find_text(v, "Docs");
    for (int i = 0; i < v->item_count; i++) if (v->items[i].kind == ITEM_TEXT && v->items[i].length == 4 && !strncmp(v->text + v->items[i].text, "Home", 4) && v->items[i].y != home->y) home_logo = &v->items[i];
    CHECK(github && github->link >= 0 && !strcmp(view_string(v, v->links[github->link].url), "https://github.com/x"));
    CHECK(home_logo && home_logo->link >= 0 && docs && docs->link >= 0);
    drop(p);
    /* A closed <details> shows its summary; decorative pictures leave no box. */
    p = load("<details><summary>More</summary><p>hidden text</p></details><details open><summary>Open</summary><p>shown text</p></details>"
             "<p><img src=a.png alt='' width=200 height=100><img src=i.png width=16 height=16><img src=b.png alt=Chart width=200 height=100>"
             "<img src=c.png width=300 height=200></p>", "https://example.org/", 0);
    v = &p->view;
    CHECK(find_text(v, "More") && !find_text(v, "hidden text") && find_text(v, "Open") && find_text(v, "shown text"));
    CHECK(count_kind(v, ITEM_IMAGE) == 2);
    drop(p);
    /* A button the page styles as text is drawn as its text. */
    p = load("<style>.t{border:none;background:none;color:#cc0000}</style><p><button class=t type=button>createServer</button><button type=button>Plain</button>"
             "<button class=t type=button aria-label=Chat style='font-size:24px'><svg></svg></button> <a href=/g aria-label=GitHub style='font-size:30px'><svg></svg></a></p>", "https://example.org/", 0);
    v = &p->view;
    int token = -1, normal = -1, chat = -1;
    for (int i = 0; i < v->control_count; i++) {
        const char *label = view_string(v, v->controls[i].label);
        if (!strcmp(label, "createServer")) token = i; else if (!strcmp(label, "Plain")) normal = i; else if (!strcmp(label, "Chat")) chat = i;
    }
    CHECK(token >= 0 && normal >= 0 && v->controls[token].item >= 0 && v->controls[normal].item >= 0);
    if (token >= 0 && normal >= 0 && v->controls[token].item >= 0 && v->controls[normal].item >= 0) {
        const view_item *a = &v->items[v->controls[token].item], *b = &v->items[v->controls[normal].item];
        CHECK((a->flags & CONTROL_PLAIN) && !(b->flags & CONTROL_PLAIN) && a->color == 0xff0000cc && a->h <= 11);
    }
    /* An icon's name is body text, whatever size the icon's font made it. */
    CHECK(chat >= 0 && v->controls[chat].item >= 0 && v->items[v->controls[chat].item].scale <= 0.641f);
    const view_item *named = find_text(v, "GitHub");
    CHECK(named && named->scale <= 0.641f);
    drop(p);
    /* ...and runs past the box sized for the icon rather than break up. */
    p = load("<div style='width:40px'><a href=/ aria-label='Atlassian logo' style='display:block'><svg></svg></a></div>", "https://example.org/", 0);
    v = &p->view;
    CHECK(find_text(v, "Atlassian logo") != NULL);
    drop(p);
    /* Text that would vanish into what is behind it: white text on a
       picture we don't draw, transparent text over a gradient. A gradient
       shows as a colour; colours that show are kept. */
    p = load("<style>.hero{background:url(hero.png) center/cover;padding:20px}.hero h1{color:#fff}.dark{background:#101010;color:#111}"
             ".band{background:linear-gradient(90deg,#1d63ed 0%,#0b214a 100%);color:#fff}.fade{background-image:radial-gradient(circle,rgba(0,0,0,.1),transparent)}"
             ".clip{background:linear-gradient(#f00,#00f);-webkit-background-clip:text;color:transparent}</style>"
             "<div class=hero><h1>Isolated</h1><a href=/x style='color:#fff'>Explore</a></div><div class=dark>Night<ul><li>Item</li></ul></div>"
             "<div class=band>Banner</div><div class=fade><span style='color:#999'>Grey</span></div><p><span class=clip>Rainbow</span></p>"
             "<table><tr><td style='background:#000;color:#fff'>Cell</td><td style='color:#fefefe'>Pale</td></tr></table>", "https://example.org/", 0);
    v = &p->view;
    const view_item *isolated = find_text(v, "Isolated"), *explore = find_text(v, "Explore"), *night = find_text(v, "Night"), *item = find_text(v, "Item");
    const view_item *banner = find_text(v, "Banner"), *grey = find_text(v, "Grey"), *rainbow = find_text(v, "Rainbow"), *cell = find_text(v, "Cell"), *pale = find_text(v, "Pale");
    CHECK(isolated && isolated->color == 0xff202020);
    CHECK(explore && explore->color == 0xffee0000);     /* still a link */
    CHECK(night && night->color == 0xffe8e8e8 && item && item->color == 0xffe8e8e8);
    int band = 0, bullet = 0, behind_rainbow = 0;
    for (int i = 0; i < v->item_count; i++) {
        if (v->items[i].kind == ITEM_RECT && rainbow && v->items[i].y == rainbow->y) behind_rainbow = 1;
        if (v->items[i].kind == ITEM_RECT && banner && v->items[i].y <= banner->y && v->items[i].y + v->items[i].h >= banner->y + banner->h &&
            v->items[i].color == 0xff9b4214) band = 1;   /* the average of #1d63ed and #0b214a */
        if (v->items[i].kind == ITEM_BULLET && item && v->items[i].y >= item->y && v->items[i].y < item->y + item->h) bullet = v->items[i].color == 0xffe8e8e8;
    }
    CHECK(band && banner && banner->color == 0xffffffff);
    CHECK(bullet);
    CHECK(grey && grey->color == 0xff999999);
    CHECK(rainbow && rainbow->color == 0xff7f007f && !behind_rainbow);   /* the gradient's colour, without a box */
    CHECK(cell && cell->color == 0xffffffff && pale && pale->color == 0xff202020);
    drop(p);
}

static const view_item *find_rect(const browser_view *v, uint32_t color)
{
    for (int i = 0; i < v->item_count; i++) if (v->items[i].kind == ITEM_RECT && v->items[i].color == color) return &v->items[i];
    return NULL;
}

static void flex_and_grid(void)
{
    /* flex: 1 shares the room equally, however long the items' words */
    {
        page *q = load("<style>.r{display:flex;gap:12px}.r div{flex:1;padding:8px;border:1px solid #ccc}</style>"
                       "<div class=r><div>A</div><div>B</div><div>Card three has longer words</div></div>", "https://example.org/", 0);
        const view_item *a = find_text(&q->view, "A"), *c = find_text(&q->view, "Card");
        int widths[3], k = 0;
        for (int i = 0; i < q->view.item_count && k < 3; i++)
            if (q->view.items[i].kind == ITEM_RECT && q->view.items[i].h == 1 && q->view.items[i].w > 20 && q->view.items[i].color == 0xffcccccc &&
                (!k || q->view.items[i].x != q->view.items[i - 1].x)) widths[k++] = q->view.items[i].w;
        CHECK(a && c && a->y == c->y && k == 3);
        CHECK(k == 3 && abs(widths[0] - widths[1]) <= 1 && abs(widths[1] - widths[2]) <= 1 && widths[0] > 100);
        drop(q);
    }
    /* a navigation bar: the links side by side */
    page *p = load("<nav style='display:flex;gap:10px'><a href=/a>Home</a><a href=/b>About</a><a href=/c>Contact</a></nav><p>after</p>",
                   "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    const view_item *home = find_text(v, "Home"), *about = find_text(v, "About"), *contact = find_text(v, "Contact"), *after = find_text(v, "after");
    CHECK(home && about && contact && after);
    if (home && about && contact && after) {
        CHECK(home->y == about->y && about->y == contact->y);
        CHECK(about->x >= home->x + home->w + 6 && contact->x >= about->x + about->w + 6);
        CHECK(after->y > home->y + home->h - 1);
    }
    drop(p);
    /* space-between: the first item at the left, the last at the right */
    p = load("<div style='display:flex;justify-content:space-between'><span>left</span><span>middle</span><span>right</span></div>", "https://example.org/", 0);
    v = &p->view;
    const view_item *left = find_text(v, "left"), *right = find_text(v, "right"), *middle = find_text(v, "middle");
    CHECK(left && right && middle && left->x <= 6 && right->x + right->w >= VIEW_WIDTH - 6 && middle->x > 150 && middle->x < 300);
    drop(p);
    /* flex: 1 shares the row equally; a gap between */
    p = load("<div style='display:flex;gap:20px'><div style='flex:1;background:#ff0000'>one</div><div style='flex:1;background:#00ff00'>two</div></div>",
             "https://example.org/", 0);
    v = &p->view;
    const view_item *red = find_rect(v, 0xff0000ff), *green = find_rect(v, 0xff00ff00);
    CHECK(red && green);
    if (red && green) {
        CHECK(red->y == green->y && abs(red->w - green->w) <= 1);
        CHECK(green->x - (red->x + red->w) >= 11 && green->x - (red->x + red->w) <= 13);    /* 20 CSS pixels */
        CHECK(red->w + green->w + 12 >= VIEW_WIDTH - 10);
    }
    drop(p);
    /* margin-left: auto pushes an item to the right; order moves one first */
    p = load("<header style='display:flex'><b>Logo</b><a href=/in style='margin-left:auto'>Sign in</a><i style='order:-1'>menu</i></header>",
             "https://example.org/", 0);
    v = &p->view;
    const view_item *logo = find_text(v, "Logo"), *sign = find_text(v, "Sign in"), *menu = find_text(v, "menu");
    CHECK(logo && sign && menu && sign->x + sign->w >= VIEW_WIDTH - 6 && menu->x < logo->x && menu->y == logo->y);
    drop(p);
    /* a column centered by align-items */
    p = load("<div style='display:flex;flex-direction:column;align-items:center;gap:8px'><div>top</div><div>bottom</div></div>", "https://example.org/", 0);
    v = &p->view;
    const view_item *top = find_text(v, "top"), *bottom = find_text(v, "bottom");
    CHECK(top && bottom && top->x > 200 && bottom->x > 180 && bottom->y >= top->y + top->h + 4);
    drop(p);
    /* cards in a row are stretched to the same height; centered items sit in the middle */
    p = load("<div style='display:flex'><div style='flex:1;background:#ff0000'>short</div>"
             "<div style='flex:1;background:#00ff00'>a much longer card text that wraps onto several lines in this narrow column of the row</div></div>"
             "<div style='display:flex;align-items:center'><div style='width:100px'>mid</div><div style='width:100px'>one two three four five six seven eight nine ten</div></div>",
             "https://example.org/", 0);
    v = &p->view;
    red = find_rect(v, 0xff0000ff); green = find_rect(v, 0xff00ff00);
    CHECK(red && green && red->h == green->h && red->h > 30);
    const view_item *mid = find_text(v, "mid"), *one = find_text(v, "one two");
    CHECK(mid && one && mid->y > one->y + 10);
    drop(p);
    /* items that don't fit even at their narrowest wrap, though nowrap: the screen doesn't scroll sideways */
    p = load("<ul style='display:flex;list-style:none'><li>Documentation</li><li>Downloads</li><li>Community</li><li>Contributing</li>"
             "<li>Security</li><li>Certification</li><li>Organization</li><li>Sponsorship</li></ul>", "https://example.org/", 0);
    v = &p->view;
    sane(v);
    const view_item *docs = find_text(v, "Documentation"), *sponsor = find_text(v, "Sponsorship");
    CHECK(docs && sponsor && sponsor->y > docs->y);
    drop(p);
    /* flex: 0 0 200px is fixed; the other item takes the rest */
    p = load("<div style='display:flex'><div style='flex:0 0 200px;background:#ff0000'>side</div><div style='flex:1;background:#00ff00'>main</div></div>",
             "https://example.org/", 0);
    v = &p->view;
    red = find_rect(v, 0xff0000ff); green = find_rect(v, 0xff00ff00);
    CHECK(red && green && red->w == 120 && green->x == red->x + 120 && green->x + green->w >= VIEW_WIDTH - 6);
    drop(p);
    /* a grid of three columns: six items in two rows */
    p = load("<div style='display:grid;grid-template-columns:repeat(3,1fr);gap:10px'>"
             "<div>c1</div><div>c2</div><div>c3</div><div>c4</div><div>c5</div><div>c6</div></div>", "https://example.org/", 0);
    v = &p->view;
    const view_item *c1 = find_text(v, "c1"), *c2 = find_text(v, "c2"), *c3 = find_text(v, "c3"), *c4 = find_text(v, "c4");
    CHECK(c1 && c2 && c3 && c4);
    if (c1 && c2 && c3 && c4) {
        CHECK(c1->y == c2->y && c2->y == c3->y && c4->y > c1->y && c4->x == c1->x);
        CHECK(c2->x - c1->x >= 150 && c2->x - c1->x <= 160 && abs((c3->x - c2->x) - (c2->x - c1->x)) <= 1);
    }
    drop(p);
    /* auto-fill columns at least 200 CSS pixels wide, and an item spanning the row */
    p = load("<div style='display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr))'>"
             "<div style='grid-column:1/-1;background:#ff0000'>banner</div><div>a1</div><div>a2</div><div>a3</div><div>a4</div></div>",
             "https://example.org/", 0);
    v = &p->view;
    red = find_rect(v, 0xff0000ff);
    const view_item *a1 = find_text(v, "a1"), *a3 = find_text(v, "a3"), *a4 = find_text(v, "a4");
    CHECK(red && red->w >= VIEW_WIDTH - 10 && a1 && a3 && a4 && a1->y == a3->y && a4->y > a1->y && a1->y > red->y);
    drop(p);
    /* fixed and flexible columns: 100px 1fr */
    p = load("<div style='display:grid;grid-template-columns:100px 1fr'><div>label</div><div>value</div></div>", "https://example.org/", 0);
    v = &p->view;
    const view_item *label = find_text(v, "label"), *value = find_text(v, "value");
    CHECK(label && value && value->y == label->y && value->x == label->x + 60);
    drop(p);
}

static void floats(void)
{
    /* text beside a floated picture, then under it at full width */
    page *p = load("<p><img style='float:left;margin-right:10px' width=100 height=80 alt=pic>"
                   "word word word word word word word word word word word word word word word word word word word word word word word "
                   "word word word word word word word word word word word word word word word word word word word word word word word "
                   "word word word word word word word word word word word word word word word word word word word word word word word "
                   "word word word word word word word word word word word word word word word word word word word word word word word "
                   "word word word word word word word word word word word word word word word word word word word word word word end</p><p>next</p>",
                   "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    const view_item *pic = NULL, *first = find_text(v, "word"), *end = NULL, *next = find_text(v, "next");
    for (int i = 0; i < v->item_count; i++) {
        if (v->items[i].kind == ITEM_IMAGE) pic = &v->items[i];
        if (v->items[i].kind == ITEM_TEXT && memmem(v->text + v->items[i].text, (size_t)v->items[i].length, "end", 3)) end = &v->items[i];
    }
    CHECK(pic && first && end && next);
    if (pic && first && end && next) {
        CHECK(pic->x == 4 && pic->w == 60 && pic->h == 48);
        CHECK(first->x >= pic->x + pic->w + 6 && first->y < pic->y + pic->h);
        int below = 0;
        for (int i = 0; i < v->item_count; i++) {
            const view_item *it = &v->items[i];
            if (it->kind != ITEM_TEXT) continue;
            if (it->y < pic->y + pic->h) CHECK(it->x >= pic->x + pic->w);   /* nothing under the picture */
            else if (it->x < pic->x + pic->w) below = 1;
        }
        CHECK(below && next->y >= end->y + end->h);
    }
    drop(p);
    /* a sidebar floated right: the text stops before it */
    p = load("<div style='float:right;width:30%;background:#ff0000'>side</div><p>main main main main main main main main main main main "
             "main main main main main main main main main main main main main main</p>", "https://example.org/", 0);
    v = &p->view;
    sane(v);
    const view_item *side = find_rect(v, 0xff0000ff);
    CHECK(side && side->x + side->w >= VIEW_WIDTH - 5 && side->w >= 135 && side->w <= 142);
    for (int i = 0; side && i < v->item_count; i++)
        if (v->items[i].kind == ITEM_TEXT && v->items[i].y < side->y + side->h && !strncmp(v->text + v->items[i].text, "main", 4))
            CHECK(v->items[i].x + v->items[i].w <= side->x + 1);
    drop(p);
    /* columns of floats side by side; clear puts what follows under them */
    p = load("<div style='float:left;width:45%;background:#ff0000'>A<br>A<br>A</div><div style='float:left;width:45%;background:#00ff00'>B</div>"
             "<div style='clear:both'>after</div>", "https://example.org/", 0);
    v = &p->view;
    const view_item *a = find_rect(v, 0xff0000ff), *b2 = find_rect(v, 0xff00ff00), *after = find_text(v, "after");
    CHECK(a && b2 && after && a->y == b2->y && b2->x >= a->x + a->w && after->y >= a->y + a->h);
    drop(p);
    /* a block holds the floats in it */
    p = load("<div style='background:#00ff00'><div style='float:left;width:50px;height:100px'>x</div></div><p>next</p>", "https://example.org/", 0);
    v = &p->view;
    const view_item *holder = find_rect(v, 0xff00ff00);
    next = find_text(v, "next");
    CHECK(holder && holder->h >= 60 && next && next->y >= holder->y + holder->h);
    drop(p);
}

/* More nesting, items and floats than the limits, and nonsense values: laid out sanely. */
static void layout_limits(void)
{
    size_t size = 400 * 1024, used = 0;
    char *html = malloc(size);
    CHECK(html != NULL);
    if (!html) return;
    used += (size_t)snprintf(html + used, size - used, "<style>.f{display:flex;flex:1 1 0;gap:3px}.g{display:grid;grid-template-columns:repeat(99,1fr)}"
                             ".l{float:left;width:30px}.r{float:right;width:40%%}.x{flex:garbage;grid-column:9/2;order:abc;min-width:-5px;grid-template-columns:repeat(,)}</style>");
    /* nested within the parser's depth limit */
    for (int i = 0; i < 40; i++) used += (size_t)snprintf(html + used, size - used, "<div class=f>n%d", i);
    for (int i = 0; i < 40; i++) used += (size_t)snprintf(html + used, size - used, "</div>");
    used += (size_t)snprintf(html + used, size - used, "<div class=f>");
    for (int i = 0; i < 200; i++) used += (size_t)snprintf(html + used, size - used, "<span>item%d</span>", i);
    used += (size_t)snprintf(html + used, size - used, "</div><div class=g>");
    for (int i = 0; i < 150; i++) used += (size_t)snprintf(html + used, size - used, "<div class=x>cell%d</div>", i);
    used += (size_t)snprintf(html + used, size - used, "</div><div>");
    for (int i = 0; i < 80; i++) used += (size_t)snprintf(html + used, size - used, "<div class=%s>fl%d</div>text%d ", i % 3 ? "l" : "r", i, i);
    snprintf(html + used, size - used, "</div><p style='clear:both'>end</p>");
    page *p = load(html, "https://example.org/", 0);
    browser_view *v = &p->view;
    sane(v);
    CHECK(find_text(v, "item199") && find_text(v, "cell149") && find_text(v, "fl78") && find_text(v, "end"));
    for (int i = 0; i < 40; i++) {
        char nested[16];
        snprintf(nested, sizeof(nested), "n%d", i);
        CHECK(find_text(v, nested) || i > 20);   /* deep ones are too narrow for whole words */
    }
    drop(p);
    free(html);
}

/* With the page's pictures on: the table the loader fills. */
static page *load_with_pictures(const char *html)
{
    page *p = calloc(1, sizeof(*p));
    char err[256];
    CHECK(browser_document_parse(&p->doc, html, strlen(html), "https://example.org/a/", "text/html", err, sizeof(err)) == 0);
    p->doc.pictures = picture_table_new(4 << 20, p->doc.dom->count);
    CHECK(p->doc.pictures != NULL);
    CHECK(browser_view_build(&p->view, &p->doc, VIEW_WIDTH, measure, err, sizeof(err)) == 0);
    return p;
}
/* Laid out again, as when the loader has learned pictures' sizes. */
static void relayout(page *p)
{
    char err[256];
    browser_view_free(&p->view);
    CHECK(browser_view_build(&p->view, &p->doc, VIEW_WIDTH, measure, err, sizeof(err)) == 0);
}
static int entry(const page *p, const char *url)
{
    const picture_table *t = p->doc.pictures;
    for (int i = 0; i < t->count; i++) if (!strcmp(t->entries[i].url, url)) return i;
    return -1;
}
/* As if the picture at https://example.org/a/<name> was loaded. */
static void arrive(page *p, const char *name, int w, int h)
{
    char url[256];
    snprintf(url, sizeof(url), "https://example.org/a/%s", name);
    int i = entry(p, url);
    CHECK(i >= 0);
    if (i >= 0) { p->doc.pictures->entries[i].natural_w = w; p->doc.pictures->entries[i].natural_h = h; p->doc.pictures->entries[i].state = PICTURE_READY; }
}
static const view_item *picture_item(const page *p, const char *name)
{
    char url[256];
    snprintf(url, sizeof(url), "https://example.org/a/%s", name);
    int i = entry(p, url);
    for (int k = 0; i >= 0 && k < p->view.item_count; k++)
        if (p->view.items[k].kind == ITEM_IMAGE && p->view.items[k].flags == i + 1) return &p->view.items[k];
    return NULL;
}

static void pictures(void)
{
    page *p = load_with_pictures("<p>top</p>"
        "<img src=a.jpg width=200 height=100><img src=d.png alt='' width=100 height=50><img src=u.png><img src=v.png alt='A cat'>"
        "<img src=t.gif width=1 height=1><img src=logo.svg alt=Logo><img src=deco.svg><img src=w.jpg width=300>"
        "<img srcset='h.png 2x'><img src=big.jpg><img src=tall.jpg><img src='my pic \xc3\xa9.jpg' width=10 height=10>"
        "<p><a href=/ title=Home><img src=logo.png></a> <a href=/x title=Xlink><img src=x.svg></a></p>"
        "<div style='display:flex'><img src=f1.jpg width=100 height=100><img src=f2.jpg width=100 height=100></div>"
        "<img src='data:image/png;base64,iVBORw0KGgo=' width=20 height=20><img src=a.jpg width=50 height=25>"
        "<img src=px.gif><img src=icon.png width=16 height=16><div style=display:none><img src=hidden.jpg></div>");
    browser_view *v = &p->view;
    picture_table *t = p->doc.pictures;
    const view_item *it0;
    sane(v);
    CHECK(t->count == 15);
    CHECK(entry(p, "https://example.org/a/a.jpg") == 0 && entry(p, "https://example.org/a/d.png") == 1);
    CHECK(entry(p, "https://example.org/a/t.gif") < 0 && entry(p, "https://example.org/a/logo.svg") < 0 && entry(p, "https://example.org/a/hidden.jpg") < 0);
    CHECK(entry(p, "https://example.org/a/my%20pic%20%C3%A9.jpg") >= 0 && entry(p, "data:image/png;base64,iVBORw0KGgo=") >= 0);
    /* a box the page sizes: the picture is stretched to it */
    const view_item *a = picture_item(p, "a.jpg");
    CHECK(a && a->w == 120 && a->h == 60);
    CHECK(t->entries[0].sized == PICTURE_FIT_STRETCH && t->entries[0].want_w == 120 && t->entries[0].want_h == 60);
    const view_item *d = picture_item(p, "d.png"), *icon = picture_item(p, "icon.png");
    CHECK(d && d->w == 60 && d->h == 30 && icon && icon->w == 10);      /* decoration shows with its picture */
    /* sized by the picture itself: nothing until its size is known, or its description */
    int u = entry(p, "https://example.org/a/u.png");
    CHECK(u >= 0 && !picture_item(p, "u.png") && t->entries[u].sized == PICTURE_FIT_OWN && t->entries[u].want_w == 464 && !t->entries[u].want_h);
    const view_item *cat = picture_item(p, "v.png");
    CHECK(cat && cat->length == 5 && cat->h == 17);
    const view_item *w = picture_item(p, "w.jpg");
    int wi = entry(p, "https://example.org/a/w.jpg");
    CHECK(w && w->w == 180 && w->h == 135 && t->entries[wi].sized == PICTURE_FIT_BOX && t->entries[wi].want_w == 180 && !t->entries[wi].want_h);
    CHECK(t->entries[entry(p, "https://example.org/a/h.png")].density == 2);
    /* what this can't decode is shown as before */
    int logo_box = 0;
    for (int i = 0; i < v->item_count; i++) if (v->items[i].kind == ITEM_IMAGE && !v->items[i].flags && v->items[i].length == 4) logo_box++;
    CHECK(logo_box == 1);
    /* an icon link shows its name until its picture arrives, then the picture */
    int logo = entry(p, "https://example.org/a/logo.png");
    CHECK(find_text(v, "Home") && find_text(v, "Xlink") && logo >= 0 && t->entries[logo].relayout && !t->entries[0].relayout);
    const view_item *f1 = picture_item(p, "f1.jpg"), *f2 = picture_item(p, "f2.jpg");
    CHECK(f1 && f2 && f1->y == f2->y && f2->x >= f1->x + 60);
    CHECK(t->entries[0].top < t->entries[entry(p, "https://example.org/a/f1.jpg")].top);
    /* the sizes arrive: laid out again, with the same entries */
    arrive(p, "u.png", 400, 200); arrive(p, "v.png", 100, 100); arrive(p, "w.jpg", 600, 300); arrive(p, "h.png", 200, 100);
    arrive(p, "big.jpg", 2000, 1000); arrive(p, "tall.jpg", 100, 2000); arrive(p, "px.gif", 1, 1); arrive(p, "logo.png", 100, 40);
    relayout(p);
    v = &p->view;
    sane(v);
    CHECK(t->count == 15);
    CHECK(!find_text(v, "Home") && find_text(v, "Xlink") && (it0 = picture_item(p, "logo.png")) && it0->link >= 0 && it0->w == 60 && it0->h == 24);
    const view_item *it;
    CHECK((it = picture_item(p, "u.png")) && it->w == 240 && it->h == 120);
    CHECK((it = picture_item(p, "v.png")) && it->w == 60 && it->h == 60);
    CHECK((it = picture_item(p, "w.jpg")) && it->w == 180 && it->h == 90);
    CHECK((it = picture_item(p, "h.png")) && it->w == 60 && it->h == 30);
    CHECK((it = picture_item(p, "big.jpg")) && it->w == 464 && it->h == 232);
    CHECK((it = picture_item(p, "tall.jpg")) && it->w == 30 && it->h == 600);
    CHECK(!picture_item(p, "px.gif"));
    CHECK((it = picture_item(p, "a.jpg")) && it->w == 120);
    drop(p);
    /* pictures sized by a percentage shrink with flex items: the cards stay side by side */
    p = load_with_pictures("<style>.c{display:flex;gap:8px}.c div{flex:1}.c img{width:100%;height:auto}.m img{max-width:100%}</style>"
                           "<div class=c><div><img src=c1.jpg><p>one</p></div><div><img src=c2.jpg><p>two</p></div><div><img src=c3.jpg><p>three</p></div></div>"
                           "<div class=c><div class=m><img src=m1.jpg><p>one</p></div><div class=m><img src=m2.jpg><p>two</p></div></div>");
    arrive(p, "c1.jpg", 480, 320); arrive(p, "c2.jpg", 480, 320); arrive(p, "c3.jpg", 480, 320);
    arrive(p, "m1.jpg", 1000, 500); arrive(p, "m2.jpg", 1000, 500);
    relayout(p);
    sane(&p->view);
    const view_item *c1 = picture_item(p, "c1.jpg"), *c3 = picture_item(p, "c3.jpg"), *m1 = picture_item(p, "m1.jpg"), *m2 = picture_item(p, "m2.jpg");
    CHECK(c1 && c3 && c1->y == c3->y && c1->w < 160 && c3->x > c1->x + c1->w && abs(c1->h - c1->w * 2 / 3) <= 1);
    CHECK(m1 && m2 && m1->y == m2->y && m1->w <= 232 && abs(m1->h - m1->w / 2) <= 1);
    drop(p);
    /* more pictures than the table holds: the rest are shown as before */
    char html[16384];
    size_t used = 0;
    for (int i = 0; i < PICTURES_MAX + 10; i++) used += (size_t)snprintf(html + used, sizeof(html) - used, "<img src=i%d.jpg alt=N width=20 height=20>", i);
    p = load_with_pictures(html);
    int plain = 0, shown = 0;
    for (int i = 0; i < p->view.item_count; i++) if (p->view.items[i].kind == ITEM_IMAGE) { shown++; plain += !p->view.items[i].flags; }
    CHECK(p->doc.pictures->count == PICTURES_MAX && shown == PICTURES_MAX + 10 && plain == 10);
    drop(p);
}

/* What a click runs: the page's scripts (marked data-flow-click by them),
   or the close of a consent notice whose buttons do nothing else. */
static int link_at(const browser_view *v, const char *text)
{
    const view_item *it = find_text(v, text);
    return it ? it->link : -1;
}
static int control_named(const browser_view *v, const char *label)
{
    for (int i = 0; i < v->control_count; i++) if (!strcmp(view_string(v, v->controls[i].label), label)) return i;
    return -1;
}
static int source_of(const page *p, const char *id)
{
    for (int i = 0; i < p->doc.dom->count; i++) if (!strcmp(dom_attr(&p->doc.dom->nodes[i], "id"), id)) return p->doc.dom->nodes[i].source_id;
    return -1;
}
static void clicks(void)
{
    page *p = load("<p><a href=/plain>plain</a> <a id=s href=/scripted data-flow-click>scripted link</a> <span id=t data-flow-click>tap here</span> "
                   "<span>not clickable</span> <span id=icon data-flow-click aria-label='Close menu'></span></p>"
                   "<form id=f data-flow-click action=/save><input name=q><button id=go>Send</button><button id=b type=button data-flow-click>Run</button><button id=dead type=button>Dead</button></form>"
                   "<form action=/other><button>Other</button></form>", "https://example.org/", 1);
    browser_view *v = &p->view;
    sane(v);
    int plain = link_at(v, "plain"), scripted = link_at(v, "scripted link"), tap = link_at(v, "tap here");
    CHECK(plain >= 0 && v->links[plain].node < 0 && v->links[plain].dismiss < 0 && !strcmp(view_string(v, v->links[plain].url), "https://example.org/plain"));
    CHECK(scripted >= 0 && v->links[scripted].node == source_of(p, "s") && !strcmp(view_string(v, v->links[scripted].url), "https://example.org/scripted"));
    CHECK(tap >= 0 && v->links[tap].node == source_of(p, "t") && v->links[tap].url < 0);
    CHECK(link_at(v, "not clickable") < 0);
    CHECK(link_at(v, "Close menu") >= 0);      /* shown by its name */
    int go = control_named(v, "Send"), run = control_named(v, "Run"), dead = control_named(v, "Dead"), other = control_named(v, "Other");
    CHECK(go >= 0 && v->controls[go].scripted && v->controls[go].node == source_of(p, "go"));     /* its form's submission */
    CHECK(run >= 0 && v->controls[run].scripted && dead >= 0 && !v->controls[dead].scripted && other >= 0 && !v->controls[other].scripted);
    CHECK(v->controls[go].dismiss < 0);
    drop(p);
    /* A consent notice: its buttons and buttonlike links close it when
       nothing else happens; real links stay links. */
    p = load("<div id=cookie-banner class='notice'><p>We use cookies.</p><a href='#' id=ok>OK</a> <a href='/privacy'>Privacy policy</a> "
             "<span role=button>Reject</span> <div class='btn-accept'>Accept all</div> <button type=button>Settings</button></div>"
             "<div class=recipe-cookie-list><a href='#'>Top</a></div><p>Article</p>", "https://example.org/", 1);
    v = &p->view;
    sane(v);
    int notice = source_of(p, "cookie-banner"), ok = link_at(v, "OK"), privacy = link_at(v, "Privacy policy"), reject = link_at(v, "Reject"), accept = link_at(v, "Accept all"), settings = control_named(v, "Settings");
    CHECK(notice >= 0 && ok >= 0 && v->links[ok].dismiss == notice && v->links[ok].url < 0 && v->links[ok].node < 0);
    CHECK(privacy >= 0 && !strcmp(view_string(v, v->links[privacy].url), "https://example.org/privacy"));
    CHECK(reject >= 0 && v->links[reject].dismiss == notice && accept >= 0 && v->links[accept].dismiss == notice);
    CHECK(settings >= 0 && v->controls[settings].dismiss == notice);
    int top = link_at(v, "Top");     /* not a notice: the link to the page's top stays */
    CHECK(top >= 0 && v->links[top].dismiss < 0 && v->links[top].url >= 0);
    /* closed, the notice is gone the next time the page is laid out */
    p->doc.hidden[p->doc.hidden_count++] = notice;
    browser_view_free(v);
    char err[256];
    CHECK(browser_view_build(v, &p->doc, VIEW_WIDTH, measure, err, sizeof(err)) == 0);
    CHECK(!find_text(v, "We use cookies") && find_text(v, "Article"));
    settings = control_named(v, "Settings");
    CHECK(settings < 0 || v->controls[settings].item < 0);     /* a field of the page, not shown */
    drop(p);
    /* names consent tools give their notices; not just any "cookie" */
    const char *names[] = {"<div id=onetrust-banner-sdk>", "<div class='qc-cmp2-container'>", "<div id=CybotCookiebotDialog>", "<div class=cc-window>",
                           "<section aria-label='Cookie banner'>", "<div class='cookie_notice'>", "<div id=gdpr-popup>", "<div class=privacy-wall>"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        char html[256];
        snprintf(html, sizeof(html), "%s<span role=button>Agree</span></div>", names[i]);
        p = load(html, "https://example.org/", 1);
        int agree = link_at(&p->view, "Agree");
        CHECK(agree >= 0 && p->view.links[agree].dismiss >= 0);
        drop(p);
    }
    /* a dialog about cookies, whatever its names */
    p = load("<div id=dlg class=dbsFrd role=dialog aria-modal=true><h1>Before you continue</h1><div><p>We use <b>cookies</b> and data.</p></div>"
             "<button class=tHlp8d id=L2AGLb>Accept all</button></div><div role=dialog><p>Newsletter</p><button>Close</button></div>", "https://example.org/", 1);
    int accept_all = control_named(&p->view, "Accept all"), close = control_named(&p->view, "Close");
    CHECK(accept_all >= 0 && p->view.controls[accept_all].dismiss == source_of(p, "dlg"));
    CHECK(close >= 0 && p->view.controls[close].dismiss < 0);
    drop(p);
    const char *not_notices[] = {"<div class=cookie-recipe>", "<div class=privacy-policy>", "<body class=cookie-consent-open><div>"};
    for (size_t i = 0; i < sizeof(not_notices) / sizeof(*not_notices); i++) {
        char html[256];
        snprintf(html, sizeof(html), "%s<span role=button>Agree</span></div>", not_notices[i]);
        p = load(html, "https://example.org/", 1);
        CHECK(link_at(&p->view, "Agree") < 0);
        drop(p);
    }
    /* an open dialog shows; a closed one doesn't */
    p = load("<dialog open><p>Open dialog</p></dialog><dialog><p>Closed dialog</p></dialog>", "https://example.org/", 1);
    CHECK(find_text(&p->view, "Open dialog") && !find_text(&p->view, "Closed dialog"));
    drop(p);
    /* what the user changed, for the page's scripts */
    p = load("<input id=a name=a value=x><input id=b type=checkbox><select id=s><option>1<option>2</select><textarea id=t>keep</textarea>", "https://example.org/", 0);
    v = &p->view;
    CHECK(!browser_view_values(v));
    CHECK(browser_view_set_value(v, 0, "say \"hi\"\n\\") == 0);
    browser_view_toggle(v, 1);
    v->controls[2].selected = 1;
    char *values = browser_view_values(v), want[256];
    snprintf(want, sizeof(want), "[[%d,\"say \\\"hi\\\"\\u000a\\\\\",null,null],[%d,null,true,null],[%d,null,null,1]]", source_of(p, "a"), source_of(p, "b"), source_of(p, "s"));
    CHECK(values && !strcmp(values, want));
    if (values && strcmp(values, want)) fprintf(stderr, "values: %s\n want: %s\n", values, want);
    free(values);
    drop(p);
}

int main(void)
{
    flow(); boxes(); tables(); forms(); navigation(); plain_text(); readability(); flex_and_grid(); floats(); layout_limits(); pictures(); clicks();
    printf("view: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

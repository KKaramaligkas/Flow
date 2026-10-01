#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "document.h"

typedef struct { browser_document *doc; size_t used, capacity; int html, pre, latin; } writer;

static int equal(const char *a, const char *b)
{
    while (*a && *b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return !*a && !*b;
}
static int starts(const char *a, const char *b)
{
    while (*b) if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    return 1;
}
static const char *find_case(const char *text, const char *wanted)
{
    for (; *text; text++) if (starts(text, wanted)) return text;
    return NULL;
}
static size_t utf8(uint32_t c, char bytes[4])
{
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || !c) c = 0xfffd;
    if (c < 0x80) { bytes[0] = (char)c; return 1; }
    if (c < 0x800) { bytes[0] = 0xc0 | (c >> 6); bytes[1] = 0x80 | (c & 63); return 2; }
    if (c < 0x10000) { bytes[0] = 0xe0 | (c >> 12); bytes[1] = 0x80 | ((c >> 6) & 63); bytes[2] = 0x80 | (c & 63); return 3; }
    bytes[0] = 0xf0 | (c >> 18); bytes[1] = 0x80 | ((c >> 12) & 63);
    bytes[2] = 0x80 | ((c >> 6) & 63); bytes[3] = 0x80 | (c & 63); return 4;
}
static size_t character(const char *p, size_t remaining, int latin, uint32_t *value)
{
    const unsigned char *s = (const unsigned char *)p;
    static const uint16_t windows[32] = {
        0x20ac,0xfffd,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
        0x02c6,0x2030,0x0160,0x2039,0x0152,0xfffd,0x017d,0xfffd,
        0xfffd,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x02dc,0x2122,0x0161,0x203a,0x0153,0xfffd,0x017e,0x0178};
    if (*s < 128 || latin) { *value = latin && *s >= 128 && *s < 160 ? windows[*s - 128] : *s; return 1; }
    size_t n = *s >= 0xc2 && *s <= 0xdf ? 2 : *s >= 0xe0 && *s <= 0xef ? 3 : *s >= 0xf0 && *s <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) { *value = 0xfffd; return 1; }
    uint32_t c = *s & (n == 2 ? 31 : n == 3 ? 15 : 7);
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *value = 0xfffd; return 1; }
        c = (c << 6) | (s[i] & 63);
    }
    if (c < (n == 2 ? 128u : n == 3 ? 2048u : 65536u) || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) { *value = 0xfffd; return 1; }
    *value = c; return n;
}
static size_t entity(const char *p, size_t remaining, uint32_t *value)
{
    if (*p != '&') return 0;
    size_t n = 1;
    while (n < remaining && n < 32 && p[n] != ';' && !isspace((unsigned char)p[n]) && p[n] != '<') n++;
    if (n >= remaining || p[n] != ';') return 0;
    char name[32]; memcpy(name, p + 1, n - 1); name[n - 1] = 0;
    static const struct { const char *name; uint32_t code; } names[] = {
        {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",32},
        {"copy",0xa9},{"reg",0xae},{"hellip",0x2026},{"ndash",0x2013},{"mdash",0x2014},{"euro",0x20ac}};
    for (size_t i = 0; i < sizeof(names)/sizeof(*names); i++) if (!strcmp(name, names[i].name)) { *value = names[i].code; return n + 1; }
    if (name[0] == '#') {
        const char *digits = name + 1; unsigned base = 10;
        if (*digits == 'x' || *digits == 'X') { digits++; base = 16; }
        if (!*digits) return 0;
        uint32_t code = 0;
        for (; *digits; digits++) {
            int digit = isdigit((unsigned char)*digits) ? *digits - '0' :
                tolower((unsigned char)*digits) >= 'a' && tolower((unsigned char)*digits) <= 'f' ? tolower((unsigned char)*digits) - 'a' + 10 : -1;
            if (digit < 0 || (unsigned)digit >= base || code > (0x10ffffu - digit) / base) { *value = 0xfffd; return n + 1; }
            code = code * base + digit;
        }
        *value = code; return n + 1;
    }
    return 0;
}
static void emit(writer *w, uint32_t c)
{
    if (c == 0xa0 || c == '\r' || c == '\t' || (c < 32 && c != '\n') || c == 127) c = ' ';
    if (w->html && !w->pre && (c == ' ' || c == '\n')) {
        if (!w->used || w->doc->text[w->used - 1] == ' ' || w->doc->text[w->used - 1] == '\n') return;
        c = ' ';
    }
    char bytes[4]; size_t n = utf8(c, bytes);
    if (w->used + n >= (w->capacity ? w->capacity : BROWSER_TEXT_MAX - 64)) { w->doc->shortened = 1; return; }
    memcpy(w->doc->text + w->used, bytes, n); w->used += n; w->doc->text[w->used] = 0;
}
static void line(writer *w)
{
    while (w->used && w->doc->text[w->used - 1] == ' ') w->used--;
    if (w->used && w->doc->text[w->used - 1] != '\n' && w->used < BROWSER_TEXT_MAX - 64)
        w->doc->text[w->used++] = '\n';
    w->doc->text[w->used] = 0;
}
static void decoded(writer *w, const char *text, size_t length)
{
    for (size_t pos = 0; pos < length && !w->doc->shortened;) {
        uint32_t c; size_t n = entity(text + pos, length - pos, &c);
        if (!n) n = character(text + pos, length - pos, w->latin, &c);
        emit(w, c); pos += n;
    }
}
static void attribute(const char *begin, const char *end, const char *wanted, char *out, size_t size)
{
    out[0] = 0;
    const char *p = begin;
    while (p < end) {
        while (p < end && (isspace((unsigned char)*p) || *p == '/')) p++;
        const char *name = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '=' && *p != '/') p++;
        size_t n = (size_t)(p - name);
        if (!n) { p++; continue; }
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p == end || *p != '=') continue;
        p++; while (p < end && isspace((unsigned char)*p)) p++;
        char quote = p < end && (*p == '\'' || *p == '"') ? *p++ : 0;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) p++;
        size_t value_length = (size_t)(p - value);
        if (quote && p < end) p++;
        if (n != strlen(wanted)) continue;
        int matches = 1;
        for (size_t i = 0; i < n; i++) if (tolower((unsigned char)name[i]) != wanted[i]) matches = 0;
        if (!matches) continue;
        size_t used = 0;
        for (size_t i = 0; i < value_length;) {
            uint32_t c; size_t taken = entity(value + i, value_length - i, &c);
            if (!taken) taken = character(value + i, value_length - i, 0, &c);
            char bytes[4]; size_t count = utf8(c, bytes);
            if (c < 32 || c == 127 || used + count >= size) { out[0] = 0; return; }
            memcpy(out + used, bytes, count); used += count; i += taken;
        }
        out[used] = 0; return;
    }
}
static int block(const char *tag)
{
    static const char *tags[] = {"p","div","article","section","header","footer","main","nav","aside","h1","h2","h3","h4","h5","h6","li","ul","ol","tr","table","blockquote","pre","hr","br"};
    for (size_t i = 0; i < sizeof(tags)/sizeof(*tags); i++) if (equal(tag,tags[i])) return 1;
    return 0;
}
static void close_link(writer *w, int *active)
{
    if (*active < 0) return;
    browser_link *link = &w->doc->links[*active];
    size_t length = w->used - link->offset;
    if (length >= sizeof(link->label)) length = sizeof(link->label) - 1;
    /* Never cut the middle of a UTF-8 code point. */
    while (length && ((unsigned char)w->doc->text[link->offset + length] & 0xc0) == 0x80) length--;
    memcpy(link->label, w->doc->text + link->offset, length); link->label[length] = 0;
    for (char *p = link->label; *p; p++) if (*p == '\n') *p = ' ';
    if (!length) snprintf(link->label, sizeof(link->label), "Link %d", *active + 1);
    char marker[16]; snprintf(marker, sizeof(marker), " [%d]", *active + 1);
    for (const char *p = marker; *p; p++) emit(w, (unsigned char)*p);
    *active = -1;
}

void browser_document_free(browser_document *doc) { free(doc->text); memset(doc, 0, sizeof(*doc)); }

int browser_document_parse(browser_document *doc, const char *data, size_t length,
                           const char *url, const char *content_type, char *err, size_t errlen)
{
    memset(doc, 0, sizeof(*doc));
    if (!data || length > BROWSER_PAGE_MAX || memchr(data, 0, length) ||
        browser_url_resolve(NULL, url, doc->url, sizeof(doc->url)) < 0) {
        snprintf(err, errlen, "Invalid or oversized page."); return -1;
    }
    if (!content_type) content_type = "";
    int html = starts(content_type, "text/html") || starts(content_type, "application/xhtml+xml");
    if (!*content_type) {
        size_t i = 0; while (i < length && isspace((unsigned char)data[i])) i++;
        html = i < length && data[i] == '<';
    } else if (!html && !starts(content_type, "text/") && !starts(content_type, "application/json")) {
        snprintf(err, errlen, "This is a file. Select Download to save it."); return -1;
    }
    for (size_t i = 0; i < length; i++) if ((unsigned char)data[i] < 9 || ((unsigned char)data[i] > 13 && (unsigned char)data[i] < 32)) {
        snprintf(err, errlen, "Binary content. Select Download to save it."); return -1;
    }
    doc->text = calloc(1, BROWSER_TEXT_MAX);
    if (!doc->text) { snprintf(err, errlen, "Not enough memory for the page."); return -1; }
    writer w = {.doc = doc, .html = html};
    w.latin = find_case(content_type, "iso-8859-1") || find_case(content_type, "windows-1252");
    snprintf(doc->title, sizeof(doc->title), "Web page");
    if (!html) {
        /* Plain text keeps its layout and does not interpret HTML entities. */
        for (size_t i = 0; i < length && !doc->shortened;) { uint32_t c; size_t n = character(data+i,length-i,w.latin,&c); emit(&w,c); i += n; }
        goto done;
    }
    char base[BROWSER_URL_MAX]; strcpy(base, doc->url);
    int active = -1, head = 0, title = 0, base_set = 0;
    char suppressed[32] = "";
    for (size_t pos = 0; pos < length && !doc->shortened;) {
        if (suppressed[0]) {
            char closing_tag[40]; snprintf(closing_tag,sizeof(closing_tag),"</%s",suppressed);
            size_t tag_length = strlen(closing_tag), candidate = pos;
            while (candidate + tag_length < length) {
                if (data[candidate] == '<') {
                    int match = 1;
                    for (size_t i=0; i<tag_length; i++) if (tolower((unsigned char)data[candidate+i]) != closing_tag[i]) match=0;
                    char after = data[candidate+tag_length];
                    if (match && (after=='>' || after=='/' || isspace((unsigned char)after))) break;
                }
                candidate++;
            }
            if (candidate + tag_length >= length) break;
            pos = candidate;
        }
        if (data[pos] != '<') {
            size_t end = pos; while (end < length && data[end] != '<') end++;
            if (!suppressed[0] && !head && !title) decoded(&w, data + pos, end - pos);
            if (title && !suppressed[0]) {
                browser_document temporary = {0}; char title_text[512];
                temporary.text = title_text; title_text[0] = 0;
                writer tw = {.doc=&temporary,.html=1,.latin=w.latin,.capacity=sizeof(title_text)}; decoded(&tw,data+pos,end-pos);
                size_t keep = strlen(title_text);
                if (keep >= sizeof(doc->title)) keep = sizeof(doc->title) - 1;
                while (keep && ((unsigned char)title_text[keep] & 0xc0) == 0x80) keep--;
                memcpy(doc->title,title_text,keep); doc->title[keep]=0;
            }
            pos = end; continue;
        }
        if (pos + 4 <= length && !memcmp(data + pos, "<!--", 4)) {
            size_t end = pos + 4;
            while (end + 3 <= length && memcmp(data + end, "-->", 3)) end++;
            pos = end + 3 <= length ? end + 3 : length; continue;
        }
        size_t end = pos + 1; char quote = 0;
        for (; end < length; end++) {
            char c = data[end];
            if (quote) { if (c == quote) quote = 0; }
            else if (c == '\'' || c == '"') quote = c;
            else if (c == '>') break;
        }
        if (end == length) { if (!suppressed[0] && !head) decoded(&w,data+pos,length-pos); break; }
        const char *p = data + pos + 1, *limit = data + end;
        while (p < limit && isspace((unsigned char)*p)) p++;
        int closing = p < limit && *p == '/'; if (closing) p++;
        char tag[32]; size_t n = 0;
        while (p < limit && (isalnum((unsigned char)*p) || *p == '-')) { if (n + 1 < sizeof(tag)) tag[n++] = (char)tolower((unsigned char)*p); p++; }
        tag[n] = 0;
        pos = end + 1;
        if (suppressed[0]) { if (closing && equal(tag,suppressed)) suppressed[0]=0; continue; }
        if (!closing && (equal(tag,"script") || equal(tag,"style") || equal(tag,"template"))) { strcpy(suppressed,tag); continue; }
        if (equal(tag,"head")) { head = !closing; continue; }
        if (equal(tag,"title")) { title = !closing; continue; }
        if (!closing && equal(tag,"body")) head = title = 0;
        if (!closing && equal(tag,"base") && !base_set) {
            char href[BROWSER_URL_MAX], resolved[BROWSER_URL_MAX]; attribute(p,limit,"href",href,sizeof(href));
            if (*href && browser_url_resolve(base,href,resolved,sizeof(resolved)) == 0) { strcpy(base,resolved); base_set=1; }
        }
        if (head || title) continue;
        if (block(tag)) line(&w);
        if (equal(tag,"pre")) w.pre = !closing;
        if (equal(tag,"td") || equal(tag,"th")) emit(&w,' ');
        if (equal(tag,"a")) {
            close_link(&w,&active);
            if (!closing) {
                char href[BROWSER_URL_MAX], resolved[BROWSER_URL_MAX]; attribute(p,limit,"href",href,sizeof(href));
                if (*href && browser_url_resolve(base,href,resolved,sizeof(resolved)) == 0) {
                    if (doc->count == BROWSER_LINKS_MAX) doc->links_omitted=1;
                    else { active=doc->count++; strcpy(doc->links[active].url,resolved); doc->links[active].offset=w.used; }
                }
            }
        }
        if (!closing && equal(tag,"img")) { char alt[256]; attribute(p,limit,"alt",alt,sizeof(alt)); if (*alt) decoded(&w,alt,strlen(alt)); }
        if (!closing && equal(tag,"li")) { emit(&w,'*'); emit(&w,' '); }
    }
    close_link(&w,&active);
done:
    if (doc->shortened) {
        const char *notice = "\n[Page shortened to fit PSP memory.]\n";
        size_t n = strlen(notice); memcpy(doc->text+w.used,notice,n+1);
    }
    if (!*doc->text) strcpy(doc->text,"This page has no readable text. JavaScript and forms are not supported.");
    return 0;
}

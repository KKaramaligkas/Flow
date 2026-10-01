#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "url.h"

static int prefix(const char *text, const char *wanted)
{
    while (*wanted) if (tolower((unsigned char)*text++) != *wanted++) return 0;
    return 1;
}

int browser_url_secure(const char *url) { return prefix(url, "https://"); }

static int clean_reference(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '"' || *p == '<' || *p == '>') return 0;
        if (*p == '%' && (!p[1] || !p[2] || !isxdigit(p[1]) || !isxdigit(p[2]))) return 0;
    }
    return 1;
}

/* Extract a validated scheme/authority, rejecting credentials and unsupported schemes. */
static int origin(const char *url, char *out, size_t size, const char **rest)
{
    int scheme = prefix(url, "https://") ? 8 : prefix(url, "http://") ? 7 : 0;
    if (!scheme || !clean_reference(url)) return -1;
    const char *host = url + scheme, *end = host + strcspn(host, "/?#");
    size_t length = (size_t)(end - host);
    if (!length || length > 255 || memchr(host, '@', length)) return -1;
    const char *colon = memchr(host, ':', length);
    size_t host_length = colon ? (size_t)(colon - host) : length;
    if (!host_length || host[0] == '.' || host[host_length - 1] == '.') return -1;
    for (size_t i = 0; i < host_length; i++)
        if (!(isalnum((unsigned char)host[i]) || host[i] == '.' || host[i] == '-')) return -1;
    if (colon) {
        unsigned int port = 0;
        if (colon + 1 == end) return -1;
        for (const char *p = colon + 1; p < end; p++) {
            if (!isdigit((unsigned char)*p) || port > 6553) return -1;
            port = port * 10 + (unsigned int)(*p - '0');
        }
        if (!port || port > 65535) return -1;
    }
    if ((size_t)(end - url) >= size) return -1;
    memcpy(out, url, end - url); out[end - url] = 0;
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
    *rest = end;
    return 0;
}

static int canonical(const char *absolute, char *out, size_t size)
{
    char authority[BROWSER_URL_MAX], path[BROWSER_URL_MAX], normalized[BROWSER_URL_MAX];
    const char *rest;
    if (origin(absolute, authority, sizeof(authority), &rest) < 0) return -1;
    size_t length = strcspn(rest, "?#");
    if (length >= sizeof(path) || (*rest && *rest != '/' && *rest != '?' && *rest != '#')) return -1;
    memcpy(path, rest, length); path[length] = 0;
    const char *suffix = rest + length;
    normalized[0] = '/'; normalized[1] = 0;
    size_t used = 1, positions[BROWSER_URL_MAX / 2], depth = 0;
    const char *part = path;
    if (*part == '/') part++;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        int final = !end || !end[1];
        if (n == 1 && part[0] == '.') {
            if (final && used > 1 && normalized[used - 1] != '/') normalized[used++] = '/';
        } else if (n == 2 && part[0] == '.' && part[1] == '.') {
            if (depth) used = positions[--depth];
            if (final && used > 1 && normalized[used - 1] != '/') normalized[used++] = '/';
        } else {
            /* Preserve empty path segments: /a//b can name a different resource. */
            if (used + n + 2 >= sizeof(normalized) || depth >= sizeof(positions)/sizeof(*positions)) return -1;
            positions[depth++] = used;
            if (used > 1 && normalized[used - 1] != '/') normalized[used++] = '/';
            memcpy(normalized + used, part, n); used += n;
            if (end) normalized[used++] = '/';
        }
        normalized[used] = 0;
        if (!end) break;
        part = end + 1;
    }
    int result = snprintf(out, size, "%s%s%s", authority, normalized, suffix);
    return result >= 0 && (size_t)result < size ? 0 : -1;
}

int browser_url_resolve(const char *base, const char *reference, char *out, size_t size)
{
    if (!reference || strlen(reference) >= BROWSER_URL_MAX || !clean_reference(reference)) return -1;
    char absolute[BROWSER_URL_MAX], authority[BROWSER_URL_MAX];
    if (prefix(reference, "http://") || prefix(reference, "https://")) return canonical(reference, out, size);
    /* A colon before /?# denotes a scheme, never an ordinary relative path. */
    const char *colon = strchr(reference, ':');
    if (colon && (size_t)(colon - reference) < strcspn(reference, "/?#")) return -1;
    const char *rest;
    if (!base || strlen(base) >= BROWSER_URL_MAX || origin(base, authority, sizeof(authority), &rest) < 0) return -1;
    int result;
    if (!strncmp(reference, "//", 2)) {
        result = snprintf(absolute, sizeof(absolute), "%s:%s", browser_url_secure(base) ? "https" : "http", reference);
    } else if (*reference == '/') {
        result = snprintf(absolute, sizeof(absolute), "%s%s", authority, reference);
    } else if (*reference == '?' || *reference == '#' || !*reference) {
        size_t keep = *reference == '?' ? strcspn(base, "?#") : strcspn(base, "#");
        result = snprintf(absolute, sizeof(absolute), "%.*s%s", (int)keep, base, reference);
    } else {
        size_t length = strcspn(rest, "?#"), directory = 0;
        for (size_t i = 0; i < length; i++) if (rest[i] == '/') directory = i + 1;
        result = snprintf(absolute, sizeof(absolute), "%s%.*s%s%s", authority, (int)directory, rest,
                          directory ? "" : "/", reference);
    }
    if (result < 0 || (size_t)result >= sizeof(absolute)) return -1;
    return canonical(absolute, out, size);
}

int browser_url_enter(const char *input, char *out, size_t size)
{
    if (!input) return -1;
    while (isspace((unsigned char)*input)) input++;
    size_t length = strlen(input);
    while (length && isspace((unsigned char)input[length - 1])) length--;
    if (!length || length >= BROWSER_URL_MAX) return -1;
    char value[BROWSER_URL_MAX]; memcpy(value, input, length); value[length] = 0;
    if (strstr(value, "://")) return browser_url_resolve(NULL, value, out, size);
    char absolute[BROWSER_URL_MAX];
    int n = snprintf(absolute, sizeof(absolute), "https://%s", value);
    if (n < 0 || (size_t)n >= sizeof(absolute)) return -1;
    return browser_url_resolve(NULL, absolute, out, size);
}

void browser_filename(const char *url, char *out, size_t size)
{
    if (!size) return;
    const char *start = strstr(url, "://");
    start = start ? strchr(start + 3, '/') : NULL;
    if (!start) start = "";
    size_t length = strcspn(start, "?#");
    for (size_t i = 0; i < length; i++) if (start[i] == '/') { start += i + 1; length -= i + 1; i = (size_t)-1; }
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < size && used < 96; i++) {
        unsigned char c = (unsigned char)start[i];
        out[used++] = isalnum(c) || c == '.' || c == '-' || c == '_' ? (char)c : '_';
    }
    out[used] = 0;
    if (!used || out[0] == '.') snprintf(out, size, "download.bin");
}

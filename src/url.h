/* ARK Browser: bounded HTTP(S) URL handling, independent of the PSP SDK. */
#ifndef ARKB_URL_H
#define ARKB_URL_H
#include <stddef.h>
#define BROWSER_URL_MAX 1024
int browser_url_resolve(const char *base, const char *reference, char *out, size_t size);
int browser_url_enter(const char *input, char *out, size_t size);
int browser_url_secure(const char *url);
void browser_filename(const char *url, char *out, size_t size);
#endif

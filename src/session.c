#include <string.h>
#include "session.h"
const char *browser_history_back(const browser_history *h)
{
    return h->count && h->current > 0 ? h->visits[h->current - 1].url : NULL;
}
int browser_history_commit(browser_history *h, int navigation, const char *url)
{
    char validated[BROWSER_URL_MAX];
    if (browser_url_resolve(NULL, url, validated, sizeof(validated)) < 0) return -1;
    if (navigation == NAV_BACK) {
        if (!browser_history_back(h)) return -1;
        h->current--;
    } else if (navigation == NAV_RELOAD) {
        if (!h->count) return -1;
    } else if (navigation == NAV_NEW) {
        if (h->count && !strcmp(h->visits[h->current].url, validated)) return 0;
        h->count = h->count ? h->current + 1 : 0;
        if (h->count == BROWSER_HISTORY_MAX) {
            memmove(h->visits, h->visits + 1, sizeof(h->visits[0]) * (BROWSER_HISTORY_MAX - 1));
            h->count--;
        }
        h->current = h->count++;
        h->visits[h->current].scroll = 0;
    } else return -1;
    strcpy(h->visits[h->current].url, validated);
    return 0;
}

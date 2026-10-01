#ifndef ARKB_SESSION_H
#define ARKB_SESSION_H
#include "url.h"
#define BROWSER_HISTORY_MAX 12
#define NAV_NEW 0
#define NAV_BACK 1
#define NAV_RELOAD 2

typedef struct { char url[BROWSER_URL_MAX]; int scroll; } browser_visit;
typedef struct { browser_visit visits[BROWSER_HISTORY_MAX]; int count, current; } browser_history;
const char *browser_history_back(const browser_history *history);
int browser_history_commit(browser_history *history, int navigation, const char *url);
#endif

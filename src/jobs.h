#ifndef ARKB_JOBS_H
#define ARKB_JOBS_H
#include <stdint.h>
#include "document.h"
#define BROWSER_JOB_PAGE 1
#define BROWSER_JOB_DOWNLOAD 2

typedef struct {
    volatile int running, finished, cancel, result;
    int type, navigation;
    char url[BROWSER_URL_MAX], destination[256], error[256];
    volatile int64_t done, total;
    browser_document *page;
} browser_job;
extern browser_job browser_work;
int browser_jobs_start(void);
void browser_jobs_stop(void);
int browser_jobs_busy(void);
int browser_jobs_submit(int type, int navigation, const char *url, const char *destination);
#endif

#ifndef PAL_MILESTONE_H
#define PAL_MILESTONE_H

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MILESTONE_BOOT_START    = 0,
    MILESTONE_HEAP_INIT     = 1,
    MILESTONE_GFX_INIT      = 2,
    MILESTONE_PAD_INIT      = 3,
    MILESTONE_FRAMEWORK_INIT = 4,
    MILESTONE_FIRST_FRAME   = 5,
    MILESTONE_LOGO_SCENE    = 6,
    MILESTONE_TITLE_SCENE   = 7,
    MILESTONE_PLAY_SCENE    = 8,
    MILESTONE_STAGE_LOADED  = 9,
    MILESTONE_FRAMES_60     = 10,
    MILESTONE_FRAMES_300    = 11,
    MILESTONE_FRAMES_1800   = 12,
    MILESTONE_TEST_COMPLETE = 99,
    MILESTONE_CRASH         = -1
};

static unsigned long s_boot_time_ms;

static inline void pal_milestone_init(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_boot_time_ms = (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static inline void pal_milestone(const char* name, int id, const char* detail) {
    struct timespec ts;
    unsigned long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    fprintf(stdout, "{\"milestone\":\"%s\",\"id\":%d,\"time_ms\":%lu,\"detail\":\"%s\"}\n",
            name, id, now - s_boot_time_ms, detail ? detail : "");
    fflush(stdout);
}

static inline void pal_milestone_frame(const char* name, int id, unsigned frame) {
    struct timespec ts;
    unsigned long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    fprintf(stdout, "{\"milestone\":\"%s\",\"id\":%d,\"time_ms\":%lu,\"frame\":%u}\n",
            name, id, now - s_boot_time_ms, frame);
    fflush(stdout);
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* PAL_MILESTONE_H */

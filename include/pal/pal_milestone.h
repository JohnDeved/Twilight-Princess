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
    MILESTONE_DVD_READ_OK   = 13,
    MILESTONE_SCENE_CREATED = 14,
    MILESTONE_RENDER_FRAME  = 15,
    MILESTONE_COUNT         = 16,
    MILESTONE_TEST_COMPLETE = 99,
    MILESTONE_CRASH         = -1
};

/* Defined in pal_milestone.cpp — shared across all translation units */
extern int g_milestones_reached[MILESTONE_COUNT];
extern unsigned long g_boot_time_ms;

void pal_milestone_init(void);
int  pal_milestone_was_reached(int id);
void pal_milestone(const char* name, int id, const char* detail);
void pal_milestone_frame(const char* name, int id, unsigned frame);
void pal_milestone_check_scene(int profname);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

#endif /* PAL_MILESTONE_H */

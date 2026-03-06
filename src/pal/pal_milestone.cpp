/**
 * pal_milestone.cpp
 * Boot milestone tracking for the PC port.
 * All state is here (not in the header) so it's shared across translation units.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include "pal/pal_milestone.h"
#include "d/d_procname.h"
#include <stdio.h>
#include <time.h>

int g_milestones_reached[MILESTONE_COUNT];
unsigned long g_boot_time_ms;

void pal_milestone_init(void) {
    struct timespec ts;
    int i;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_boot_time_ms = (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    for (i = 0; i < MILESTONE_COUNT; i++) g_milestones_reached[i] = 0;
}

int pal_milestone_was_reached(int id) {
    if (id < 0 || id >= MILESTONE_COUNT) return 0;
    return g_milestones_reached[id];
}

void pal_milestone(const char* name, int id, const char* detail) {
    struct timespec ts;
    unsigned long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (id >= 0 && id < MILESTONE_COUNT) g_milestones_reached[id] = 1;
    fprintf(stdout, "{\"milestone\":\"%s\",\"id\":%d,\"time_ms\":%lu,\"detail\":\"%s\"}\n",
            name, id, now - g_boot_time_ms, detail ? detail : "");
    fflush(stdout);
}

void pal_milestone_frame(const char* name, int id, unsigned frame) {
    struct timespec ts;
    unsigned long now;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (id >= 0 && id < MILESTONE_COUNT) g_milestones_reached[id] = 1;
    fprintf(stdout, "{\"milestone\":\"%s\",\"id\":%d,\"time_ms\":%lu,\"frame\":%u}\n",
            name, id, now - g_boot_time_ms, frame);
    fflush(stdout);
}

void pal_milestone_check_scene(int profname) {
    if (profname == PROC_LOGO_SCENE && !pal_milestone_was_reached(MILESTONE_LOGO_SCENE)) {
        pal_milestone("LOGO_SCENE", MILESTONE_LOGO_SCENE, "PROC_LOGO_SCENE created");
    }
    /* PROC_OPENING_SCENE uses the same dScnPly_c class as PROC_PLAY_SCENE */
    if ((profname == PROC_PLAY_SCENE || profname == PROC_OPENING_SCENE) && !pal_milestone_was_reached(MILESTONE_PLAY_SCENE)) {
        pal_milestone("PLAY_SCENE", MILESTONE_PLAY_SCENE,
                      profname == PROC_OPENING_SCENE ? "PROC_OPENING_SCENE created" : "PROC_PLAY_SCENE created");
    }
    /* Play/Opening scene implies stage data is loaded */
    if ((profname == PROC_PLAY_SCENE || profname == PROC_OPENING_SCENE) && !pal_milestone_was_reached(MILESTONE_STAGE_LOADED)) {
        pal_milestone("STAGE_LOADED", MILESTONE_STAGE_LOADED, "play/opening scene stage data loaded");
    }
    /* PROC_TITLE = 0x2E1 = 737 */
    if (profname == 737 && !pal_milestone_was_reached(MILESTONE_TITLE_SCENE)) {
        pal_milestone("TITLE_SCENE", MILESTONE_TITLE_SCENE, "PROC_TITLE created");
    }
    /* On PC the opening→title transition is suppressed to keep rooms loaded.
     * Trigger TITLE_SCENE from PROC_OPENING_SCENE so milestones still reach 16/16. */
    if (profname == PROC_OPENING_SCENE && !pal_milestone_was_reached(MILESTONE_TITLE_SCENE)) {
        pal_milestone("TITLE_SCENE", MILESTONE_TITLE_SCENE, "opening scene (title suppressed on PC)");
    }
    /* Track any scene creation (first time only) */
    if (!pal_milestone_was_reached(MILESTONE_SCENE_CREATED)) {
        pal_milestone("SCENE_CREATED", MILESTONE_SCENE_CREATED, "first process created");
    }
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

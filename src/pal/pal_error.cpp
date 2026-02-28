/**
 * @file pal_error.cpp
 * @brief Error reporting and telemetry for PC port.
 *
 * Provides structured error logging to replace silent fail-open guards.
 * Errors are deduplicated by (category, detail) and written to
 * verify_output/error_log.txt with frame context.
 */

#include "global.h"

#if PLATFORM_PC

#include "pal/pal_error.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ================================================================ */
/* Error deduplication table                                        */
/* ================================================================ */

#define PAL_ERR_MAX_UNIQUE 512
#define PAL_ERR_DETAIL_LEN 128

typedef struct {
    PalErrorCategory cat;
    char             detail[PAL_ERR_DETAIL_LEN];
    u32              count;
    u32              first_frame;
} PalErrorEntry;

static PalErrorEntry s_errors[PAL_ERR_MAX_UNIQUE];
static u32 s_error_count = 0;
static u32 s_category_counts[PAL_ERR_COUNT];
static u32 s_current_frame = 0;
static FILE* s_error_log = NULL;
static u32 s_total_errors = 0;
static int s_initialized = 0;

static const char* s_category_names[PAL_ERR_COUNT] = {
    "J3D_LOAD",
    "J3D_ENDIAN",
    "RARC_PARSE",
    "RESOURCE",
    "NULL_PTR",
    "DL_PARSE",
    "TEV_CONFIG",
    "STAGE_DATA",
    "STUB_CALL"
};

extern "C" {

void pal_error_init(void) {
    if (s_initialized) return;
    memset(s_errors, 0, sizeof(s_errors));
    memset(s_category_counts, 0, sizeof(s_category_counts));
    s_error_count = 0;
    s_total_errors = 0;
    s_current_frame = 0;

    s_error_log = fopen("verify_output/error_log.txt", "w");
    if (!s_error_log) {
        /* Try without directory */
        s_error_log = stderr;
    }
    s_initialized = 1;
}

void pal_error_shutdown(void) {
    if (!s_initialized) return;

    /* Write summary */
    pal_error_dump_summary();

    if (s_error_log && s_error_log != stderr) {
        fclose(s_error_log);
    }
    s_error_log = NULL;
    s_initialized = 0;
}

void pal_error_set_frame(u32 frame) {
    s_current_frame = frame;
}

int pal_error(PalErrorCategory cat, const char* detail) {
    if (!s_initialized) pal_error_init();
    if ((unsigned)cat >= PAL_ERR_COUNT) return 0;

    s_total_errors++;
    s_category_counts[cat]++;

    /* Look for existing entry */
    const char* d = detail ? detail : "";
    for (u32 i = 0; i < s_error_count; i++) {
        if (s_errors[i].cat == cat && strncmp(s_errors[i].detail, d, PAL_ERR_DETAIL_LEN - 1) == 0) {
            s_errors[i].count++;
            return (int)s_errors[i].count;
        }
    }

    /* New unique error */
    if (s_error_count < PAL_ERR_MAX_UNIQUE) {
        PalErrorEntry* e = &s_errors[s_error_count++];
        e->cat = cat;
        strncpy(e->detail, d, PAL_ERR_DETAIL_LEN - 1);
        e->detail[PAL_ERR_DETAIL_LEN - 1] = '\0';
        e->count = 1;
        e->first_frame = s_current_frame;
    }

    /* Log to file (first occurrence only) */
    if (s_error_log) {
        fprintf(s_error_log, "frame=%u cat=%s detail=\"%s\"\n",
                s_current_frame, s_category_names[cat], d);
        fflush(s_error_log);
    }

    return 1;
}

u32 pal_error_get_count(PalErrorCategory cat) {
    if ((unsigned)cat >= PAL_ERR_COUNT) return 0;
    return s_category_counts[cat];
}

u32 pal_error_get_total(void) {
    return s_total_errors;
}

void pal_error_dump_summary(void) {
    fprintf(stderr, "{\"pal_errors\":{\"total\":%u,\"unique\":%u,\"by_cat\":{",
            s_total_errors, s_error_count);
    for (int i = 0; i < PAL_ERR_COUNT; i++) {
        if (i > 0) fprintf(stderr, ",");
        fprintf(stderr, "\"%s\":%u", s_category_names[i], s_category_counts[i]);
    }
    fprintf(stderr, "},\"top\":[");
    /* Print top 10 most frequent errors */
    int printed = 0;
    for (u32 i = 0; i < s_error_count && printed < 10; i++) {
        if (s_errors[i].count > 0) {
            if (printed > 0) fprintf(stderr, ",");
            fprintf(stderr, "{\"cat\":\"%s\",\"detail\":\"%s\",\"count\":%u,\"first_frame\":%u}",
                    s_category_names[s_errors[i].cat], s_errors[i].detail,
                    s_errors[i].count, s_errors[i].first_frame);
            printed++;
        }
    }
    fprintf(stderr, "]}}\n");
}

} /* extern "C" */

#endif /* PLATFORM_PC */

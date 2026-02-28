/**
 * @file pal_error.h
 * @brief Error reporting and telemetry for PC port.
 *
 * Replaces silent fail-open guards with explicit error states that are
 * logged to verify_output/error_log.txt with frame number and context.
 * Errors are categorized by subsystem for structured analysis.
 */
#ifndef PAL_ERROR_H
#define PAL_ERROR_H

#include "dolphin/types.h"

#if PLATFORM_PC

#ifdef __cplusplus
extern "C" {
#endif

/* Error categories */
typedef enum {
    PAL_ERR_J3D_LOAD,       /* J3D model/animation load failure */
    PAL_ERR_J3D_ENDIAN,     /* Endian swap validation failure */
    PAL_ERR_RARC_PARSE,     /* RARC archive parse/access error */
    PAL_ERR_RESOURCE,       /* Resource load failure (archive/file) */
    PAL_ERR_NULL_PTR,       /* NULL pointer in expected-valid path */
    PAL_ERR_DL_PARSE,       /* Display list parse error */
    PAL_ERR_TEV_CONFIG,     /* TEV combiner config not handled */
    PAL_ERR_STAGE_DATA,     /* Stage data access/parse error */
    PAL_ERR_STUB_CALL,      /* Stub function reached that needs impl */
    PAL_ERR_COUNT
} PalErrorCategory;

/**
 * Initialize the error reporting system.
 * Opens verify_output/error_log.txt for writing.
 */
void pal_error_init(void);

/**
 * Shut down and flush error log.
 */
void pal_error_shutdown(void);

/**
 * Report an error with context.
 * Logs to error_log.txt with frame number, category, and message.
 * First occurrence of each unique (category, detail) pair is logged;
 * subsequent occurrences increment a counter but don't re-log.
 * Returns the total count for this error (1 = first occurrence).
 */
int pal_error(PalErrorCategory cat, const char* detail);

/**
 * Set the current frame number for error context.
 * Called once per frame from the main loop.
 */
void pal_error_set_frame(u32 frame);

/**
 * Get error count for a category.
 */
u32 pal_error_get_count(PalErrorCategory cat);

/**
 * Get total error count across all categories.
 */
u32 pal_error_get_total(void);

/**
 * Write a summary of all errors to stderr (JSON format).
 */
void pal_error_dump_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC */
#endif /* PAL_ERROR_H */

/**
 * pal_os_stubs.cpp
 * Stub implementations for Dolphin/Revolution OS functions that are called
 * by game code but whose SDK source files are excluded from the PC build.
 * These provide safe no-op or minimal implementations.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Cache operations (no-ops on x86/ARM) ---- */
void DCFlushRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCFlushRangeNoSync(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRangeNoSync(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void ICInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }

/* ---- OS functions ---- */
void OSReport(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void OSReportInit(void) {}

void OSReportDisable(void) {}

void OSReport_Error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
    va_list args;
    fprintf(stderr, "PANIC at %s:%d: ", file, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    abort();
}

u32 OSGetConsoleType(void) { return 0x10000006; /* OS_CONSOLE_DEVELOPMENT */ }

/* Time: use host clock, scaled to match GC bus clock (162 MHz) */
static u64 s_start_time = 0;
static struct timespec s_start_ts;

static u64 get_host_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 elapsed_ns = (u64)(ts.tv_sec - s_start_ts.tv_sec) * 1000000000ULL +
                     (u64)(ts.tv_nsec - s_start_ts.tv_nsec);
    /* Scale to GC timebase: 1/4 of bus clock = 40.5 MHz */
    return elapsed_ns * 405 / 10000;
}

s64 OSGetTime(void) {
    if (s_start_time == 0) {
        clock_gettime(CLOCK_MONOTONIC, &s_start_ts);
        s_start_time = 1;
    }
    return (s64)get_host_ticks();
}

u32 OSGetTick(void) {
    return (u32)get_host_ticks();
}

/* Interrupt control (no-ops on PC) */
int OSDisableInterrupts(void) { return 0; }
int OSEnableInterrupts(void) { return 0; }
int OSRestoreInterrupts(int level) { (void)level; return 0; }

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

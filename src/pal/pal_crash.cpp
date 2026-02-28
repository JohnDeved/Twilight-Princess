/**
 * pal_crash.cpp
 * Signal handler + backtrace for crash reporting in CI.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>  /* _exit */

#ifdef __linux__
#include <execinfo.h>
#endif

static void crash_handler(int sig) {
    /* Emit CRASH JSON to stdout so parse_milestones.py can consume it */
    fprintf(stdout, "{\"milestone\":\"CRASH\",\"id\":-1,\"signal\":%d}\n", sig);
    fflush(stdout);
#ifdef __linux__
    void* bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
#endif
    _exit(128 + sig);
}

void pal_crash_init(void) {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

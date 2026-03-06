/**
 * pal_crash.cpp
 * Signal handler + backtrace for crash reporting in CI.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>  /* _exit */

#ifdef __linux__
#include <execinfo.h>
#include <ucontext.h>
#define MAX_CRASH_LOG_FRAMES 10
#endif

extern "C" uint32_t pal_capture_get_frame_count(void);
extern "C" void pal_tev_report_diagnostics(void);
extern "C" void gx_stub_report(void);

static void crash_handler_sa(int sig, siginfo_t* info, void* ucontext) {
    /* Emit CRASH JSON to stdout so parse_milestones.py can consume it */
    uint32_t frame = pal_capture_get_frame_count();
    fprintf(stdout, "{\"milestone\":\"CRASH\",\"id\":-1,\"signal\":%d,\"frame\":%u}\n", sig, frame);
    fflush(stdout);
#ifdef __linux__
    /* Print the actual faulting instruction pointer from the signal context */
    ucontext_t* uc = (ucontext_t*)ucontext;
    void* fault_addr = info->si_addr;
    void* rip = (void*)uc->uc_mcontext.gregs[REG_RIP];
    fprintf(stderr, "CRASH: sig=%d fault_addr=%p rip=%p frame=%u\n", sig, fault_addr, rip, frame);
    fflush(stderr);

    void* bt[32];
    int n = backtrace(bt, 32);
    /* Replace first backtrace entry with actual RIP for accurate decode */
    if (n > 0) bt[0] = rip;
    backtrace_symbols_fd(bt, n, 2);

    /* Append crash entry to crash log file for multi-failure triage.
     * TP_CRASH_LOG env var sets the path (default: crash_log.txt). */
    const char* log_path = getenv("TP_CRASH_LOG");
    if (!log_path) log_path = "crash_log.txt";
    FILE* logf = fopen(log_path, "a");
    if (logf) {
        fprintf(logf, "CRASH: sig=%d fault_addr=%p rip=%p frame=%u\n",
                sig, fault_addr, rip, frame);
        char** syms = backtrace_symbols(bt, n);
        if (syms) {
            int count = n > MAX_CRASH_LOG_FRAMES ? MAX_CRASH_LOG_FRAMES : n;
            for (int i = 0; i < count; i++)
                fprintf(logf, "  [%d] %s\n", i, syms[i]);
            free(syms);
        }
        fprintf(logf, "---\n");
        fclose(logf);
    }
#endif
    _exit(128 + sig);
}

static void term_handler(int sig) {
    /* Ensure clean exit on SIGTERM (used by timeout(1) in CI) */
    uint32_t frame = pal_capture_get_frame_count();
    fprintf(stdout, "{\"milestone\":\"TERMINATED\",\"id\":-2,\"signal\":%d,\"frame\":%u}\n", sig, frame);
    fflush(stdout);
    /* Emit TEV/draw diagnostics so validate_telemetry.py has tev_config_summary
     * even when killed by CI timeout (avoids spurious "0 TEV configs" regression). */
    pal_tev_report_diagnostics();
    gx_stub_report();
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}

void pal_crash_init(void) {
    struct sigaction sa;
    sa.sa_sigaction = crash_handler_sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    /* Handle SIGTERM for clean exit from timeout(1) in CI */
    struct sigaction sa_term;
    sa_term.sa_handler = term_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, NULL);
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

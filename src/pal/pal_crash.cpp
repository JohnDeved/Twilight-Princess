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
#include <ucontext.h>
#endif

static void crash_handler_sa(int sig, siginfo_t* info, void* ucontext) {
    /* Emit CRASH JSON to stdout so parse_milestones.py can consume it */
    fprintf(stdout, "{\"milestone\":\"CRASH\",\"id\":-1,\"signal\":%d}\n", sig);
    fflush(stdout);
#ifdef __linux__
    /* Print the actual faulting instruction pointer from the signal context */
    ucontext_t* uc = (ucontext_t*)ucontext;
    void* fault_addr = info->si_addr;
    void* rip = (void*)uc->uc_mcontext.gregs[REG_RIP];
    fprintf(stderr, "CRASH: sig=%d fault_addr=%p rip=%p\n", sig, fault_addr, rip);
    fflush(stderr);

    void* bt[32];
    int n = backtrace(bt, 32);
    /* Replace first backtrace entry with actual RIP for accurate decode */
    if (n > 0) bt[0] = rip;
    backtrace_symbols_fd(bt, n, 2);
#endif
    _exit(128 + sig);
}

void pal_crash_init(void) {
    struct sigaction sa;
    sa.sa_sigaction = crash_handler_sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

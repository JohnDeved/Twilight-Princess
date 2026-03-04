/**
 * f_pc_draw.cpp
 * Framework - Process Draw
 */

#include "f_pc/f_pc_draw.h"
#include "SSystem/SComponent/c_API_graphic.h"
#include "f_pc/f_pc_leaf.h"
#include "f_pc/f_pc_node.h"
#include "f_pc/f_pc_pause.h"

#if PLATFORM_PC
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <unistd.h>
extern "C" int pal_is_exec_crashed(unsigned int id);
static volatile sig_atomic_t s_draw_crash = 0;
static sigjmp_buf s_draw_jmpbuf;
static void draw_sigsegv_handler(int sig) {
    (void)sig;
    s_draw_crash = 1;
    siglongjmp(s_draw_jmpbuf, 1);
}
/* Per-actor Draw timeout: skip actors whose Draw hangs > 15s.
 * Uses SIGALRM which siglongjmps to a separate jmpbuf. */
static sigjmp_buf s_draw_alarm_jmpbuf;
static void draw_alarm_handler(int sig) {
    (void)sig;
    siglongjmp(s_draw_alarm_jmpbuf, 1);
}
#endif

int fpcDw_Execute(base_process_class* i_proc) {
#if PLATFORM_PC
    if (i_proc == NULL || i_proc->methods == NULL) return 0;
    /* Skip Draw for actors that crashed during Execute — their internal
     * state is corrupted and Draw may loop forever. */
    if (pal_is_exec_crashed(i_proc->id)) return 0;
    {
        static int s_dw_exec_frame = 0;
        static int s_dw_exec_count = 0;
        s_dw_exec_count++;
        if (s_dw_exec_count < 20 || (s_dw_exec_count % 200 == 0 && s_dw_exec_count < 2000)) {
            fprintf(stderr, "{\"dw_exec\":{\"n\":%d,\"prof\":%d,\"id\":%u}}\n",
                    s_dw_exec_count, i_proc->profname, i_proc->id);
        }
    }
#endif
    if (
#if PLATFORM_PC
        true ||  /* On PC, bypass pause check — fader/overlap doesn't unpause */
#endif
        !fpcPause_IsEnable(i_proc, 2)) {
        layer_class* save_layer;
        int ret;
        process_method_func draw_func;
    
        save_layer = fpcLy_CurrentLayer();
        if (fpcBs_Is_JustOfType(g_fpcLf_type, i_proc->subtype)) {
            draw_func = ((leafdraw_method_class*)i_proc->methods)->draw_method;
        } else {
            draw_func = ((nodedraw_method_class*)i_proc->methods)->draw_method;
        }
    
#if PLATFORM_PC
        if (draw_func == NULL) return 0;
        /* Wrap Draw with SIGSEGV/SIGABRT protection + SIGALRM timeout */
        {
            struct sigaction sa_new, sa_old_segv, sa_old_abrt, sa_old_alarm;
            memset(&sa_new, 0, sizeof(sa_new));
            sa_new.sa_handler = draw_sigsegv_handler;
            sigemptyset(&sa_new.sa_mask);
            sa_new.sa_flags = SA_NODEFER | SA_ONSTACK;
            sigaction(SIGSEGV, &sa_new, &sa_old_segv);
            sigaction(SIGABRT, &sa_new, &sa_old_abrt);

            /* SIGALRM timeout: skip actor Draw if it hangs > 15s */
            struct sigaction sa_alm;
            memset(&sa_alm, 0, sizeof(sa_alm));
            sa_alm.sa_handler = draw_alarm_handler;
            sigemptyset(&sa_alm.sa_mask);
            sa_alm.sa_flags = 0;
            sigaction(SIGALRM, &sa_alm, &sa_old_alarm);

            s_draw_crash = 0;

            if (sigsetjmp(s_draw_jmpbuf, 1) != 0) {
                alarm(0);
                sigaction(SIGSEGV, &sa_old_segv, NULL);
                sigaction(SIGABRT, &sa_old_abrt, NULL);
                sigaction(SIGALRM, &sa_old_alarm, NULL);
                fpcLy_SetCurrentLayer(save_layer);
                fprintf(stderr, "[PAL] SIGSEGV caught in Draw (prof=%d id=%u)\n",
                        i_proc->profname, i_proc->id);
                return 0;
            }
            if (sigsetjmp(s_draw_alarm_jmpbuf, 1) != 0) {
                alarm(0);
                sigaction(SIGSEGV, &sa_old_segv, NULL);
                sigaction(SIGABRT, &sa_old_abrt, NULL);
                sigaction(SIGALRM, &sa_old_alarm, NULL);
                fpcLy_SetCurrentLayer(save_layer);
                fprintf(stderr, "[PAL] Draw timeout (prof=%d id=%u) — skipping\n",
                        i_proc->profname, i_proc->id);
                return 0;
            }

            alarm(15);
            fpcLy_SetCurrentLayer(i_proc->layer_tag.layer);
            ret = draw_func(i_proc);
            alarm(0);
            fpcLy_SetCurrentLayer(save_layer);

            sigaction(SIGSEGV, &sa_old_segv, NULL);
            sigaction(SIGABRT, &sa_old_abrt, NULL);
            sigaction(SIGALRM, &sa_old_alarm, NULL);
            return ret;
        }
#else
        fpcLy_SetCurrentLayer(i_proc->layer_tag.layer);
        ret = draw_func(i_proc);
        fpcLy_SetCurrentLayer(save_layer);
        return ret;
#endif
    }

    return 0;
}

int fpcDw_Handler(fpcDw_HandlerFuncFunc i_iterHandler, fpcDw_HandlerFunc i_func) {
    int ret;
    cAPIGph_BeforeOfDraw();
    ret = i_iterHandler(i_func);
#if PLATFORM_PC
    {
        static int s_dw_handler_frame = 0;
        if (s_dw_handler_frame < 5 || (s_dw_handler_frame >= 127 && s_dw_handler_frame <= 135) || s_dw_handler_frame % 50 == 0) {
            fprintf(stderr, "{\"dw_handler\":{\"frame\":%d,\"ret\":%d}}\n", s_dw_handler_frame, ret);
        }
        s_dw_handler_frame++;
    }
#endif
    cAPIGph_AfterOfDraw();
    return ret;
}

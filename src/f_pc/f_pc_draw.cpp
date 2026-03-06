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
extern "C" int pal_is_exec_crashed(unsigned int id);
extern "C" int pal_is_profile_suppressed(int profname);
extern "C" void pal_crash_handler_init(void);
extern sigjmp_buf* pal_crash_jmpbuf;
extern volatile sig_atomic_t pal_crash_occurred;

/* Per-profile Draw crash counter — threshold = 1 (skip after first crash) */
#define PAL_DRAW_MAX_CRASH_PROFILES 128
#define PAL_DRAW_CRASH_THRESHOLD 1
struct pal_draw_crash_entry { int profname; int count; };
static pal_draw_crash_entry s_draw_profile_crashes[PAL_DRAW_MAX_CRASH_PROFILES];
static int s_draw_profile_crash_count = 0;
static int pal_draw_crash_increment(int profname) {
    for (int i = 0; i < s_draw_profile_crash_count; i++) {
        if (s_draw_profile_crashes[i].profname == profname)
            return ++s_draw_profile_crashes[i].count;
    }
    if (s_draw_profile_crash_count < PAL_DRAW_MAX_CRASH_PROFILES) {
        s_draw_profile_crashes[s_draw_profile_crash_count].profname = profname;
        s_draw_profile_crashes[s_draw_profile_crash_count].count = 1;
        s_draw_profile_crash_count++;
    }
    return 1;
}
static int pal_draw_is_suppressed(int profname) {
    for (int i = 0; i < s_draw_profile_crash_count; i++) {
        if (s_draw_profile_crashes[i].profname == profname &&
            s_draw_profile_crashes[i].count >= PAL_DRAW_CRASH_THRESHOLD)
            return 1;
    }
    return 0;
}
#endif

int fpcDw_Execute(base_process_class* i_proc) {
#if PLATFORM_PC
    if (i_proc == NULL || i_proc->methods == NULL) return 0;
    /* Skip Draw for actors that crashed during Execute */
    if (pal_is_exec_crashed(i_proc->id)) return 0;
    /* Skip profiles permanently suppressed (Execute or Draw crashed) */
    if (pal_is_profile_suppressed(i_proc->profname)) return 0;
    if (pal_draw_is_suppressed(i_proc->profname)) return 0;
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
        /* Use global crash handler — no per-call sigaction overhead */
        {
            pal_crash_handler_init();
            sigjmp_buf jb;
            sigjmp_buf* prev_target = pal_crash_jmpbuf;
            pal_crash_jmpbuf = &jb;
            pal_crash_occurred = 0;

            if (sigsetjmp(jb, 1) != 0) {
                pal_crash_jmpbuf = prev_target;
                fpcLy_SetCurrentLayer(save_layer);
                int cc = pal_draw_crash_increment(i_proc->profname);
                fprintf(stderr, "[PAL] SIGSEGV caught in Draw (prof=%d id=%u) — permanently suppressed\n",
                        i_proc->profname, i_proc->id);
                return 0;
            }

            fpcLy_SetCurrentLayer(i_proc->layer_tag.layer);
            ret = draw_func(i_proc);
            fpcLy_SetCurrentLayer(save_layer);

            pal_crash_jmpbuf = prev_target;
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

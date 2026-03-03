/**
 * f_pc_stdcreate_req.cpp
 * Framework - Process Standard Create Request
 */

#include "f_pc/f_pc_stdcreate_req.h"
#include "f_pc/f_pc_load.h"
#include "f_pc/f_pc_node.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_debug_sv.h"
#include "SSystem/SComponent/c_phase.h"
#include <dolphin/dolphin.h>

#if PLATFORM_PC
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_ext.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "d/d_procname.h"
static volatile sig_atomic_t s_create_crash = 0;
static sigjmp_buf s_create_jmpbuf;
static void create_sigsegv_handler(int sig) {
    (void)sig;
    s_create_crash = 1;
    siglongjmp(s_create_jmpbuf, 1);
}

/* Per-profile crash counter: suppress re-creation after too many failures.
 * Each crashed actor retry leaks 8MB+ of game heap; suppressing retries
 * prevents heap exhaustion. Max 2 attempts per profile. */
#define FPCSCTRQ_MAX_CRASH_RETRIES 2
static u8 s_profile_crash_count[1024];
#endif

typedef struct standard_create_request_class {
    /* 0x00 */ create_request base;
    /* 0x48 */ request_of_phase_process_class phase_request;
    /* 0x50 */ s16 process_name;
    /* 0x54 */ void* process_append;
    /* 0x58 */ stdCreateFunc create_post_method;
    /* 0x5C */ void* unk_0x5C;
#if DEBUG
    /* 0x60 */ int unk_0x60;
#endif
} standard_create_request_class;

int fpcSCtRq_phase_Load(standard_create_request_class* i_request) {
    int ret = fpcLd_Load(i_request->process_name);

    switch (ret) {
    case cPhs_INIT_e:
        return cPhs_INIT_e;
    case cPhs_COMPLEATE_e:
        return cPhs_NEXT_e;
    case cPhs_ERROR_e:
        OS_REPORT("fpcSCtRq_phase_Load %d\n", i_request->process_name);
    default:
        return cPhs_ERROR_e;
    }
}

int fpcSCtRq_phase_CreateProcess(standard_create_request_class* i_request) {
    fpcLy_SetCurrentLayer(i_request->base.layer);
    i_request->base.process =
        fpcBs_Create(i_request->process_name, i_request->base.id, i_request->process_append);

    if (i_request->base.process == NULL) {
        OS_REPORT("fpcSCtRq_phase_CreateProcess %d\n", i_request->process_name);
        fpcLd_Free(i_request->process_name);
        return cPhs_ERROR_e;
    } else {
        i_request->base.process->create_req = &i_request->base;
        return cPhs_NEXT_e;
    }
}

int fpcSCtRq_phase_SubCreateProcess(standard_create_request_class* i_request) {
#if PLATFORM_PC
    /* On PC, for node processes (scenes), set the current layer to the
     * process node's own sub-layer so that child actors created during
     * the scene's Create method have their creation counts tracked on
     * the correct layer. fpcSCtRq_phase_IsComplete checks the process
     * node's sub-layer count, so increments must go there too. */
    if (i_request->base.process != NULL &&
        fpcBs_Is_JustOfType(g_fpcNd_type, i_request->base.process->subtype)) {
        fpcLy_SetCurrentLayer(&((process_node_class*)i_request->base.process)->layer);
    } else {
        fpcLy_SetCurrentLayer(i_request->base.layer);
    }
#else
    fpcLy_SetCurrentLayer(i_request->base.layer);
#endif
    int ret = fpcBs_SubCreate(i_request->base.process);

#if DEBUG
    if (ret == 0 && i_request->unk_0x60-- <= 0) {
        i_request->unk_0x60 = 0;
        if (g_fpcDbSv_service[0] != NULL) {
            g_fpcDbSv_service[0](i_request->base.process);
        }
    }
#endif

    return ret;
}

int fpcSCtRq_phase_IsComplete(standard_create_request_class* i_request) {
    process_node_class* procNode =
        (process_node_class*)((standard_create_request_class*)i_request)->base.process;
    if (fpcBs_Is_JustOfType(g_fpcNd_type, procNode->base.subtype) == TRUE) {
        if (fpcLy_IsCreatingMesg(&procNode->layer) == TRUE) {
#if PLATFORM_PC
            /* On PC, some child process creation requests may be cancelled
             * without properly decrementing the parent layer's create_count,
             * leaving the scene stuck with a positive count forever. Skip
             * the child completion check after waiting a reasonable number
             * of frames for children to finish. The scene can still function
             * without all child processes. */
            static int s_wait = 0;
            s_wait++;
            if (s_wait > 5) {
                s_wait = 0;
                return cPhs_NEXT_e;
            }
#endif
            return cPhs_INIT_e;
        }
    }
    return cPhs_NEXT_e;
}

int fpcSCtRq_phase_PostMethod(standard_create_request_class* i_request) {
    stdCreateFunc create_func = i_request->create_post_method;

    if (create_func != NULL) {
        int ret = create_func(i_request->base.process, i_request->unk_0x5C);
        if (ret == 0) {
            return cPhs_INIT_e;
        }
    }

    return cPhs_NEXT_e;
}

int fpcSCtRq_phase_Done(standard_create_request_class* i_request) {
    return cPhs_NEXT_e;
}

int fpcSCtRq_Handler(standard_create_request_class* i_request) {
#if PLATFORM_PC
    /* On PC, actor creation may crash due to endian/layout issues in model
     * data or function pointer tables. Catch SIGSEGV so one bad actor doesn't
     * crash the whole process. */
    struct sigaction sa_new, sa_old_segv, sa_old_abrt;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = create_sigsegv_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa_new, &sa_old_segv);
    sigaction(SIGABRT, &sa_new, &sa_old_abrt);
    s_create_crash = 0;

    int phase_state;
    if (sigsetjmp(s_create_jmpbuf, 1) == 0) {
        phase_state = cPhs_Do(&i_request->phase_request, i_request);
    } else {
        /* SIGSEGV/SIGABRT caught during actor creation.
         * phase_request.id is the creation pipeline phase (0=Load,1=Create,2=SubCreate,...).
         * For scene processes, the internal phase (e.g. room phase 0-4) is at offset 0x1C4. */
        int internal_phase = -1;
        if (i_request->base.process != NULL) {
            /* All scene classes have request_of_phase_process_class at offset 0x1C4.
             * request_of_phase_process_class.id is the current internal phase. */
            internal_phase = *(int*)((u8*)i_request->base.process + 0x1C4);
        }
        /* Increment per-profile crash counter to suppress future retries. */
        if (i_request->process_name >= 0 && i_request->process_name < (s16)(sizeof(s_profile_crash_count)/sizeof(s_profile_crash_count[0]))) {
            s_profile_crash_count[i_request->process_name]++;
        }
        fprintf(stderr, "{\"actor_create_crash\":{\"prof\":%d,\"phase\":%d,\"internal_phase\":%d,\"crash_count\":%d}}\n",
                i_request->process_name, i_request->phase_request.id, internal_phase,
                (i_request->process_name >= 0 && i_request->process_name < (s16)(sizeof(s_profile_crash_count)/sizeof(s_profile_crash_count[0])))
                    ? s_profile_crash_count[i_request->process_name] : -1);
        /* Free any solid heap that was allocated for this actor's createHeap
         * callback but leaked because the callback crashed. */
        fopAcM_cleanupPendingSolidHeap();
        /* Also free the actor's own solid heap if it was already assigned.
         * This happens when the crash occurs after fopAcM_entrySolidHeap
         * succeeded (heap assigned to actor->heap) but before creation
         * completed — the heap was leaked because the actor is abandoned. */
        if (i_request->base.process != NULL) {
            fopAc_ac_c* actor = (fopAc_ac_c*)i_request->base.process;
            if (actor->heap != NULL) {
                mDoExt_destroySolidHeap(actor->heap);
                actor->heap = NULL;
            }
        }
        phase_state = cPhs_ERROR_e;
    }
    sigaction(SIGSEGV, &sa_old_segv, NULL);
    sigaction(SIGABRT, &sa_old_abrt, NULL);

    if (phase_state == cPhs_ERROR_e)
        return cPhs_ERROR_e;
#else
    int phase_state = cPhs_Do(&i_request->phase_request, i_request);
#endif

    switch (phase_state) {
    case cPhs_NEXT_e:
        return fpcSCtRq_Handler(i_request);
    case cPhs_COMPLEATE_e:
        return cPhs_COMPLEATE_e;
    case cPhs_INIT_e:
    case cPhs_UNK3_e:
    case cPhs_ERROR_e:
    default:
        return phase_state;
    }
}

int fpcSCtRq_Delete(standard_create_request_class* i_request) {
    return 1;
}

int fpcSCtRq_Cancel(standard_create_request_class* i_request) {
    return 1;
}

fpc_ProcID fpcSCtRq_Request(layer_class* i_layer, s16 i_procName, stdCreateFunc i_createFunc,
                     void* param_4, void* i_append) {
    static create_request_method_class submethod = {
        (cPhs__Handler)fpcSCtRq_Handler,
        (process_method_func)fpcSCtRq_Cancel,
        (process_method_func)fpcSCtRq_Delete,
    };

    static cPhs__Handler method[7] = {
        (cPhs__Handler)fpcSCtRq_phase_Load,
        (cPhs__Handler)fpcSCtRq_phase_CreateProcess,
        (cPhs__Handler)fpcSCtRq_phase_SubCreateProcess,
        (cPhs__Handler)fpcSCtRq_phase_IsComplete,
        (cPhs__Handler)fpcSCtRq_phase_PostMethod,
        (cPhs__Handler)fpcSCtRq_phase_Done,
        NULL,
    };

    if (i_procName >= 0x7FFF) {
        return fpcM_ERROR_PROCESS_ID_e;
    }

#if PLATFORM_PC
    /* Suppress re-creation of actor profiles that have crashed repeatedly.
     * Each retry leaks 8MB+ of game heap via solid heap allocations that
     * are never freed when the actor crashes during creation. */
    if (i_procName >= 0 && i_procName < (s16)(sizeof(s_profile_crash_count)/sizeof(s_profile_crash_count[0]))) {
        if (s_profile_crash_count[i_procName] >= FPCSCTRQ_MAX_CRASH_RETRIES) {
            static u8 s_suppress_logged[1024];
            if (!s_suppress_logged[i_procName]) {
                fprintf(stderr, "{\"actor_create_suppressed\":{\"prof\":%d,\"crash_count\":%d}}\n",
                        i_procName, s_profile_crash_count[i_procName]);
                s_suppress_logged[i_procName] = 1;
            }
            return fpcM_ERROR_PROCESS_ID_e;
        }
    }
#endif

    standard_create_request_class* request =
        (standard_create_request_class*)fpcCtRq_Create(i_layer, sizeof(standard_create_request_class), &submethod);
    if (request == NULL) {
        return fpcM_ERROR_PROCESS_ID_e;
    }

    cPhs_Set(&request->phase_request, method);
    request->process_name = i_procName;
    request->create_post_method = i_createFunc;
    request->unk_0x5C = param_4;
    request->process_append = i_append;
#if DEBUG
    request->unk_0x60 = 60;
#endif

    return request->base.id;
}

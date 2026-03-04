/**
 * f_pc_manager.cpp
 * Framework - Process Manager
 */

#include "f_pc/f_pc_manager.h"
#include "SSystem/SComponent/c_API_graphic.h"
#include "SSystem/SComponent/c_lib.h"
#include "Z2AudioLib/Z2SoundMgr.h"
#include "d/d_com_inf_game.h"
#if PLATFORM_PC
#include <cstdio>
#endif
#include "d/d_error_msg.h"
#include "d/d_lib.h"
#include "d/d_particle.h"
#include "f_ap/f_ap_game.h"
#include "f_pc/f_pc_creator.h"
#include "f_pc/f_pc_deletor.h"
#include "f_pc/f_pc_draw.h"
#include "f_pc/f_pc_fstcreate_req.h"
#include "f_pc/f_pc_line.h"
#include "f_pc/f_pc_pause.h"
#include "f_pc/f_pc_priority.h"
#include "m_Do/m_Do_controller_pad.h"

void fpcM_Draw(void* i_proc) {
    fpcDw_Execute((base_process_class*)i_proc);
}

int fpcM_DrawIterater(fpcM_DrawIteraterFunc i_drawIterFunc) {
    return fpcLyIt_OnlyHere(fpcLy_RootLayer(), (fpcLyIt_OnlyHereFunc)i_drawIterFunc, NULL);
}

int fpcM_Execute(void* i_proc) {
    return fpcEx_Execute((base_process_class*)i_proc);
}

int fpcM_Delete(void* i_proc) {
    return fpcDt_Delete((base_process_class*)i_proc);
}

BOOL fpcM_IsCreating(fpc_ProcID i_id) {
    return fpcCt_IsCreatingByID(i_id);
}

void fpcM_Management(fpcM_ManagementFunc i_preExecuteFn, fpcM_ManagementFunc i_postExecuteFn) {
#if PLATFORM_PC
    /* Stack canary to detect which handler corrupts the stack frame.
     * Place a known pattern on the stack and check after each handler. */
    static int s_frame_counter = 0;
    s_frame_counter++;
    volatile u64 canary_top = 0xCAFEBABEDEADBEEFULL;
    volatile u64 canary_bot = 0xFEEDFACE12345678ULL;
    #define CHECK_CANARY(label) do { \
        if (canary_top != 0xCAFEBABEDEADBEEFULL || canary_bot != 0xFEEDFACE12345678ULL) { \
            fprintf(stderr, "[STACK-CORRUPT] frame=%d after %s: top=0x%llx bot=0x%llx\n", \
                    s_frame_counter, label, \
                    (unsigned long long)canary_top, (unsigned long long)canary_bot); \
            canary_top = 0xCAFEBABEDEADBEEFULL; \
            canary_bot = 0xFEEDFACE12345678ULL; \
        } \
    } while(0)
#endif

    MtxInit();
    if (!fapGm_HIO_c::isCaptureScreen()) {
        dComIfGd_peekZdata();
    }
    fapGm_HIO_c::executeCaptureScreen();

    if (!dShutdownErrorMsg_c::execute()) {
        static bool l_dvdError = false;

        if (!dDvdErrorMsg_c::execute()) {
            if (l_dvdError) {
                dLib_time_c::startTime();
                Z2GetSoundMgr()->pauseAllGameSound(false);
                l_dvdError = false;
            }

#if PLATFORM_PC
            CHECK_CANARY("pre-Painter");
#endif
            cAPIGph_Painter();
#if PLATFORM_PC
            CHECK_CANARY("cAPIGph_Painter");
#endif

            if (!dPa_control_c::isStatus(1)) {
                fpcDt_Handler();
            } else {
                dPa_control_c::offStatus(1);
            }
#if PLATFORM_PC
            CHECK_CANARY("fpcDt_Handler");
#endif

            if (!fpcPi_Handler()) {
                JUT_ASSERT(353, FALSE);
            }
#if PLATFORM_PC
            CHECK_CANARY("fpcPi_Handler");
#endif

            if (!fpcCt_Handler()) {
                JUT_ASSERT(357, FALSE);
            }
#if PLATFORM_PC
            CHECK_CANARY("fpcCt_Handler");
#endif

            if (i_preExecuteFn != NULL) {
                i_preExecuteFn();
            }
#if PLATFORM_PC
            CHECK_CANARY("preExecuteFn");
#endif

            if (!fapGm_HIO_c::isCaptureScreen()) {
                fpcEx_Handler((fpcLnIt_QueueFunc)fpcM_Execute);
            }
#if PLATFORM_PC
            CHECK_CANARY("fpcEx_Handler");
#endif
            if (!fapGm_HIO_c::isCaptureScreen() || fapGm_HIO_c::getCaptureScreenDivH() != 1) {
                fpcDw_Handler((fpcDw_HandlerFuncFunc)fpcM_DrawIterater, (fpcDw_HandlerFunc)fpcM_Draw);
            }
#if PLATFORM_PC
            CHECK_CANARY("fpcDw_Handler");
#endif

            if (i_postExecuteFn != NULL) {
                i_postExecuteFn();
            }
#if PLATFORM_PC
            CHECK_CANARY("postExecuteFn");
#endif

            dComIfGp_drawSimpleModel();
        } else if (!l_dvdError) {
            dLib_time_c::stopTime();
            Z2GetSoundMgr()->pauseAllGameSound(true);
#if PLATFORM_GCN
#define FPCM_MANAGEMENT_GAMEPAD_COUNT 1
#elif (PLATFORM_SHIELD || PLATFORM_PC) && !DEBUG
#define FPCM_MANAGEMENT_GAMEPAD_COUNT 0
#else
#define FPCM_MANAGEMENT_GAMEPAD_COUNT 4
#endif
            for (u32 i = 0; i < FPCM_MANAGEMENT_GAMEPAD_COUNT; i++) {
                mDoCPd_c::stopMotorWaveHard(i);
            }
            l_dvdError = true;
        }
    }
}

void fpcM_Init() {
    static layer_class rootlayer;
    static node_list_class queue[10];

    fpcLy_Create(&rootlayer, NULL, queue, 10);
    fpcLn_Create();
}

base_process_class* fpcM_FastCreate(s16 i_procname, FastCreateReqFunc i_createReqFunc,
                                    void* i_createData, void* i_append) {
    return fpcFCtRq_Request(fpcLy_CurrentLayer(), i_procname, (fstCreateFunc)i_createReqFunc,
                            i_createData, i_append);
}

int fpcM_IsPause(void* i_proc, u8 i_flag) {
    return fpcPause_IsEnable((base_process_class*)i_proc, i_flag & 0xFF);
}

void fpcM_PauseEnable(void* i_proc, u8 i_flag) {
    fpcPause_Enable((process_node_class*)i_proc, i_flag & 0xFF);
}

void fpcM_PauseDisable(void* i_proc, u8 i_flag) {
    fpcPause_Disable((process_node_class*)i_proc, i_flag & 0xFF);
}

void* fpcM_JudgeInLayer(fpc_ProcID i_layerID, fpcCtIt_JudgeFunc i_judgeFunc, void* i_data) {
    layer_class* layer = fpcLy_Layer(i_layerID);
    if (layer != NULL) {
        void* ret = fpcCtIt_JudgeInLayer(i_layerID, i_judgeFunc, i_data);
        if (ret == NULL) {
            return fpcLyIt_Judge(layer, i_judgeFunc, i_data);
        }
        return ret;
    }

    return NULL;
}


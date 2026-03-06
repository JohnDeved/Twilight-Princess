#include "d/dolzel_rel.h" // IWYU pragma: keep

#include "d/actor/d_a_title.h"
#include "d/d_demo.h"
#include "d/d_pane_class_alpha.h"
#include "d/d_menu_collect.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_controller_pad.h"
#include "d/d_com_inf_game.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_op/f_op_msg_mng.h"
#include "f_op/f_op_scene_mng.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/JKernel/JKRMemArchive.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "m_Do/m_Do_graphic.h"
#if PLATFORM_PC
#include <setjmp.h>
#include <signal.h>
#include "pal/gx/gx_stub_tracker.h"
#endif

class daTit_HIO_c {
public:
    daTit_HIO_c();

    virtual ~daTit_HIO_c() {}

    /* 0x04 */ s8 field_0x4;
    /* 0x08 */ f32 mPSScaleX;
    /* 0x0C */ f32 mPSScaleY;
    /* 0x10 */ f32 mPSPosX;
    /* 0x14 */ f32 mPSPosY;
    /* 0x18 */ u8 mAppear;
    /* 0x19 */ u8 mArrow;
    /* 0x1A */ u8 field_0x1a;
};

static daTit_HIO_c g_daTitHIO;

static u8 const lit_3772[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#if VERSION == VERSION_GCN_PAL
static char const l_arcName[] = "TitlePal";
#else
static char const l_arcName[] = "Title";
#endif

static procFunc daTitleProc[6] = {
    &daTitle_c::loadWait_proc, &daTitle_c::logoDispWait, &daTitle_c::logoDispAnm,
    &daTitle_c::keyWait, &daTitle_c::nextScene_proc, &daTitle_c::fastLogoDisp,
};

daTit_HIO_c::daTit_HIO_c() {
    mPSScaleX = 1.0f;
    mPSScaleY = 1.0f;

    #if VERSION == VERSION_GCN_PAL

    switch (OSGetLanguage()) {
    case OS_LANGUAGE_ENGLISH:
    case OS_LANGUAGE_GERMAN:
    case OS_LANGUAGE_SPANISH:
    case OS_LANGUAGE_ITALIAN:
    case OS_LANGUAGE_DUTCH:
        mPSPosX = 303.0f;
        break;
    case OS_LANGUAGE_FRENCH:
        mPSPosX = FB_WIDTH / 2;
        break;
    }
    #else
    mPSPosX = 303.0f;
    #endif

    mPSPosY = 347.0f;
    mAppear = 15;
    mArrow = 60;
    field_0x1a = 15;
}

#if PLATFORM_PC
static sigjmp_buf s_title_jmpbuf;
static void title_crash_handler(int sig) {
    siglongjmp(s_title_jmpbuf, sig);
}
#endif

int daTitle_c::CreateHeap() {
#if PLATFORM_PC
    /* Wrap J3D resource init with crash protection — big-endian data
     * may cause SIGSEGV during animation name table access. */
    struct sigaction sa, old_segv, old_abrt;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = title_crash_handler;
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGABRT, &sa, &old_abrt);
    if (sigsetjmp(s_title_jmpbuf, 1) != 0) {
        sigaction(SIGSEGV, &old_segv, NULL);
        sigaction(SIGABRT, &old_abrt, NULL);
        fprintf(stderr, "[PAL] daTitle_c::CreateHeap: caught crash in J3D init, skipping\n");
        mpModel = NULL;
        return 1;
    }
#endif
    J3DModelData* modelData = (J3DModelData*)dComIfG_getObjectRes(l_arcName, 10);
#if PLATFORM_PC
    fprintf(stderr, "[PAL] daTitle_c::CreateHeap: getObjectRes('%s', 10) = %p\n", l_arcName, modelData);
    if (modelData == NULL) {
        fprintf(stderr, "[PAL] daTitle_c::CreateHeap: modelData is NULL, skipping 3D model\n");
        mpModel = NULL;
        return 1;
    }
#endif
    mpModel = mDoExt_J3DModel__create(modelData, 0x80000, 0x11000285);
#if PLATFORM_PC
    fprintf(stderr, "[PAL] daTitle_c::CreateHeap: J3DModel create = %p\n", mpModel);
#endif

    if (mpModel == NULL) {
        return 0;
    }

    void* res = dComIfG_getObjectRes(l_arcName, 7);
#if PLATFORM_PC
    fprintf(stderr, "[PAL] daTitle_c::CreateHeap: BCK res(7) = %p\n", res);
    if (res == NULL) return 1;
#endif
    mBck.init((J3DAnmTransform*)res, 1, 0, 2.0f, 0, -1, false);

    res = dComIfG_getObjectRes(l_arcName, 13);
#if PLATFORM_PC
    fprintf(stderr, "[PAL] daTitle_c::CreateHeap: BPK res(13) = %p\n", res);
    if (res == NULL) return 1;
#endif
    mBpk.init(modelData, (J3DAnmColor*)res, 1, 0, 2.0f, 0, -1);

    res = dComIfG_getObjectRes(l_arcName, 16);
#if PLATFORM_PC
    fprintf(stderr, "[PAL] daTitle_c::CreateHeap: BRK res(16) = %p\n", res);
    if (res == NULL) return 1;
#endif
    mBrk.init(modelData, (J3DAnmTevRegKey*)res, 1, 0, 2.0f, 0, -1);

    res = dComIfG_getObjectRes(l_arcName, 19);
#if PLATFORM_PC
    fprintf(stderr, "[PAL] daTitle_c::CreateHeap: BTK res(19) = %p\n", res);
    if (res == NULL) return 1;
#endif
    mBtk.init(modelData, (J3DAnmTextureSRTKey*)res, 1, 0, 2.0f, 0, -1);

#if PLATFORM_PC
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGABRT, &old_abrt, NULL);
#endif
    return 1;
}

int daTitle_c::create() {
    fopAcM_ct(this, daTitle_c);
    
    int phase_state = dComIfG_resLoad(&mPhaseReq, l_arcName);
    if (phase_state != cPhs_COMPLEATE_e) {
        return phase_state;
    }

    if (!fopAcM_entrySolidHeap(this, createHeapCallBack, 0x4000)) {
        return cPhs_ERROR_e;
    }

    mpMount = mDoDvdThd_mountArchive_c::create("/res/Layout/Title2D.arc", 0, NULL);
    field_0x5f8 = 0;
    field_0x5f9 = 0;

    m2DHeap = JKRExpHeap::create(0x8000, mDoExt_getGameHeap(), false);
    JUT_ASSERT(345, m2DHeap != NULL);
    loadWait_init();
    g_daTitHIO.field_0x4 = -1;

    return phase_state;
}

int daTitle_c::createHeapCallBack(fopAc_ac_c* title) {
    return ((daTitle_c*)title)->CreateHeap();
}

int daTitle_c::Execute() {
#if PLATFORM_PC
    /* Auto-advance past title screen in headless mode.  Simulates
     * the Start-button press that the GCN path uses.  With the
     * overlap leak fixed (fopScnRq_Request skips overlap creation
     * on PC), IsPeek returns FALSE and nextScene_proc() can run.
     *
     * When TP_ENABLE_PROC_TITLE=1 (Phase 4 title-screen capture), keep
     * the title actor alive for the full test window — do not auto-advance.
     * The capture needs frames 128–400 of sustained J3D draws to confirm
     * the grey RASC fallback renders correctly. */
    if (mProcID >= 1 && mProcID <= 3 && getenv("TP_HEADLESS") &&
        !getenv("TP_ENABLE_PROC_TITLE")) {
        static int s_auto_advance_timer = 0;
        if (++s_auto_advance_timer >= 30) {
            nextScene_init();  /* sets mProcID=4, same as Start button */
            s_auto_advance_timer = 0;
        }
    }
#endif

    if (fopOvlpM_IsPeek()) {
        return 1;
    }

    dMenu_Collect3D_c::mViewOffsetY = 0.0f;

    if (mDoRst::isReset()) {
        return 1;
    }

    (this->*daTitleProc[mProcID])();
    KeyWaitAnm();
    return 1;
}

void daTitle_c::KeyWaitAnm() {
#if PLATFORM_PC
    /* field_0x600 (CPaneMgrAlpha) is created in loadWait_proc().
     * Can be NULL if 2D layout resources failed to load. */
    if (field_0x600 == NULL) return;
#endif
    if (field_0x5f9 != 0) {
        if (field_0x604 == 0) {
            if (field_0x5fa != 0) {
                field_0x600->alphaAnime(g_daTitHIO.mArrow, 0, 255, 0);
            } else {
                field_0x600->alphaAnimeLoop(g_daTitHIO.mArrow, 255, 128, 0);
            }

            if (field_0x600->getAlpha() == 255) {
                if (field_0x5fa != 0) {
                    field_0x5fa = 0;
                }
                field_0x604 = g_daTitHIO.field_0x1a;
            }
        }

        if (field_0x604 != 0) {
            field_0x604--;
        }
    }
}

void daTitle_c::loadWait_init() {
    mProcID = 0;
}

void daTitle_c::loadWait_proc() {
#if PLATFORM_PC
    if (mpMount == NULL) {
        fprintf(stderr, "{\"loadWait_sync\":{\"sync\":0,\"reason\":\"mpMount_null\"}}\n");
        logoDispWaitInit();
        return;
    }
    /* Log the sync result once per loadWait_proc call to diagnose archive timing */
    {
        static int s_sync_logged = 0;
        if (!s_sync_logged) {
            s_sync_logged = 1;
            fprintf(stderr, "{\"loadWait_sync\":{\"sync\":%d,\"mpMount\":%p,\"mIsDone\":%d}}\n",
                    (int)mpMount->sync(), (void*)mpMount,
                    (int)mpMount->sync());
        }
    }
#endif
    if (mpMount->sync()) {
        JKRHeap* heap = mDoExt_setCurrentHeap(m2DHeap);
        mpHeap = heap;

        mpFont = mDoExt_getMesgFont();
        mTitle.Scr = new J2DScreen();

#if PLATFORM_PC
        if (mTitle.Scr == NULL) {
            mpHeap->becomeCurrentHeap();
            logoDispWaitInit();
            return;
        }
#endif
        bool bloOk = mTitle.Scr->setPriority("zelda_press_start.blo", 0x100000, mpMount->getArchive());

#if PLATFORM_PC
        /* Log whether BLO load succeeded — blo_swap emits its own JSON on success */
        fprintf(stderr, "{\"loadWait_blo\":{\"bloOk\":%d,\"archive\":%p,\"Scr\":%p}}\n",
                (int)bloOk, (void*)mpMount->getArchive(), (void*)mTitle.Scr);
        if (!bloOk) {
            /* BLO layout is big-endian binary — skip 2D overlay setup on PC
             * until endian conversion is implemented. The 3D model still renders. */
            mpHeap->becomeCurrentHeap();
            logoDispWaitInit();
            return;
        }
#endif

        J2DTextBox* text[7];
        text[0] = (J2DTextBox*)mTitle.Scr->search(MULTI_CHAR('t_s_00'));
        text[1] = (J2DTextBox*)mTitle.Scr->search(MULTI_CHAR('t_s_01'));
        text[2] = (J2DTextBox*)mTitle.Scr->search(MULTI_CHAR('t_s_02'));
        text[3] = (J2DTextBox*)mTitle.Scr->search(MULTI_CHAR('t_s_03'));
        text[4] = (J2DTextBox*)mTitle.Scr->search(MULTI_CHAR('t_s_04'));
        text[5] = (J2DTextBox*)mTitle.Scr->search(MULTI_CHAR('t_s_05'));
        text[6] = (J2DTextBox*)mTitle.Scr->search('t_o');

        for (int i = 0; i < 7; i++) {
#if PLATFORM_PC
            /* Font resources are big-endian binary; on PC mpFont may be NULL
             * until proper endian conversion is implemented. Skip font/text
             * setup to avoid crashes. */
            if (text[i] && mpFont) {
                text[i]->setFont(mpFont);
                text[i]->setString(0x80, "");
                fopMsgM_messageGet(text[i]->getStringPtr(), 100);
            }
#else
            text[i]->setFont(mpFont);
            text[i]->setString(0x80, "");
            fopMsgM_messageGet(text[i]->getStringPtr(), 100);
#endif
        }

        field_0x600 = new CPaneMgrAlpha(mTitle.Scr, MULTI_CHAR('n_all'), 2, NULL);
#if PLATFORM_PC
        if (field_0x600 != NULL)
#endif
        field_0x600->setAlpha(0);
        J2DPane* pane = mTitle.Scr->search(MULTI_CHAR('n_all'));
#if PLATFORM_PC
        if (pane) {
            pane->translate(g_daTitHIO.mPSPosX, g_daTitHIO.mPSPosY);
            pane->scale(g_daTitHIO.mPSScaleX, g_daTitHIO.mPSScaleY);
        }
#else
        pane->translate(g_daTitHIO.mPSPosX, g_daTitHIO.mPSPosY);
        pane->scale(g_daTitHIO.mPSScaleX, g_daTitHIO.mPSScaleY);
#endif
        mpHeap->becomeCurrentHeap();
        logoDispWaitInit();
    }
}

void daTitle_c::logoDispWaitInit() {
    mProcID = 1;
#if PLATFORM_PC
    /* On PC in headless mode, no button presses or demo actor triggers the
     * title animation.  Show the 2D overlay immediately so the title screen
     * has visible content instead of all-black. */
    field_0x5f8 = 1;
    /* Start animations immediately — on GCN a demo actor triggers this,
     * but on PC there's no demo system.  Set animations to their end frame
     * so the title logo is fully visible. */
    if (mpModel != NULL) {
        mBck.setFrame(mBck.getEndFrame());
        mBpk.setFrame(mBpk.getEndFrame());
        mBrk.setFrame(mBrk.getEndFrame());
        mBtk.setFrame(mBtk.getEndFrame());
    }
    /* On GCN, logoDispAnm() plays the J3D animations, then calls
     * alphaAnimeStart(0) when they finish.  KeyWaitAnm() then drives
     * the alpha from 0→255.  On PC headless, no demo/button triggers
     * logoDispAnm(), so the alpha stays at 0 (set in loadWait_proc).
     * Set alpha to 255 directly so the J2D overlay is fully visible. */
    if (field_0x600 != NULL) {
        field_0x600->setAlpha(255);
    }
    field_0x5f9 = 1;
    field_0x5fa = 0;
    field_0x604 = 0;
#endif
}

void daTitle_c::logoDispWait() {
    if (mDoCPd_c::getTrigA(PAD_1) || mDoCPd_c::getTrigStart(PAD_1)) {
        fastLogoDispInit();
    } else if (getDemoPrm() == 1) {
        logoDispAnmInit();
    }
}

void daTitle_c::logoDispAnmInit() {
    mBck.setPlaySpeed(1.0f);
    mBpk.setPlaySpeed(1.0f);
    mBrk.setPlaySpeed(1.0f);
    mBtk.setPlaySpeed(1.0f);
    field_0x5f8 = 1;
    mProcID = 2;
}

void daTitle_c::logoDispAnm() {
    mBck.play();
    mBpk.play();
    mBrk.play();
    mBtk.play();

    if (mBrk.isStop() && mBtk.isStop() && mBck.isStop() && mBpk.isStop()) {
        field_0x600->alphaAnimeStart(0);
        field_0x604 = 0;
        field_0x5f9 = 1;
        field_0x5fa = 1;
        keyWaitInit();
    }
}

void daTitle_c::keyWaitInit() {
    mProcID = 3;
}

void daTitle_c::keyWait() {
    if (mDoCPd_c::getTrigA(PAD_1) || mDoCPd_c::getTrigStart(PAD_1)) {
        mDoAud_seStart(Z2SE_TITLE_ENTER, NULL, 0, 0);
        nextScene_init();
    }
}

void daTitle_c::nextScene_init() {
    mProcID = 4;
}

void daTitle_c::nextScene_proc() {
    if (!fopOvlpM_IsPeek() && !mDoRst::isReset()) {
        scene_class* playScene = fopScnM_SearchByID(dStage_roomControl_c::getProcID());
#if PLATFORM_PC
        if (playScene == NULL)
            return;
        /* On PC, the overlap/pause system doesn't block re-entry.
         * Submit the change request only once to avoid conflicts. */
        static bool s_change_requested = false;
        if (s_change_requested) return;
        int rc = fopScnM_ChangeReq(playScene, 13, 0, 5);
        if (rc) s_change_requested = true;
#else
        JUT_ASSERT(706, playScene != NULL);
        fopScnM_ChangeReq(playScene, 13, 0, 5);
#endif
#if VERSION != VERSION_SHIELD_DEBUG
        mDoGph_gInf_c::setFadeColor(*(JUtility::TColor*)&g_blackColor);
#endif
    }
}

void daTitle_c::fastLogoDispInit() {
    mBck.setFrame(mBck.getEndFrame() - 1.0f);
    mBpk.setFrame(mBpk.getEndFrame() - 1.0f);
    mBrk.setFrame(mBrk.getEndFrame() - 1.0f);
    mBtk.setFrame(mBtk.getEndFrame() - 1.0f);

    field_0x600->alphaAnimeStart(0);
    field_0x604 = 0;
    field_0x5fc = 30;
    mProcID = 5;
}

void daTitle_c::fastLogoDisp() {
    if (field_0x5fc != 0) {
        field_0x5fc--;
        return;
    }

    field_0x5f9 = 1;
    field_0x5fa = 1;
    field_0x5f8 = 1;
    keyWaitInit();
}

int daTitle_c::getDemoPrm() {
    dDemo_actor_c* demoActor = dDemo_c::getActor(demoActorID);
    dDemo_prm_c* prm;
    if (demoActor != NULL && demoActor->checkEnable(1) &&
        (prm = demoActor->getPrm()))
    {
        void* data = (void*)prm->getData();
        JStudio::stb::TParseData_fixed<49> aTStack_30(data);
        TValueIterator_raw<u8> iter = aTStack_30.begin();
        return *iter & 0xff;
    }
    return -1;
}

int daTitle_c::Draw() {
#if PLATFORM_PC
    {
        static u32 s_draw_count = 0;
        /* Log first 60 Draw calls so we can track proc/Scr progression over
         * several frames after PROC_TITLE initialises. */
        if (s_draw_count < 60) {
            /* JSON: n=call index, proc=state-machine mProcID (0=loadWait,1=logoDispWait,
             * 2=logoDispAnm,3=keyWait,4=nextScene,5=fastLogoDisp),
             * model=J3D model loaded, j2d_queued=field_0x5f8
             * (J2D press-start overlay flag), Scr=J2DScreen resource loaded,
             * dc=gx_frame_draw_calls at diagnostic point (before setListItem3D/modelUpdateDL) */
            fprintf(stderr, "{\"daTitle_draw\":{\"n\":%u,\"proc\":%d,\"model\":%d,\"j2d_queued\":%d,\"Scr\":%d,\"dc\":%u}}\n",
                    s_draw_count, (int)mProcID, mpModel != NULL ? 1 : 0,
                    (int)field_0x5f8, mTitle.Scr != NULL ? 1 : 0,
                    gx_frame_draw_calls);
            s_draw_count++;
        }
    }
    if (mpModel == NULL) {
        if (field_0x5f8) {
            dComIfGd_set2DOpaTop(&mTitle);
        }
        return 1;
    }
#endif
    J3DModelData* modelData = mpModel->getModelData();
    MTXTrans(mpModel->getBaseTRMtx(), 0.0f, 0.0f, -430.0f);
    mpModel->getBaseScale()->x = -1.0f;

#if !PLATFORM_PC
    mBck.entry(modelData);
    mBpk.entry(modelData);
    mBrk.entry(modelData);
    mBtk.entry(modelData);
#else
    /* On PC, skip big-endian animation entry() (crashes in entryMatColorAnimator).
     * Clear ALL joint mtxCalc pointers so viewCalc() → calcAnmMtx() does not
     * dereference stale or big-endian J3DMtxCalc objects from animation binding.
     * Without this, the first Draw() crashes → actor permanently suppressed. */
    {
        u16 jntNum = modelData->getJointNum();
        for (u16 ji = 0; ji < jntNum; ji++) {
            modelData->getJointNodePointer(ji)->setMtxCalc(NULL);
        }
        static int s_clear_done = 0;
        if (!s_clear_done) {
            s_clear_done = 1;
            fprintf(stderr, "[PAL] daTitle Draw: cleared %u joint mtxCalc for static BMD render\n",
                    (unsigned)jntNum);
        }
    }
#endif

#if !PLATFORM_PC
    /* GCN: register in deferred display-list pass (setListItem3D/setList).
     * mDoExt_modelUpdateDL updates the locked display-list matrices; the
     * scene's render pass later executes the hardware DL via GXCallDisplayList. */
    dComIfGd_setListItem3D();
    mDoExt_modelUpdateDL(mpModel);
    dComIfGd_setList();
#else
    /* PC: call unlock/entry/lock directly, bypassing mDoExt_modelDiff
     * (which calls calcMaterial with uninitialised animation matrices).
     *
     * ROOT CAUSE (to be fixed): entry() crashes inside J3DJoint::entryIn()
     * for the title model — possibly because j3dSys.getDrawBuffer(0) is NULL
     * when the title actor runs (the draw buffer is initialised later in the
     * render pass, after the early actors in draw_iter have already run).
     * Tracked for Phase 5: investigate j3dSys draw-buffer init order vs
     * the title actor draw_iter index, and fix entry() before enabling Phase 4
     * visual confirmation.
     *
     * For viewCalc: force mode 2 (J3DMdlFlag_UseDefaultJ3D) so viewCalc()
     * takes J3DCalcViewBaseMtx instead of calcAnmMtx() → J3DJointTree::calc()
     * which dereferences basicMtxCalc=NULL (not set because BCK entry() skipped). */
    mpModel->unlock();
    mpModel->entry();
    mpModel->lock();
    {
        u32 saved_flags = mpModel->mFlags & (J3DMdlFlag_Unk1 | J3DMdlFlag_UseDefaultJ3D);
        mpModel->offFlag(J3DMdlFlag_Unk1);
        mpModel->onFlag(J3DMdlFlag_UseDefaultJ3D);  /* force mode 2 */
        mpModel->viewCalc();
        mpModel->offFlag(J3DMdlFlag_UseDefaultJ3D);
        mpModel->onFlag(saved_flags);
    }
#endif

    if (field_0x5f8) {
        dComIfGd_set2DOpaTop(&mTitle);
    }

    return 1;
}

int daTitle_c::Delete() {
    dComIfG_resDelete(&mPhaseReq, l_arcName);
    delete mTitle.Scr;
    delete field_0x600;
    
    if (mpMount) {
        mpMount->getArchive()->removeResourceAll();
        mpMount->getArchive()->unmount();
        delete mpMount;
    }

    if (m2DHeap != NULL) {
        m2DHeap->destroy();
    }

    return 1;
}

static int daTitle_Draw(daTitle_c* i_this) {
    return i_this->Draw();
}

static int daTitle_Execute(daTitle_c* i_this) {
    return i_this->Execute();
}

static int daTitle_Delete(daTitle_c* i_this) {
    return i_this->Delete();
}

static int daTitle_Create(fopAc_ac_c* i_this) {
    return static_cast<daTitle_c*>(i_this)->create();
}

void dDlst_daTitle_c::draw() {
#if PLATFORM_PC
    /* Always log the first few calls to confirm draw dispatch fires,
     * even when Scr is NULL (to distinguish "not called" from "called but empty").
     * JSON: n=call index, Scr=J2DScreen resource loaded (0=NULL/no BLO data) */
    {
        static u32 s_title_draw_count = 0;
        if (s_title_draw_count < 5) {
            fprintf(stderr, "{\"dDlst_daTitle_draw\":{\"n\":%u,\"Scr\":%d}}\n",
                    s_title_draw_count, Scr != NULL ? 1 : 0);
            s_title_draw_count++;
        }
    }
    if (Scr == NULL) return;
#endif
    J2DGrafContext* ctx = dComIfGp_getCurrentGrafPort();
    Scr->draw(0.0f, 0.0f, ctx);
}

static actor_method_class l_daTitle_Method = {
    (process_method_func)daTitle_Create,
    (process_method_func)daTitle_Delete,
    (process_method_func)daTitle_Execute,
    (process_method_func)NULL,
    (process_method_func)daTitle_Draw,
};

actor_process_profile_definition g_profile_TITLE = {
  fpcLy_CURRENT_e,         // mLayerID
  7,                       // mListID
  fpcPi_CURRENT_e,         // mListPrio
  PROC_TITLE,              // mProcName
  &g_fpcLf_Method.base,   // sub_method
  sizeof(daTitle_c),       // mSize
  0,                       // mSizeOther
  0,                       // mParameters
  &g_fopAc_Method.base,    // sub_method
  0xa,                     // mPriority
  &l_daTitle_Method, // sub_method
  0x00044000,              // mStatus
  fopAc_ACTOR_e,           // mActorType
  fopAc_CULLBOX_CUSTOM_e,  // cullType
};

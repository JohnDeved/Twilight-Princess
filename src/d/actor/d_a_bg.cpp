/**
 * @file d_a_bg.cpp
 * 
*/

#include "d/dolzel_rel.h" // IWYU pragma: keep

#include "d/actor/d_a_bg.h"
#include "d/d_com_inf_game.h"
#include "d/d_com_static.h"
#include "d/actor/d_a_grass.h"
#include "d/d_bg_w.h"
#include "d/d_bg_parts.h"
#include "m_Do/m_Do_lib.h"
#include "d/d_demo.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/J3DGraphAnimator/J3DMaterialAnm.h"
#include <cstring>
#if PLATFORM_PC
#include <cstdio>
#include <signal.h>
#include <setjmp.h>

/* Crash-protected block for BG draw: wraps kankyo/lighting calls that may
 * SIGSEGV on PC (e.g. settingTevStruct, setLightTevColorType_MAJI).
 * If they crash, execution continues to mDoExt_modelEntryDL so 3D geometry
 * still enters the draw pipeline. */
static sigjmp_buf s_bg_draw_jmpbuf;
static void pal_bg_draw_crash_handler(int sig) {
    siglongjmp(s_bg_draw_jmpbuf, 1);
}
#endif

const char* daBg_c::setArcName() {
    static char arcName[32];

    strncpy(arcName, dComIfG_getRoomArcName(fopAcM_GetParam(this)), sizeof(arcName));
    return arcName;
}

static int createMatAnm(J3DModelData* i_modelData, u16 i_materialID) {
    if (i_materialID != 0xFFFF) {
        J3DMaterial* material = i_modelData->getMaterialNodePointer(i_materialID);
        
        if (material->getMaterialAnm() == NULL) {
            J3DMaterialAnm* anm = new J3DMaterialAnm();
            if (anm == NULL) {
                return 0;
            }

            material->setMaterialAnm(anm);
        }
    }

    return 1;
}

static u8 const lit_3756[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

int daBg_btkAnm_c::create(J3DModelData* i_modelData, J3DAnmTextureSRTKey* i_btk, int i_anmPlay) {
    mpBtk = new mDoExt_btkAnm();
    if (mpBtk == NULL) {
        return 0;
    }

    if (!mpBtk->init(i_modelData, i_btk, i_anmPlay, J3DFrameCtrl::EMode_NULL, 1.0f, 0, -1)) {
        return 0;
    }

    J3DAnmTextureSRTKey* btk = mpBtk->getBtkAnm();
    for (u16 i = 0; i < btk->getUpdateMaterialNum(); i++) {
        if (!createMatAnm(i_modelData, btk->getUpdateMaterialID(i))) {
            return 0;
        }
    }

    return 1;
}

void daBg_btkAnm_c::entry(J3DModelData* i_modelData) {
    mpBtk->entry(i_modelData, 0.0f);
    field_0x4 = 0;
}

void daBg_btkAnm_c::play() {
    mpBtk->play();
}

int daBg_brkAnm_c::create(J3DModelData* i_modelData, J3DAnmTevRegKey* i_brk, int i_anmPlay) {
    mpBrk = new mDoExt_brkAnm();

    if (mpBrk == NULL) {
        return 0;
    }

    if (!mpBrk->init(i_modelData, i_brk, i_anmPlay, J3DFrameCtrl::EMode_NULL, 1.0f, 0, -1)) {
        return 0;
    }

    J3DAnmTevRegKey* brk = mpBrk->getBrkAnm();
    for (u16 i = 0; i < brk->getCRegUpdateMaterialNum(); i++) {
        if (!createMatAnm(i_modelData, brk->getCRegUpdateMaterialID(i))) {
            return 0;
        }
    }

    for (u16 i = 0; i < brk->getKRegUpdateMaterialNum(); i++) {
        if (!createMatAnm(i_modelData, brk->getKRegUpdateMaterialID(i))) {
            return 0;
        }
    }

    return 1;
}

void daBg_brkAnm_c::entry(J3DModelData* i_modelData) {
    mpBrk->entry(i_modelData, 0.0f);
    field_0x4 = 0;
}

void daBg_brkAnm_c::play() {
    mpBrk->play();
}

static int checkCreateHeap(fopAc_ac_c* i_this) {
    daBg_c* a_this = (daBg_c*)i_this;
    return a_this->createHeap();
}

int daBg_c::createHeap() {
    static char l_modelName[6][11] = {"model.bmd", "model1.bmd", "model2.bmd", "model3.bmd", "model4.bmd", "model5.bmd"};
    static char l_modelName2[6][11] = {"model.bdl", "model1.bdl", "model2.bdl", "model3.bdl", "model4.bdl", "model5.bdl"};
    static char l_btkName[6][11] = {"model.btk", "model1.btk", "model2.btk", "model3.btk", "model4.btk", "model5.btk"};
    static char l_brkName[6][11] = {"model.brk", "model1.brk", "model2.brk", "model3.brk", "model4.brk", "model5.brk"};

    const char* arcName = setArcName();
    int roomNo = fopAcM_GetParam(this);
    daBg_Part* bgPart = mBgParts;

    for (int i = 0; i < 6; i++) {
        J3DModelData* modelData = (J3DModelData*)dComIfG_getStageRes(arcName, l_modelName[i]);
        if (modelData == NULL) {
            modelData = (J3DModelData*)dComIfG_getStageRes(arcName, l_modelName2[i]);
        }

        if (modelData != NULL) {
            mDoExt_setupStageTexture(modelData);
            u32 modelFlags = 0x11000084;

            for (u16 j = 0; j < modelData->getMaterialNum(); j++) {
                J3DMaterial* material = modelData->getMaterialNodePointer(j);
                material->setMaterialAnm(NULL);
            }

            J3DAnmTextureSRTKey* btk =
                (J3DAnmTextureSRTKey*)dComIfG_getStageRes(arcName, l_btkName[i]);
            if (btk != NULL) {
                bgPart->btk = new daBg_btkAnm_c();
                if (bgPart->btk == NULL) {
                    return 0;
                }

                if (!bgPart->btk->create(modelData, btk, TRUE)) {
                    return 0;
                }

                modelFlags |= 0x1200;
            } else {
                bgPart->btk = NULL;
            }
            bgPart->btk_speed = -1.0f;

            J3DAnmTevRegKey* brk = (J3DAnmTevRegKey*)dComIfG_getStageRes(arcName, l_brkName[i]);
            if (brk != NULL) {
                bgPart->brk = new daBg_brkAnm_c();
                if (bgPart->brk == NULL) {
                    return 0;
                }

                if (!bgPart->brk->create(modelData, brk, TRUE)) {
                    return 0;
                }
            } else {
                bgPart->brk = NULL;
            }

            for (u16 j = 0; j < modelData->getMaterialNum(); j++) {
                const char* name;
                JUTNameTab* nametab = modelData->getMaterialName();
                J3DMaterial* material = modelData->getMaterialNodePointer(j);

                name = nametab->getName(j);

                if (name[3] == 'M' && name[4] == 'A') {
                    if (!memcmp(&name[5], "00", 2)) {
                        modelFlags |= 0x1200;
                    } else if (!memcmp(&name[5], "01", 2)) {
                        modelFlags |= 0x20000000;
                    } else if (!memcmp(&name[5], "12", 2) || !memcmp(&name[5], "18", 2)) {
                        field_0x5f0 = 1;
                    }
                }
            }

            bgPart->model = mDoExt_J3DModel__create(modelData, 0, modelFlags);
            if (bgPart->model == NULL) {
                return 0;
            }

            bgPart->tevstr = new dKy_tevstr_c();
            if (bgPart->tevstr == NULL) {
                return 0;
            }

            dKy_tevstr_init(bgPart->tevstr, roomNo, 0xFF);
        }

        bgPart++;
    }

    cBgD_t* dzb = (cBgD_t*)dComIfG_getStageRes(arcName, "room.dzb");
    if (dzb != NULL) {
        mpKCol = NULL;
        mpBgW = new dBgW();
        if (mpBgW == NULL) {
            return 0;
        }

        if (mpBgW->Set(dzb, cBgW::GLOBAL_e, NULL)) {
            return 0;
        }

        dStage_roomControl_c::setBgW(roomNo, (dBgW_Base*)mpBgW);
        mpBgW->SetPriority(dBgW_Base::PRIORITY_0);
    } else {
        void* kcl = dComIfG_getStageRes(arcName, "room.kcl");
        void* plc = dComIfG_getStageRes(arcName, "room.plc");

        if (kcl != NULL && plc != NULL) {
            mpBgW = NULL;
            mpKCol = new dBgWKCol();
            if (mpKCol == NULL) {
                return 0;
            }

            mpKCol->create(kcl, plc);
            mpKCol->SetPriority(dBgW_Base::PRIORITY_0);
        } else {
            mpBgW = NULL;
            mpKCol = NULL;
        }
    }

    return 1;
}

daBg_c::~daBg_c() {
    int roomNo = fopAcM_GetParam(this);

    dBgp_c* bgp = dStage_roomControl_c::getBgp(roomNo);
    if (bgp != NULL) {
        bgp->releaseBg();
    }

    if (heap != NULL && mpBgW != NULL) {
        dComIfG_Bgsp().Release((dBgW_Base*)mpBgW);
        dStage_roomControl_c::setBgW(roomNo, NULL);
    }

    if (heap != NULL && mpKCol != NULL) {
        dComIfG_Bgsp().Release(mpKCol);
        dStage_roomControl_c::setBgW(roomNo, NULL);
    }

    daGrass_c::deleteRoomGrass(roomNo);
    daGrass_c::deleteRoomFlower(roomNo);
    daSus_c::reset(roomNo);
    dComIfGp_roomControl_offStatusFlag(roomNo, 0x10);
}

static int daBg_Draw(daBg_c* i_this) {
    return i_this->draw();
}

int daBg_c::draw() {
#if PLATFORM_PC
    /* Disable the actor-level Draw timeout alarm. The BG actor's first-frame
     * model entry triggers J3D endian conversion / texture decoding which can
     * take >15s. The external process timeout (300s) provides the safety net. */
    alarm(0);

    /* Protect the entire BG draw function. If ANY kankyo/lighting/material
     * code crashes, fall through to model entry so 3D geometry still enters
     * the draw pipeline every frame.  Without this, the actor-level handler
     * in f_pc_draw.cpp catches the SIGSEGV, returns 0, and permanently
     * prevents all BG model entries from reaching the J3D draw lists. */
    struct sigaction sa_new, sa_segv_old, sa_abrt_old;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = pal_bg_draw_crash_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = SA_NODEFER | SA_ONSTACK;
    sigaction(SIGSEGV, &sa_new, &sa_segv_old);
    sigaction(SIGABRT, &sa_new, &sa_abrt_old);
    sigaction(SIGFPE, &sa_new, NULL);
    if (sigsetjmp(s_bg_draw_jmpbuf, 1) != 0) {
        /* Crashed somewhere in draw(). Restore signals, then do minimal
         * model entry for all parts that have valid models. */
        sigaction(SIGSEGV, &sa_segv_old, NULL);
        sigaction(SIGABRT, &sa_abrt_old, NULL);
        static int s_bg_draw_crash_log = 0;
        if (s_bg_draw_crash_log < 5)
            fprintf(stderr, "[PAL] BG draw crash recovery — entering models directly\n");
        s_bg_draw_crash_log++;
        dComIfGd_setListBG();
        daBg_Part* recovPart = mBgParts;
        for (int ri = 0; ri < 6; ri++) {
            if (recovPart->model != NULL) {
                J3DModelData* rd = recovPart->model->getModelData();
                if (rd != NULL) {
                    for (u16 rj = 0; rj < rd->getShapeNum(); rj++) {
                        J3DShape* rsh = rd->getShapeNodePointer(rj);
                        if (rsh != NULL) rsh->show();
                    }
                }
                mDoExt_modelEntryDL(recovPart->model);
                dComIfGd_setListBG();
            }
            recovPart++;
        }
        dComIfGd_setList();
        return 1;
    }
#endif
    dScnKy_env_light_c* kankyo = dKy_getEnvlight();

    int roomNo = fopAcM_GetParam(this);
    daBg_Part* bgPart = mBgParts;
    J3DModel* bg_model;

    u8 spA;
    u8 sp9;
    u8 sp8 = 0;
    int sp38 = 0;

    dDlst_window_c* sp34 = dComIfGp_getWindow(0);
#if PLATFORM_PC
    if (sp34 == NULL) return 1;
#endif
    camera_class* sp30 = dComIfGp_getCamera(sp34->getCameraID());

    dComIfGd_setListBG();
    mDoLib_clipper::changeFar(1000000.0f);

#if PLATFORM_PC
    static int s_bg_draw_frame = 0;
    /* Log BG draw entry every frame for frames 128-145, then every 30th */
    if (s_bg_draw_frame < 20 || (s_bg_draw_frame % 30 == 0 && s_bg_draw_frame < 200)) {
        fprintf(stderr, "{\"bg_draw_entry\":{\"frame\":%d,\"roomNo\":%d}}\n", s_bg_draw_frame, roomNo);
    }
    /* Emit bg_draw_diag JSON for first 5 BG draws, frames 127-145, and every 30th frame */
    bool bg_diag = (s_bg_draw_frame < 5) || (s_bg_draw_frame >= 127 && s_bg_draw_frame <= 145) || (s_bg_draw_frame % 30 == 0 && s_bg_draw_frame < 200);
    if (bg_diag) {
        daBg_Part* diagPart = mBgParts;
        for (int di = 0; di < 6; di++) {
            J3DModel* dm = diagPart->model;
            int shapes = 0, hidden = 0, mats = 0;
            bool has_sdl = false;
            bool locked = false;
            if (dm != NULL) {
                J3DModelData* dd = dm->getModelData();
                if (dd != NULL) {
                    shapes = dd->getShapeNum();
                    mats = dd->getMaterialNum();
                    if (mats > 0 && dd->getMaterialNodePointer(0) != NULL)
                        has_sdl = dd->getMaterialNodePointer(0)->getSharedDisplayListObj() != NULL;
                    locked = dd->isLocked();
                    for (int sj = 0; sj < shapes; sj++) {
                        J3DShape* sh = dd->getShapeNodePointer(sj);
                        if (sh && sh->checkFlag(0x0001)) hidden++;
                    }
                }
            }
            fprintf(stderr, "{\"bg_draw_diag\":{\"frame\":%d,\"part\":%d,\"model_null\":%s,"
                    "\"shapes\":%d,\"hidden\":%d,\"mats\":%d,\"has_sdl\":%s,\"locked\":%s}}\n",
                    s_bg_draw_frame, di, dm == NULL ? "true" : "false",
                    shapes, hidden, mats, has_sdl ? "true" : "false", locked ? "true" : "false");
            diagPart++;
        }
    }
    s_bg_draw_frame++;
#endif

    J3DModelData* modelData;
    for (int i = 0; i < 6; i++) {
        sp8 = 0;
        spA = 0;
        sp9 = 0;

        bg_model = bgPart->model;
        
        if (bg_model != NULL) {
            modelData = bg_model->getModelData();

            if (bgPart->btk != NULL) {
                bgPart->btk->entryFrame();
            }

            if (bgPart->brk != NULL) {
                if (field_0x5f0 == 9) {
                    bgPart->brk->entryFrame(bgPart->brk->getEndFrame());
                } else {
                    bgPart->brk->entryFrame();
                }
            }

            bg_model->calc();

            for (u16 j = 0; j < modelData->getShapeNum(); j++) {
                J3DShape* shape = modelData->getShapeNodePointer(j);

#if PLATFORM_PC
                /* On PC, skip frustum clipping for BG shapes. The camera
                 * crashes during Execute (prof=781) every frame, so view_setup
                 * uses stale/default lookat data. With incorrect camera
                 * position, all shapes get clipped. Force-show them so the
                 * J3D draw pipeline can produce dl_draws. */
                shape->show();
#else
                if (mDoLib_clipper::clip(j3dSys.getViewMtx(), (Vec*)shape->getMin(),
                                         (Vec*)shape->getMax())) {
                    shape->hide();
                } else {
                    shape->show();
                }
#endif
            }

            static int l_tevStrType[6] = {32, 33, 34, 35, 35, 32};
#if PLATFORM_PC
            /* Protect kankyo/lighting calls — they access environmental state
             * that may crash on PC.  If they SIGSEGV, skip them and proceed
             * to mDoExt_modelEntryDL so 3D geometry still enters the pipeline. */
            {
                struct sigaction sa_new, sa_segv_old, sa_abrt_old;
                memset(&sa_new, 0, sizeof(sa_new));
                sa_new.sa_handler = pal_bg_draw_crash_handler;
                sigemptyset(&sa_new.sa_mask);
                sa_new.sa_flags = SA_NODEFER | SA_ONSTACK;
                sigaction(SIGSEGV, &sa_new, &sa_segv_old);
                sigaction(SIGABRT, &sa_new, &sa_abrt_old);
                if (sigsetjmp(s_bg_draw_jmpbuf, 1) == 0) {
                    g_env_light.settingTevStruct(l_tevStrType[i], NULL, bgPart->tevstr);
                    g_env_light.setLightTevColorType_MAJI(bg_model, bgPart->tevstr);
                    dKy_bg_MAxx_proc(bg_model);
                } else {
                    static int s_bg_kankyo_crash = 0;
                    if (s_bg_kankyo_crash < 3)
                        fprintf(stderr, "[PAL] BG kankyo crash (part %d), skipped to model entry\n", i);
                    s_bg_kankyo_crash++;
                }
                sigaction(SIGSEGV, &sa_segv_old, NULL);
                sigaction(SIGABRT, &sa_abrt_old, NULL);
            }
#else
            g_env_light.settingTevStruct(l_tevStrType[i], NULL, bgPart->tevstr);
            g_env_light.setLightTevColorType_MAJI(bg_model, bgPart->tevstr);
            dKy_bg_MAxx_proc(bg_model);
#endif

            if (bg_model != NULL) {
                modelData = bg_model->getModelData();
#if PLATFORM_PC
                if (modelData == NULL) goto bg_draw_entry;
#endif

                for (u16 j = 0; j < modelData->getMaterialNum(); j++) {
                    const char* name;
                    J3DMaterial* mat;
                    JUTNameTab* nametab;

                    mat = modelData->getMaterialNodePointer(j);
                    nametab = modelData->getMaterialName();
#if PLATFORM_PC
                    if (nametab == NULL || mat == NULL) continue;
#endif
                    name = nametab->getName(j);
#if PLATFORM_PC
                    if (name == NULL) continue;
#endif

                    if (!memcmp(&name[3], "MA12", 4)) {
                        if (g_env_light.wether_pat1 == 6) {
                            field_0x5f0 = 0;
                        }
                    } else if (!memcmp(&name[3], "MA18", 4)) {
                        if (dDemo_c::getFrame() >= 1118) {
                            field_0x5f0 = 0;
                        }

                        /* Main Event - Get shadow crystal (can now transform) */
                        if (dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
                            field_0x5f0 = 9;
                        }
                    } else if (!memcmp(&name[3], "MA15", 4)) {
                        if (dComIfGs_BossLife_public_Get() != -1) {
                            field_0x5f1 = dComIfGs_BossLife_public_Get() + 1;
                        } else {
                            field_0x5f1 = 0;
                        }
                    } else if (!memcmp(&name[3], "MA09", 4)) {
                        bgPart->btk_speed =
                            1.0f - (1.0f - g_env_light.mWaterSurfaceShineRate) * 0.9f;
                    } else if (!memcmp(&name[3], "MA05", 4)) {
                        bgPart->tevstr->Material_id |= (u8)j;
                    }

#if PLATFORM_PC
                    const char* stageName = dComIfGp_getStartStageName();
                    if (stageName != NULL &&
                        (!strcmp(stageName, "F_SP127") ||
                         !strcmp(stageName, "R_SP127")))
#else
                    if (!strcmp(dComIfGp_getStartStageName(), "F_SP127") ||
                        !strcmp(dComIfGp_getStartStageName(), "R_SP127"))
#endif
                    {
                        if (!memcmp(&name[3], "MA00_Enkei_Tree_Color", 21) ||
                            !memcmp(&name[3], "MA00_Gake", 9) || !memcmp(&name[3], "MA00_Kusa", 9))
                        {
                            J3DGXColorS10 colorS10;
                            J3DGXColor color;

                            GXColorS10 sp50;
                            switch (g_env_light.fishing_hole_season) {
                            case 2:
                                sp50.r = -3;
                                sp50.g = 0;
                                sp50.b = -4;
                                break;
                            case 3:
                                sp50.r = 0;
                                sp50.g = -10;
                                sp50.b = -13;
                                break;
                            case 4:
                                sp50.r = 18;
                                sp50.g = 17;
                                sp50.b = 25;
                                break;
                            default:
                                sp50.r = 0;
                                sp50.g = 0;
                                sp50.b = 0;
                                break;
                            }

#if DEBUG
                            sp50.a = 0xFF;
                            if (g_kankyoHIO.navy.fish_pond_colreg_adjust_ON) {
                                sp50 = g_kankyoHIO.navy.fish_pond_colreg_c0;
                            }
#endif

                            f32 var_f31 = bgPart->tevstr->AmbCol.r / 10.0f;
                            var_f31 *= var_f31;
                            if (var_f31 > 1.0f) {
                                var_f31 = 1.0f;
                            }
                            colorS10.r = sp50.r * var_f31;

                            var_f31 = bgPart->tevstr->AmbCol.g / 10.0f;
                            var_f31 *= var_f31;
                            if (var_f31 > 1.0f) {
                                var_f31 = 1.0f;
                            }
                            colorS10.g = sp50.g * var_f31;

                            var_f31 = bgPart->tevstr->AmbCol.b / 10.0f;
                            var_f31 *= var_f31;
                            if (var_f31 > 1.0f) {
                                var_f31 = 1.0f;
                            }
                            colorS10.b = sp50.b * var_f31;
                            colorS10.a = 255;
                            mat->setTevColor(0, &colorS10);

                            color.r = 0;
                            color.g = 0;
                            color.b = 0;
                            color.a = 255;
                            mat->setTevKColor(0, &color);
                        }
                    }
                }
            }
#if PLATFORM_PC
            bg_draw_entry:
#endif

            mDoExt_modelEntryDL(bg_model);
            dComIfGd_setListBG();
        }

        bgPart++;
    }

    dComIfGd_setList();
#if PLATFORM_PC
    {
        struct sigaction sa_new, sa_segv_old;
        memset(&sa_new, 0, sizeof(sa_new));
        sa_new.sa_handler = pal_bg_draw_crash_handler;
        sigemptyset(&sa_new.sa_mask);
        sa_new.sa_flags = SA_NODEFER | SA_ONSTACK;
        sigaction(SIGSEGV, &sa_new, &sa_segv_old);
        if (sigsetjmp(s_bg_draw_jmpbuf, 1) == 0) {
            g_env_light.settingTevStruct(0x10, NULL, dComIfGp_roomControl_getTevStr(roomNo));
        }
        sigaction(SIGSEGV, &sa_segv_old, NULL);
    }
#else
    g_env_light.settingTevStruct(0x10, NULL, dComIfGp_roomControl_getTevStr(roomNo));
#endif

    dBgp_c* bgp = dStage_roomControl_c::getBgp(roomNo);
    if (bgp != NULL) {
        bgp->draw(this);
    }

#if PLATFORM_PC
    /* Restore original signal handlers now that all crash-prone code is done */
    sigaction(SIGSEGV, &sa_segv_old, NULL);
    sigaction(SIGABRT, &sa_abrt_old, NULL);
#endif
    return 1;
}

int daBg_c::execute() {
    daBg_Part* bgPart = mBgParts;

    for (int i = 0; i < 6; i++) {
        if (bgPart->btk != NULL) {
            if (field_0x5f1 != 0) {
                f32 speed = (field_0x5f1 - 1) / 100.0f;
                bgPart->btk->playspeed(speed);
            }

            if (bgPart->btk_speed >= 0.0f) {
                bgPart->btk->playspeed(bgPart->btk_speed);
            }
            bgPart->btk->play();
        }

        if (bgPart->brk != NULL && field_0x5f0 == 0) {
            bgPart->brk->play();
        }

        bgPart++;
    }

    return 1;
}

static int daBg_Execute(daBg_c* i_this) {
    return i_this->execute();
}

int daBg_c::isDelete() {
#if PLATFORM_PC
    /* On PC the BG actor holds room geometry and must stay alive for its Draw
     * to call mDoExt_modelEntryDL every frame.  The room manager already
     * protects BG actors from objectDeleteJugge (d_s_room.cpp), but other
     * framework paths call fpcDt_Delete which checks isDelete().  Return 0 to
     * keep the actor in the draw queue permanently. */
    return 0;
#else
    return 1;
#endif
}

static int daBg_IsDelete(daBg_c* i_this) {
    return i_this->isDelete();
}

static int daBg_Delete(daBg_c* i_this) {
    i_this->~daBg_c();
    return 1;
}

static int daBg_Create(fopAc_ac_c* i_this) {
    daBg_c* a_this = (daBg_c*)i_this;
    int rt = a_this->create();
    return rt;
}

int daBg_c::create() {
    int roomNo = fopAcM_GetParam(this);
    field_0x5f0 = 0;
    field_0x5f1 = 0;
    dBgp_c* bgp = dStage_roomControl_c::getBgp(roomNo);

#if PLATFORM_PC
    static int s_bg_create_log = 0;
    if (s_bg_create_log < 10) {
        fprintf(stderr, "{\"bg_create\":{\"roomNo\":%d,\"heap\":%p,\"bgp\":%p,\"call\":%d}}\n",
                roomNo, (void*)this->heap, (void*)bgp, s_bg_create_log);
        s_bg_create_log++;
    }
#endif

    if (this->heap == NULL) {
        fopAcM_ct(this, daBg_c);

        home.roomNo = roomNo;
        current.roomNo = roomNo;

        JKRExpHeap* room_heap = dStage_roomControl_c::getMemoryBlock(roomNo);
        if (room_heap != NULL) {
            this->heap = JKRCreateSolidHeap(-1, room_heap, false);
            JUT_ASSERT(471, heap != NULL);

            JKRHeap* old = mDoExt_setCurrentHeap(this->heap);
            int rt = createHeap();
            JUT_ASSERT(476, rt == 1);

            mDoExt_setCurrentHeap(old);
            this->heap->adjustSize();
        } else {
            u32 size = 0x20040;
            size |= 0x80000000;
            if (!fopAcM_entrySolidHeap(this, checkCreateHeap, size)) {
                return cPhs_ERROR_e;
            }
        }

        daBg_Part* bgPart = mBgParts;
        for (int i = 0; i < 6; i++) {
            if (bgPart->model != NULL) {
                J3DModelData* modelData = bgPart->model->getModelData();

                if (bgPart->btk != NULL) {
                    bgPart->btk->entry(modelData);
                }

                if (bgPart->brk != NULL) {
                    bgPart->brk->entry(modelData);
                }
            }
            bgPart++;
        }

        J3DModelData* modelData;
        f32 transX;
        f32 transY;
        s16 angle;
        if (dComIfGp_getMapTrans(roomNo, &transX, &transY, &angle)) {
            daBg_Part* bgPart = mBgParts;
            J3DModel* model;
            for (int i = 0; i < 6; i++) {
                model = bgPart->model;

                if (model != NULL) {
                    mDoMtx_stack_c::transS(transX, 0.0f, transY);
                    mDoMtx_stack_c::YrotM(angle);
                    model->setBaseTRMtx(mDoMtx_stack_c::get());

                    modelData = model->getModelData();
                    for (u16 j = 0; j < modelData->getMaterialNum(); j++) {
                        JUTNameTab* nametab = modelData->getMaterialName();
                        const char* name = nametab->getName(j);

                        if (name[3] == 'M' && name[4] == 'A' && name[5] == '0' && name[6] == '8') {
                            J3DMaterial* material = modelData->getMaterialNodePointer(j);
                            J3DShape* shape = material->getShape();
                            shape->setTexMtxLoadType(0x2000);
                        }
                    }
                }
                bgPart++;
            }
        }

        if (mpBgW != NULL) {
            if (dComIfG_Bgsp().Regist(mpBgW, this)) {
                return cPhs_ERROR_e;
            }
        }

        if (mpKCol != NULL) {
            if (dComIfG_Bgsp().Regist(mpKCol, this)) {
                return cPhs_ERROR_e;
            }
        }

        dKy_tevstr_c* tevstr = dComIfGp_roomControl_getTevStr(roomNo);
        dKy_tevstr_init(tevstr, roomNo, 0xFF);

        if (bgp != NULL) {
            bgp->registBg(this);
        }
    }

    if (bgp != NULL) {
#if PLATFORM_PC
        static int s_bgp_exec_log = 0;
        if (s_bgp_exec_log < 10) {
            fprintf(stderr, "{\"bgp_exec\":{\"call\":%d}}\n", s_bgp_exec_log);
            s_bgp_exec_log++;
        }
#endif
        if (!bgp->execute(false)) {
            return cPhs_INIT_e;
        }
    }

    dComIfGp_roomControl_onStatusFlag(roomNo, 0x10);
    OS_REPORT("<BG> room%d\n", roomNo);
#if PLATFORM_PC
    fprintf(stderr, "{\"bg_create_complete\":{\"roomNo\":%d}}\n", roomNo);
#endif
    return cPhs_COMPLEATE_e;
}

static actor_method_class l_daBg_Method = {
    (process_method_func)daBg_Create,
    (process_method_func)daBg_Delete,
    (process_method_func)daBg_Execute,
    (process_method_func)daBg_IsDelete,
    (process_method_func)daBg_Draw,
};

actor_process_profile_definition2 g_profile_BG = {
    fpcLy_CURRENT_e,       // mLayerID
    7,                     // mListID
    fpcPi_CURRENT_e,       // mListPrio
    PROC_BG,        // mProcName
    &g_fpcLf_Method.base, // sub_method
    sizeof(daBg_c),            // mSize
    0,                     // mSizeOther
    0,                     // mParameters
    &g_fopAc_Method.base,  // sub_method
    753,                   // mPriority
    &l_daBg_Method, // sub_method
    0x00060000,            // mStatus
    fopAc_ACTOR_e,   // mActorType
    fopAc_CULLBOX_0_e,     // cullType
    0,
};

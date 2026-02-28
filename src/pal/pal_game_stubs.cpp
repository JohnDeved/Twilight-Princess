/**
 * pal_game_stubs.cpp
 * Stub implementations for game-specific undefined references that come from
 * missing/uncompiled game code. These resolve link errors for functions and
 * data that exist in the game source but aren't compiled in certain configs.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <cstring>
#include <cstdio>

#include "dolphin/types.h"

/* Include GX types early to avoid conflicts with game headers */
#include "dolphin/gx.h"

/* ---------------------------------------------------------------- */
/* Forward declarations - avoid pulling in complex game headers     */
/* ---------------------------------------------------------------- */

class JKRHeap;
class J3DPacket;
class JORPropertyEvent;
class JORMContext;

struct cXyz;
struct csXyz;

/* ================================================================ */
/* DynamicModuleControlBase::m_heap                                 */
/* ================================================================ */

namespace {
    JKRHeap* s_dmc_heap = NULL;
}

/* Provide the static member - matches include/DynamicLink.h:60 */
#include "DynamicLink.h"
JKRHeap* DynamicModuleControlBase::m_heap = NULL;

/* ================================================================ */
/* cM3dGVtx destructor                                              */
/* ================================================================ */

#include "SSystem/SComponent/c_m3d_g_vtx.h"

cM3dGVtx::~cM3dGVtx() {}

/* ================================================================ */
/* dDbVw_draw* functions (debug view - all no-ops)                  */
/* ================================================================ */

extern "C" {

/* Debug viewer functions don't need the actual implementation for release builds */
void dDbVw_deleteDrawPacketList() {}
int dDbVw_Report(int x, int y, const char* string, ...) {
    (void)x; (void)y; (void)string;
    return 0;
}

} /* extern "C" */

/* These have C++ linkage with references/pointers */
void dDbVw_drawCube8p(int i_bufferType, cXyz* i_points, const GXColor& i_color) {
    (void)i_bufferType; (void)i_points; (void)i_color;
}
void dDbVw_drawCube(int i_bufferType, cXyz& i_pos, cXyz& i_size, csXyz& i_angle, const GXColor& i_color) {
    (void)i_bufferType; (void)i_pos; (void)i_size; (void)i_angle; (void)i_color;
}
void dDbVw_drawTriangle(int i_bufferType, cXyz* i_points, const GXColor& i_color, u8 i_clipZ) {
    (void)i_bufferType; (void)i_points; (void)i_color; (void)i_clipZ;
}
void dDbVw_drawQuad(int i_bufferType, cXyz* i_points, const GXColor& i_color, u8 i_clipZ) {
    (void)i_bufferType; (void)i_points; (void)i_color; (void)i_clipZ;
}
void dDbVw_drawLine(int i_bufferType, cXyz& i_start, cXyz& i_end, const GXColor& i_color, u8 i_clipZ, u8 i_width) {
    (void)i_bufferType; (void)i_start; (void)i_end; (void)i_color; (void)i_clipZ; (void)i_width;
}
void dDbVw_drawArrow(int i_bufferType, cXyz& i_pos, cXyz& i_end, const GXColor& i_color, u8 i_clipZ, u8 i_width) {
    (void)i_bufferType; (void)i_pos; (void)i_end; (void)i_color; (void)i_clipZ; (void)i_width;
}
void dDbVw_drawPoint(int i_bufferType, cXyz& i_pos, const GXColor& i_color, u8 i_clipZ, u8 i_width) {
    (void)i_bufferType; (void)i_pos; (void)i_color; (void)i_clipZ; (void)i_width;
}
void dDbVw_drawSphere(int i_bufferType, cXyz& i_pos, f32 i_size, const GXColor& i_color, u8 i_clipZ) {
    (void)i_bufferType; (void)i_pos; (void)i_size; (void)i_color; (void)i_clipZ;
}
void dDbVw_drawCylinder(int i_bufferType, cXyz& i_pos, f32 i_radius, f32 i_height, const GXColor& i_color, u8 i_clipZ) {
    (void)i_bufferType; (void)i_pos; (void)i_radius; (void)i_height; (void)i_color; (void)i_clipZ;
}
void dDbVw_drawCircle(int i_bufferType, cXyz& i_pos, f32 i_radius, const GXColor& i_color, u8 i_clipZ) {
    (void)i_bufferType; (void)i_pos; (void)i_radius; (void)i_color; (void)i_clipZ;
}

J3DPacket* dDbVw_setDrawPacketList(J3DPacket* i_packet, int i_bufferType) {
    (void)i_packet; (void)i_bufferType;
    return NULL;
}

/* ================================================================ */
/* dMenu_Calibration_c::_move                                       */
/* ================================================================ */

#include "d/d_menu_calibration.h"

void dMenu_Calibration_c::_move() {}

/* ================================================================ */
/* dMeter2Info wide screen functions                                */
/* ================================================================ */

void dMeter2Info_onWide2D() {}
void dMeter2Info_offWide2D() {}

/* ================================================================ */
/* dMeter_map_HIO_c                                                 */
/* ================================================================ */

/* Stub constructor and static member */
#include "d/d_meter_map.h"

dMeter_map_HIO_c::dMeter_map_HIO_c() {}

dMeter_map_HIO_c* dMeter_map_HIO_c::mMySelfPointer = NULL;

void dMeter_map_HIO_c::listenPropertyEvent(const JORPropertyEvent*) {}
void dMeter_map_HIO_c::genMessage(JORMContext*) {}
void dMeter_map_HIO_c::update() {}

/* ================================================================ */
/* dMsgObject_c::setWord / setSelectWord                            */
/* ================================================================ */

#include "d/d_msg_object.h"

void dMsgObject_c::setWord(const char* i_word) { (void)i_word; }
void dMsgObject_c::setSelectWord(int i_no, const char* i_word) { (void)i_no; (void)i_word; }

/* ================================================================ */
/* mDoExt packet classes                                            */
/* ================================================================ */

/* m_Do_ext.cpp now compiled - only keep destructors if still needed */

mDoExt_offCupOnAupPacket::~mDoExt_offCupOnAupPacket() {}
mDoExt_onCupOffAupPacket::~mDoExt_onCupOffAupPacket() {}

/* ================================================================ */
/* mDoMemCd_Ctrl_c::command_attach                                  */
/* ================================================================ */

#include "m_Do/m_Do_MemCard.h"

void mDoMemCd_Ctrl_c::command_attach() {}

/* ================================================================ */
/* Z2SeMgr::homeMenuSeCallback                                      */
/* ================================================================ */

#include "Z2AudioLib/Z2SeMgr.h"

void Z2SeMgr::homeMenuSeCallback(s32 param) { (void)param; }

/* ================================================================ */
/* GF wrapper functions                                             */
/* ================================================================ */

/* GF wrappers - C++ linkage (no extern "C"), proper GX enum types */
#include "revolution/gf/GFPixel.h"

void GFSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    (void)type; (void)startz; (void)endz; (void)nearz; (void)farz; (void)color;
}

void GFSetBlendModeEtc(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor,
                       GXLogicOp logic_op, u8 color_update_enable, u8 alpha_update_enable,
                       u8 dither_enable) {
    (void)type; (void)src_factor; (void)dst_factor; (void)logic_op;
    (void)color_update_enable; (void)alpha_update_enable; (void)dither_enable;
}

void GFSetZMode(u8 compare_enable, GXCompare func, u8 update_enable) {
    (void)compare_enable; (void)func; (void)update_enable;
}

/* ================================================================ */
/* JSU Stream classes                                               */
/* ================================================================ */

/* JSU stream stubs are in pal_remaining_stubs.cpp; JSUInputStream is compiled from source */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

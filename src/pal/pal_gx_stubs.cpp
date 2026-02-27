/**
 * pal_gx_stubs.cpp
 * Stub implementations for GX/GD graphics functions needed to link the PC port.
 * These provide safe no-op or minimal implementations.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <cstring>

#include "dolphin/types.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/vi/vitypes.h"
#include "revolution/gx/GXFifo.h"
#include "revolution/gx/GXManage.h"
#include "revolution/gx/GXTransform.h"
#include "revolution/gx/GXTexture.h"
#include "revolution/gx/GXTev.h"
#include "revolution/gx/GXPixel.h"
#include "revolution/gx/GXLighting.h"
#include "revolution/gx/GXGeometry.h"
#include "revolution/gx/GXFrameBuffer.h"
#include "revolution/gx/GXBump.h"
#include "revolution/gx/GXCull.h"
#include "revolution/gx/GXGet.h"
#include "revolution/gx/GXDispList.h"
#include "revolution/gx/GXPerf.h"
#include "revolution/gx/GXCpu2Efb.h"
#include "revolution/gd/GDBase.h"
#include "pal/pal_milestone.h"
#include "pal/gx/gx_stub_tracker.h"

extern "C" {

/* ================================================================ */
/* GX Manage / Sync                                                 */
/* ================================================================ */

static u8 s_gx_fifo_mem[1024];
static GXFifoObj s_gx_fifo;

GXFifoObj* GXInit(void* base, u32 size) {
    (void)base; (void)size;
    memset(&s_gx_fifo, 0, sizeof(s_gx_fifo));
    return &s_gx_fifo;
}

void GXSetMisc(GXMiscToken token, u32 val) { (void)token; (void)val; }
void GXFlush(void) {}
void GXResetWriteGatherPipe(void) {}
void GXAbortFrame(void) {}
void GXSetDrawSync(u16 token) { (void)token; }
u16 GXReadDrawSync(void) { return 0; }
void GXSetDrawDone(void) {}
void GXWaitDrawDone(void) {}
void GXDrawDone(void) {}
void GXPixModeSync(void) {}
void GXTexModeSync(void) {}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) { (void)cb; return NULL; }
GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) { (void)cb; return NULL; }

/* ================================================================ */
/* GX FIFO                                                          */
/* ================================================================ */

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) { (void)fifo; (void)base; (void)size; }
void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) { (void)fifo; (void)readPtr; (void)writePtr; }
void GXInitFifoLimits(GXFifoObj* fifo, u32 hi, u32 lo) { (void)fifo; (void)hi; (void)lo; }
void GXSetCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSetGPFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSaveCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSaveGPFifo(GXFifoObj* fifo) { (void)fifo; }
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    if (overhi) *overhi = GX_FALSE;
    if (underlow) *underlow = GX_FALSE;
    if (readIdle) *readIdle = GX_TRUE;
    if (cmdIdle) *cmdIdle = GX_TRUE;
    if (brkpt) *brkpt = GX_FALSE;
}
void GXGetFifoStatus(GXFifoObj* fifo, GXBool* overhi, GXBool* underflow, u32* fifoCount,
                     GXBool* cpuWrite, GXBool* gpRead, GXBool* fifowrap) {
    (void)fifo;
    if (overhi) *overhi = GX_FALSE;
    if (underflow) *underflow = GX_FALSE;
    if (fifoCount) *fifoCount = 0;
    if (cpuWrite) *cpuWrite = GX_FALSE;
    if (gpRead) *gpRead = GX_FALSE;
    if (fifowrap) *fifowrap = GX_FALSE;
}
void GXGetFifoPtrs(GXFifoObj* fifo, void** readPtr, void** writePtr) {
    (void)fifo;
    if (readPtr) *readPtr = NULL;
    if (writePtr) *writePtr = NULL;
}
void* GXGetFifoBase(const GXFifoObj* fifo) { (void)fifo; return NULL; }
u32 GXGetFifoSize(const GXFifoObj* fifo) { (void)fifo; return 0; }
void GXGetFifoLimits(const GXFifoObj* fifo, u32* hi, u32* lo) {
    (void)fifo;
    if (hi) *hi = 0;
    if (lo) *lo = 0;
}
GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) { (void)cb; return NULL; }
void GXEnableBreakPt(void* break_pt) { (void)break_pt; }
void GXDisableBreakPt(void) {}
GXFifoObj* GXGetCPUFifo(void) { return &s_gx_fifo; }
GXFifoObj* GXGetGPFifo(void) { return &s_gx_fifo; }
u32 GXGetOverflowCount(void) { return 0; }
u32 GXResetOverflowCount(void) { return 0; }
volatile void* GXRedirectWriteGatherPipe(void* ptr) { (void)ptr; return NULL; }
void GXRestoreWriteGatherPipe(void) {}

/* GXSetCurrentGXThread / GXGetCurrentGXThread use OSThread* but we forward-declare */
struct OSThread;
OSThread* GXSetCurrentGXThread(void) { return NULL; }
OSThread* GXGetCurrentGXThread(void) { return NULL; }

/* ================================================================ */
/* GX Transform                                                     */
/* ================================================================ */

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm,
               const f32* vp, f32* sx, f32* sy, f32* sz) {
    (void)x; (void)y; (void)z; (void)mtx; (void)pm; (void)vp;
    if (sx) *sx = 0.0f;
    if (sy) *sy = 0.0f;
    if (sz) *sz = 0.0f;
}
void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) { (void)mtx; (void)type; }
void GXSetProjectionv(const f32* ptr) { (void)ptr; }
void GXLoadPosMtxImm(const f32 mtx[3][4], u32 id) { (void)mtx; (void)id; }
void GXLoadPosMtxIndx(u16 mtx_indx, u32 id) { (void)mtx_indx; (void)id; }
void GXLoadNrmMtxImm(const f32 mtx[3][4], u32 id) { (void)mtx; (void)id; }
void GXLoadNrmMtxImm3x3(const f32 mtx[3][3], u32 id) { (void)mtx; (void)id; }
void GXLoadNrmMtxIndx3x3(u16 mtx_indx, u32 id) { (void)mtx_indx; (void)id; }
void GXSetCurrentMtx(u32 id) { (void)id; }
void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, GXTexMtxType type) { (void)mtx; (void)id; (void)type; }
void GXLoadTexMtxIndx(u16 mtx_indx, u32 id, GXTexMtxType type) { (void)mtx_indx; (void)id; (void)type; }
void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    (void)left; (void)top; (void)wd; (void)ht; (void)nearz; (void)farz; (void)field;
}
void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    (void)left; (void)top; (void)wd; (void)ht; (void)nearz; (void)farz;
}
void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetScissorBoxOffset(s32 x_off, s32 y_off) { (void)x_off; (void)y_off; }
void GXSetClipMode(GXClipMode mode) { (void)mode; }
void GXSetZScaleOffset(f32 scale, f32 offset) { (void)scale; (void)offset; }

/* ================================================================ */
/* GX Get functions                                                 */
/* ================================================================ */

void GXGetViewportv(f32* vp) { if (vp) memset(vp, 0, 6 * sizeof(f32)); }
void GXGetProjectionv(f32* ptr) { if (ptr) memset(ptr, 0, 7 * sizeof(f32)); }
void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    if (left) *left = 0;
    if (top) *top = 0;
    if (wd) *wd = 640;
    if (ht) *ht = 480;
}
void GXGetVtxDesc(GXAttr attr, GXAttrType* type) { (void)attr; if (type) *type = (GXAttrType)0; }
void GXGetVtxDescv(GXVtxDescList* vcd) { (void)vcd; }
void GXGetVtxAttrFmt(GXVtxFmt fmt, GXAttr attr, GXCompCnt* cnt, GXCompType* type, u8* frac) {
    (void)fmt; (void)attr;
    if (cnt) *cnt = (GXCompCnt)0;
    if (type) *type = (GXCompType)0;
    if (frac) *frac = 0;
}
void GXGetVtxAttrFmtv(GXVtxFmt fmt, GXVtxAttrFmtList* vat) { (void)fmt; (void)vat; }

/* ================================================================ */
/* GX Texture                                                       */
/* ================================================================ */

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod) {
    (void)format; (void)mipmap; (void)max_lod;
    return (u32)width * height * 4;
}
void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                  GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap) {
    if (obj) memset(obj, 0, sizeof(GXTexObj));
    (void)image_ptr; (void)width; (void)height; (void)format; (void)wrap_s; (void)wrap_t; (void)mipmap;
}
void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height,
                    GXCITexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t,
                    u8 mipmap, u32 tlut_name) {
    if (obj) memset(obj, 0, sizeof(GXTexObj));
    (void)image_ptr; (void)width; (void)height; (void)format; (void)wrap_s; (void)wrap_t; (void)mipmap; (void)tlut_name;
}
void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt,
                     f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp,
                     GXBool do_edge_lod, GXAnisotropy max_aniso) {
    (void)obj; (void)min_filt; (void)mag_filt; (void)min_lod; (void)max_lod;
    (void)lod_bias; (void)bias_clamp; (void)do_edge_lod; (void)max_aniso;
}
void GXInitTexObjData(GXTexObj* obj, void* image_ptr) { (void)obj; (void)image_ptr; }
void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode s, GXTexWrapMode t) { (void)obj; (void)s; (void)t; }
void GXInitTexObjTlut(GXTexObj* obj, u32 tlut_name) { (void)obj; (void)tlut_name; }
void GXInitTexObjUserData(GXTexObj* obj, void* user_data) { (void)obj; (void)user_data; }
void* GXGetTexObjUserData(const GXTexObj* obj) { (void)obj; return NULL; }
void GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion* region, GXTexMapID id) { (void)obj; (void)region; (void)id; }
void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) { (void)obj; (void)id; }
void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, GXTlutFmt fmt, u16 n_entries) {
    if (tlut_obj) memset(tlut_obj, 0, sizeof(GXTlutObj));
    (void)lut; (void)fmt; (void)n_entries;
}
void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name) { (void)tlut_obj; (void)tlut_name; }
void GXInitTexCacheRegion(GXTexRegion* region, u8 is_32b_mipmap, u32 tmem_even,
                          GXTexCacheSize size_even, u32 tmem_odd, GXTexCacheSize size_odd) {
    if (region) memset(region, 0, sizeof(GXTexRegion));
    (void)is_32b_mipmap; (void)tmem_even; (void)size_even; (void)tmem_odd; (void)size_odd;
}
void GXInitTexPreLoadRegion(GXTexRegion* region, u32 tmem_even, u32 size_even,
                            u32 tmem_odd, u32 size_odd) {
    if (region) memset(region, 0, sizeof(GXTexRegion));
    (void)tmem_even; (void)size_even; (void)tmem_odd; (void)size_odd;
}
void GXInitTlutRegion(GXTlutRegion* region, u32 tmem_addr, GXTlutSize tlut_size) {
    if (region) memset(region, 0, sizeof(GXTlutRegion));
    (void)tmem_addr; (void)tlut_size;
}
void GXInvalidateTexRegion(GXTexRegion* region) { (void)region; }
void GXInvalidateTexAll(void) {}
GXTexRegionCallback GXSetTexRegionCallback(GXTexRegionCallback f) { (void)f; return NULL; }
GXTlutRegionCallback GXSetTlutRegionCallback(GXTlutRegionCallback f) { (void)f; return NULL; }
void GXPreLoadEntireTexture(GXTexObj* tex_obj, GXTexRegion* region) { (void)tex_obj; (void)region; }
void GXSetTexCoordScaleManually(GXTexCoordID coord, u8 enable, u16 ss, u16 ts) {
    (void)coord; (void)enable; (void)ss; (void)ts;
}
void GXSetTexCoordCylWrap(GXTexCoordID coord, u8 s_enable, u8 t_enable) {
    (void)coord; (void)s_enable; (void)t_enable;
}
void GXSetTexCoordBias(GXTexCoordID coord, u8 s_enable, u8 t_enable) {
    (void)coord; (void)s_enable; (void)t_enable;
}
void GXInitTexObjFilter(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt) {
    (void)obj; (void)min_filt; (void)mag_filt;
}
void GXInitTexObjMaxLOD(GXTexObj* obj, f32 max_lod) { (void)obj; (void)max_lod; }
void GXInitTexObjMinLOD(GXTexObj* obj, f32 min_lod) { (void)obj; (void)min_lod; }
void GXInitTexObjLODBias(GXTexObj* obj, f32 lod_bias) { (void)obj; (void)lod_bias; }
void GXInitTexObjBiasClamp(GXTexObj* obj, u8 bias_clamp) { (void)obj; (void)bias_clamp; }
void GXInitTexObjEdgeLOD(GXTexObj* obj, u8 do_edge_lod) { (void)obj; (void)do_edge_lod; }
void GXInitTexObjMaxAniso(GXTexObj* obj, GXAnisotropy max_aniso) { (void)obj; (void)max_aniso; }

u16 GXGetTexObjWidth(const GXTexObj* obj) { (void)obj; return 0; }
u16 GXGetTexObjHeight(const GXTexObj* obj) { (void)obj; return 0; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* obj) { (void)obj; return (GXTexFmt)0; }
GXTexWrapMode GXGetTexObjWrapS(const GXTexObj* obj) { (void)obj; return (GXTexWrapMode)0; }
GXTexWrapMode GXGetTexObjWrapT(const GXTexObj* obj) { (void)obj; return (GXTexWrapMode)0; }
u8 GXGetTexObjMipMap(const GXTexObj* obj) { (void)obj; return 0; }
void* GXGetTexObjData(const GXTexObj* obj) { (void)obj; return NULL; }
u32 GXGetTexObjTlut(const GXTexObj* obj) { (void)obj; return 0; }
void GXGetTexObjAll(const GXTexObj* obj, void** image_ptr, u16* width, u16* height,
                    GXTexFmt* format, GXTexWrapMode* wrap_s, GXTexWrapMode* wrap_t, u8* mipmap) {
    (void)obj;
    if (image_ptr) *image_ptr = NULL;
    if (width) *width = 0;
    if (height) *height = 0;
    if (format) *format = (GXTexFmt)0;
    if (wrap_s) *wrap_s = (GXTexWrapMode)0;
    if (wrap_t) *wrap_t = (GXTexWrapMode)0;
    if (mipmap) *mipmap = 0;
}

/* ================================================================ */
/* GX TEV                                                           */
/* ================================================================ */

void GXSetTevOp(GXTevStageID id, GXTevMode mode) { (void)id; (void)mode; }
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b,
                     GXTevColorArg c, GXTevColorArg d) {
    (void)stage; (void)a; (void)b; (void)c; (void)d;
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b,
                     GXTevAlphaArg c, GXTevAlphaArg d) {
    (void)stage; (void)a; (void)b; (void)c; (void)d;
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg) {
    (void)stage; (void)op; (void)bias; (void)scale; (void)clamp; (void)out_reg;
}
void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias,
                     GXTevScale scale, GXBool clamp, GXTevRegID out_reg) {
    (void)stage; (void)op; (void)bias; (void)scale; (void)clamp; (void)out_reg;
}
void GXSetTevColor(GXTevRegID id, GXColor color) { (void)id; (void)color; }
void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) { (void)id; (void)color; }
void GXSetTevKColor(GXTevKColorID id, GXColor color) { (void)id; (void)color; }
void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) { (void)stage; (void)sel; }
void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) { (void)stage; (void)sel; }
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    (void)stage; (void)ras_sel; (void)tex_sel;
}
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                           GXTevColorChan blue, GXTevColorChan alpha) {
    (void)table; (void)red; (void)green; (void)blue; (void)alpha;
}
void GXSetTevClampMode(void) {}
void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    (void)comp0; (void)ref0; (void)op; (void)comp1; (void)ref1;
}
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) { (void)op; (void)fmt; (void)bias; }
void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    (void)stage; (void)coord; (void)map; (void)color;
}
void GXSetNumTevStages(u8 nStages) { (void)nStages; }

/* ================================================================ */
/* GX Pixel / Blend / Fog                                           */
/* ================================================================ */

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    (void)type; (void)startz; (void)endz; (void)nearz; (void)farz; (void)color;
}
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, const f32 projmtx[4][4]) {
    if (table) memset(table, 0, sizeof(GXFogAdjTable));
    (void)width; (void)projmtx;
}
void GXSetFogRangeAdj(GXBool enable, u16 center, const GXFogAdjTable* table) {
    (void)enable; (void)center; (void)table;
}
void GXSetFogColor(GXColor color) { (void)color; }
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    (void)type; (void)src_factor; (void)dst_factor; (void)op;
}
void GXSetColorUpdate(GXBool update_enable) { (void)update_enable; }
void GXSetAlphaUpdate(GXBool update_enable) { (void)update_enable; }
void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    (void)compare_enable; (void)func; (void)update_enable;
}
void GXSetZCompLoc(GXBool before_tex) { (void)before_tex; }
void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) { (void)pix_fmt; (void)z_fmt; }
void GXSetDither(GXBool dither) { (void)dither; }
void GXSetDstAlpha(GXBool enable, u8 alpha) { (void)enable; (void)alpha; }
void GXSetFieldMask(GXBool odd_mask, GXBool even_mask) { (void)odd_mask; (void)even_mask; }
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio) { (void)field_mode; (void)half_aspect_ratio; }

/* ================================================================ */
/* GX Lighting                                                      */
/* ================================================================ */

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    (void)lt_obj; (void)a0; (void)a1; (void)a2; (void)k0; (void)k1; (void)k2;
}
void GXInitLightAttnA(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2) { (void)lt_obj; (void)a0; (void)a1; (void)a2; }
void GXInitLightAttnK(GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2) { (void)lt_obj; (void)k0; (void)k1; (void)k2; }
void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func) {
    (void)lt_obj; (void)cutoff; (void)spot_func;
}
void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func) {
    (void)lt_obj; (void)ref_dist; (void)ref_br; (void)dist_func;
}
void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) { (void)lt_obj; (void)x; (void)y; (void)z; }
void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) { (void)lt_obj; (void)nx; (void)ny; (void)nz; }
void GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) { (void)lt_obj; (void)nx; (void)ny; (void)nz; }
void GXInitSpecularDirHA(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz, f32 hx, f32 hy, f32 hz) {
    (void)lt_obj; (void)nx; (void)ny; (void)nz; (void)hx; (void)hy; (void)hz;
}
void GXInitLightColor(GXLightObj* lt_obj, GXColor color) { (void)lt_obj; (void)color; }
void GXLoadLightObjImm(const GXLightObj* lt_obj, GXLightID light) { (void)lt_obj; (void)light; }
void GXLoadLightObjIndx(u32 lt_obj_indx, GXLightID light) { (void)lt_obj_indx; (void)light; }
void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) { (void)chan; (void)amb_color; }
void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) { (void)chan; (void)mat_color; }
void GXSetNumChans(u8 nChans) { (void)nChans; }
void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src,
                   u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    (void)chan; (void)enable; (void)amb_src; (void)mat_src; (void)light_mask; (void)diff_fn; (void)attn_fn;
}

/* ================================================================ */
/* GX Geometry / Vertex                                             */
/* ================================================================ */

void __GXCalculateVLim(void) {}
void GXSetVtxDesc(GXAttr attr, GXAttrType type) { (void)attr; (void)type; }
void GXSetVtxDescv(const GXVtxDescList* attrPtr) { (void)attrPtr; }
void GXClearVtxDesc(void) {}
void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    (void)vtxfmt; (void)attr; (void)cnt; (void)type; (void)frac;
}
void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) { (void)vtxfmt; (void)list; }
void GXSetArray(GXAttr attr, void* base_ptr, u8 stride) { (void)attr; (void)base_ptr; (void)stride; }
void GXInvalidateVtxCache(void) {}
void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param,
                       u32 mtx, GXBool normalize, u32 pt_texmtx) {
    (void)dst_coord; (void)func; (void)src_param; (void)mtx; (void)normalize; (void)pt_texmtx;
}
void GXSetNumTexGens(u8 nTexGens) { (void)nTexGens; }
void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) { (void)type; (void)vtxfmt; (void)nverts; }
void GXSetLineWidth(u8 width, GXTexOffset texOffsets) { (void)width; (void)texOffsets; }
void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) { (void)pointSize; (void)texOffsets; }
void GXEnableTexOffsets(GXTexCoordID coord, u8 line_enable, u8 point_enable) {
    (void)coord; (void)line_enable; (void)point_enable;
}
void GXSetCullMode(GXCullMode mode) { (void)mode; }
void __GXSetVAT(void) {}

/* ================================================================ */
/* GX Frame Buffer / Copy                                           */
/* ================================================================ */

void GXAdjustForOverscan(const GXRenderModeObj* rmin, GXRenderModeObj* rmout, u16 hor, u16 ver) {
    if (rmin && rmout) memcpy(rmout, rmin, sizeof(GXRenderModeObj));
    (void)hor; (void)ver;
}
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetDispCopyDst(u16 wd, u16 ht) { (void)wd; (void)ht; }
void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) { (void)wd; (void)ht; (void)fmt; (void)mipmap; }
void GXSetDispCopyFrame2Field(GXCopyMode mode) { (void)mode; }
void GXSetCopyClamp(GXFBClamp clamp) { (void)clamp; }
u32 GXSetDispCopyYScale(f32 vscale) { (void)vscale; return 0; }
void GXSetCopyClear(GXColor clear_clr, u32 clear_z) { (void)clear_clr; (void)clear_z; }
void GXSetCopyFilter(GXBool aa, const u8 sample_pattern[12][2], GXBool vf, const u8 vfilter[7]) {
    (void)aa; (void)sample_pattern; (void)vf; (void)vfilter;
}
void GXSetDispCopyGamma(GXGamma gamma) { (void)gamma; }
void GXCopyDisp(void* dest, GXBool clear) {
    (void)dest; (void)clear;
    /* When the GX shim is active (bgfx initialized), this presents
     * the rendered frame. Fire RENDER_FRAME milestone on first real
     * frame — proving actual pixels were drawn, not just stubs called. */
    if (gx_shim_active && !pal_milestone_was_reached(MILESTONE_RENDER_FRAME)) {
        pal_milestone("RENDER_FRAME", MILESTONE_RENDER_FRAME, "first real rendered frame via GXCopyDisp");
    }
}
void GXCopyTex(void* dest, GXBool clear) { (void)dest; (void)clear; }
void GXClearBoundingBox(void) {}
void GXReadBoundingBox(u16* left, u16* top, u16* right, u16* bottom) {
    if (left) *left = 0;
    if (top) *top = 0;
    if (right) *right = 640;
    if (bottom) *bottom = 480;
}
u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) { (void)yScale; return efbHeight; }
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) { (void)efbHeight; (void)xfbHeight; return 1.0f; }

/* ================================================================ */
/* GX Display List                                                  */
/* ================================================================ */

void GXCallDisplayList(void* list, u32 nbytes) { (void)list; (void)nbytes; }

/* ================================================================ */
/* GX Indirect Texture (Bump)                                       */
/* ================================================================ */

void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                      GXIndTexFormat format, GXIndTexBiasSel bias_sel,
                      GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,
                      GXBool add_prev, GXBool utc_lod, GXIndTexAlphaSel alpha_sel) {
    (void)tev_stage; (void)ind_stage; (void)format; (void)bias_sel;
    (void)matrix_sel; (void)wrap_s; (void)wrap_t; (void)add_prev; (void)utc_lod; (void)alpha_sel;
}
void GXSetIndTexMtx(GXIndTexMtxID mtx_id, const f32 offset[2][3], s8 scale_exp) {
    (void)mtx_id; (void)offset; (void)scale_exp;
}
void GXSetIndTexCoordScale(GXIndTexStageID ind_stage, GXIndTexScale scale_s, GXIndTexScale scale_t) {
    (void)ind_stage; (void)scale_s; (void)scale_t;
}
void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    (void)ind_stage; (void)tex_coord; (void)tex_map;
}
void GXSetNumIndStages(u8 nIndStages) { (void)nIndStages; }
void GXSetTevDirect(GXTevStageID tev_stage) { (void)tev_stage; }
void GXSetTevIndWarp(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                     u8 signed_offset, u8 replace_mode, GXIndTexMtxID matrix_sel) {
    (void)tev_stage; (void)ind_stage; (void)signed_offset; (void)replace_mode; (void)matrix_sel;
}
void GXSetTevIndTile(GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                     u16 tilesize_s, u16 tilesize_t, u16 tilespacing_s, u16 tilespacing_t,
                     GXIndTexFormat format, GXIndTexMtxID matrix_sel,
                     GXIndTexBiasSel bias_sel, GXIndTexAlphaSel alpha_sel) {
    (void)tev_stage; (void)ind_stage; (void)tilesize_s; (void)tilesize_t;
    (void)tilespacing_s; (void)tilespacing_t; (void)format; (void)matrix_sel;
    (void)bias_sel; (void)alpha_sel;
}
void GXSetTevIndBumpST(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexMtxID matrix_sel) {
    (void)tev_stage; (void)ind_stage; (void)matrix_sel;
}
void GXSetTevIndBumpXYZ(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexMtxID matrix_sel) {
    (void)tev_stage; (void)ind_stage; (void)matrix_sel;
}
void GXSetTevIndRepeat(GXTevStageID tev_stage) { (void)tev_stage; }
void __GXSetIndirectMask(u32 mask) { (void)mask; }

/* ================================================================ */
/* GX Performance / Metrics                                         */
/* ================================================================ */

void GXSetGPMetric(GXPerf0 perf0, GXPerf1 perf1) { (void)perf0; (void)perf1; }
void GXClearGPMetric(void) {}
void GXReadXfRasMetric(u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy, u32* clocks) {
    if (xf_wait_in) *xf_wait_in = 0;
    if (xf_wait_out) *xf_wait_out = 0;
    if (ras_busy) *ras_busy = 0;
    if (clocks) *clocks = 0;
}

/* ================================================================ */
/* GX Poke                                                          */
/* ================================================================ */

void GXPokeAlphaMode(GXCompare func, u8 threshold) { (void)func; (void)threshold; }
void GXPokeAlphaRead(GXAlphaReadMode mode) { (void)mode; }
void GXPokeBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    (void)type; (void)src_factor; (void)dst_factor; (void)op;
}
void GXPokeDither(GXBool dither) { (void)dither; }
void GXPokeZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    (void)compare_enable; (void)func; (void)update_enable;
}

/* ================================================================ */
/* GX Render Mode Objects (global structs, not functions)            */
/* ================================================================ */

GXRenderModeObj GXNtsc480Int = {
    VI_TVMODE_NTSC_INT, /* viTVmode */
    640,  /* fbWidth */
    480,  /* efbHeight */
    480,  /* xfbHeight */
    40,   /* viXOrigin */
    0,    /* viYOrigin */
    640,  /* viWidth */
    480,  /* viHeight */
    VI_XFBMODE_SF, /* xFBmode */
    0,    /* field_rendering */
    0,    /* aa */
    { {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6} },
    { 0, 0, 21, 22, 21, 0, 0 }
};

GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT, /* viTVmode */
    640,  /* fbWidth */
    480,  /* efbHeight */
    480,  /* xfbHeight */
    40,   /* viXOrigin */
    0,    /* viYOrigin */
    640,  /* viWidth */
    480,  /* viHeight */
    VI_XFBMODE_DF, /* xFBmode */
    0,    /* field_rendering */
    0,    /* aa */
    { {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6}, {6,6} },
    { 8, 8, 10, 12, 10, 8, 8 }
};

/* ================================================================ */
/* GD (Graphics Display List) functions                             */
/* ================================================================ */

void GDInitGDLObj(GDLObj* dl, void* start, u32 length) { (void)dl; (void)start; (void)length; }
void GDFlushCurrToMem(void) {}
void GDPadCurr32(void) {}
void GDOverflowed(void) {}
void GDSetOverflowCallback(GDOverflowCb callback) { (void)callback; }
GDOverflowCb GDGetOverflowCallback(void) { return NULL; }

/* ================================================================ */
/* GX Vertex data I/O (normally inline/macro, may need symbols)     */
/* ================================================================ */

void GXPosition3f32(f32 x, f32 y, f32 z) { (void)x; (void)y; (void)z; }
void GXPosition3s16(s16 x, s16 y, s16 z) { (void)x; (void)y; (void)z; }
void GXPosition3u16(u16 x, u16 y, u16 z) { (void)x; (void)y; (void)z; }
void GXPosition3s8(s8 x, s8 y, s8 z) { (void)x; (void)y; (void)z; }
void GXPosition3u8(u8 x, u8 y, u8 z) { (void)x; (void)y; (void)z; }
void GXPosition2f32(f32 x, f32 y) { (void)x; (void)y; }
void GXPosition2s16(s16 x, s16 y) { (void)x; (void)y; }
void GXPosition2u16(u16 x, u16 y) { (void)x; (void)y; }
void GXPosition2s8(s8 x, s8 y) { (void)x; (void)y; }
void GXPosition2u8(u8 x, u8 y) { (void)x; (void)y; }
void GXPosition1x8(u8 index) { (void)index; }
void GXPosition1x16(u16 index) { (void)index; }

void GXNormal3f32(f32 nx, f32 ny, f32 nz) { (void)nx; (void)ny; (void)nz; }
void GXNormal3s16(s16 nx, s16 ny, s16 nz) { (void)nx; (void)ny; (void)nz; }
void GXNormal3s8(s8 nx, s8 ny, s8 nz) { (void)nx; (void)ny; (void)nz; }
void GXNormal1x8(u8 index) { (void)index; }
void GXNormal1x16(u16 index) { (void)index; }

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { (void)r; (void)g; (void)b; (void)a; }
void GXColor3u8(u8 r, u8 g, u8 b) { (void)r; (void)g; (void)b; }
void GXColor1u32(u32 clr) { (void)clr; }
void GXColor1u16(u16 clr) { (void)clr; }
void GXColor1x8(u8 index) { (void)index; }
void GXColor1x16(u16 index) { (void)index; }

void GXTexCoord2f32(f32 s, f32 t) { (void)s; (void)t; }
void GXTexCoord2s16(s16 s, s16 t) { (void)s; (void)t; }
void GXTexCoord2u16(u16 s, u16 t) { (void)s; (void)t; }
void GXTexCoord2s8(s8 s, s8 t) { (void)s; (void)t; }
void GXTexCoord2u8(u8 s, u8 t) { (void)s; (void)t; }
void GXTexCoord1f32(f32 s) { (void)s; }
void GXTexCoord1s16(s16 s) { (void)s; }
void GXTexCoord1u16(u16 s) { (void)s; }
void GXTexCoord1s8(s8 s) { (void)s; }
void GXTexCoord1u8(u8 s) { (void)s; }
void GXTexCoord1x8(u8 index) { (void)index; }
void GXTexCoord1x16(u16 index) { (void)index; }

void GXMatrixIndex1x8(u8 index) { (void)index; }

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

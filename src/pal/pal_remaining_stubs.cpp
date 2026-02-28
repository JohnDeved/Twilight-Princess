/**
 * pal_remaining_stubs.cpp
 * Stubs for remaining undefined references after main stub files.
 * Covers GF/GD extras, misc SDK functions, and a few game-specific stubs.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include "dolphin/types.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/gd/GDBase.h"
#include "revolution/os/OSModule.h"
#include "dolphin/dvd.h"
#include "revolution/nand.h"
#include <cstring>
#include <cmath>

#ifdef __cplusplus
extern "C" {
#endif

/* --- GX extras --- */
void GXPeekZ(u16 x, u16 y, u32* z) { if (z) *z = 0; }
void GXSetCoPlanar(u8 enable) { (void)enable; }

/* --- GD extras --- */
GDLObj* __GDCurrentDL = NULL;

void GDSetArray(GXAttr attr, void* base_ptr, u8 stride) {
    (void)attr; (void)base_ptr; (void)stride;
}
void GDSetArrayRaw(GXAttr attr, u32 base_ptr_raw, u8 stride) {
    (void)attr; (void)base_ptr_raw; (void)stride;
}
void GDSetVtxDescv(const GXVtxDescList* attrPtr) { (void)attrPtr; }

/* --- DVD extras --- */
BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    (void)block; (void)callback; return FALSE;
}
BOOL DVDOpenDir(const char* dirName, DVDDir* dir) {
    (void)dirName; if (dir) memset(dir, 0, sizeof(DVDDir)); return FALSE;
}
BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirent) {
    (void)dir; (void)dirent; return FALSE;
}
BOOL DVDCloseDir(DVDDir* dir) { (void)dir; return TRUE; }

/* --- NAND extras --- */
void NANDInitBanner(NANDBanner* bnr, u32 flag, const u16* title, const u16* comment) {
    (void)bnr; (void)flag; (void)title; (void)comment;
}
s32 NANDSimpleSafeOpen(const char* path, NANDFileInfo* info, u8 accType, void* buf, u32 length) {
    (void)path; (void)info; (void)accType; (void)buf; (void)length; return -128;
}
s32 NANDSimpleSafeClose(NANDFileInfo* info) { (void)info; return 0; }

/* --- OS extras --- */
s32 OSCheckActiveThreads(void) { return 1; }
u8 OSGetLanguage(void) { return 1; }

/* MEM2 arena: On Wii, this is 64MB of external memory starting at 0x90000000.
 * On PC, we provide a 128MB malloc-backed buffer to account for 64-bit overhead
 * and ARAM archives loaded into main RAM instead of dedicated audio memory. */
static u8 s_mem2_arena[128 * 1024 * 1024];
static u8* s_mem2_lo = s_mem2_arena;
static u8* s_mem2_hi = s_mem2_arena + sizeof(s_mem2_arena);
void* OSGetMEM2ArenaHi(void) { return s_mem2_hi; }
void* OSGetMEM2ArenaLo(void) { return s_mem2_lo; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSLink(OSModuleInfo* module, void* bss) { (void)module; (void)bss; return TRUE; }
BOOL OSLinkFixed(OSModuleInfo* module, void* bss) { (void)module; (void)bss; return TRUE; }
void OSRestart(u32 resetCode) { (void)resetCode; }
void OSReturnToMenu(void) {}
void OSSetMEM2ArenaHi(void* addr) { s_mem2_hi = (u8*)addr; }
void OSSetMEM2ArenaLo(void* addr) { s_mem2_lo = (u8*)addr; }
void OSSetSoundMode(u32 mode) { (void)mode; }
void OSSetStringTable(void* table) { (void)table; }
void OSShutdownSystem(void) {}
BOOL OSUnlink(OSModuleInfo* module) { (void)module; return TRUE; }

/* --- PAD extras --- */
void PADSetAnalogMode(u32 mode) { (void)mode; }

/* --- VI extras (use proper types from headers) --- */
#include "revolution/vi/vifuncs.h"
void VISetTrapFilter(u8 enable) { (void)enable; }
BOOL VIEnableDimming(BOOL enable) { (void)enable; return FALSE; }
void VISetGamma(VIGamma gamma) { (void)gamma; }

/* --- WPAD extras --- */
u32 WPADGetAcceptConnection(void) { return 0; }
u32 WPADGetDpdSensitivity(void) { return 0; }
u32 WPADGetSpeakerVolume(void) { return 0; }
void WPADSetAcceptConnection(u32 accept) { (void)accept; }
void WPADSetAutoSleepTime(u8 minutes) { (void)minutes; }
typedef void (*WPADConnectCallback_t)(s32, s32);
typedef void (*WPADExtensionCallback_t)(s32, s32);
void WPADSetConnectCallback(s32 chan, WPADConnectCallback_t cb) { (void)chan; (void)cb; }
void WPADSetExtensionCallback(s32 chan, WPADExtensionCallback_t cb) { (void)chan; (void)cb; }

/* --- KPAD extras --- */
void KPADSetObjInterval(f32 interval) { (void)interval; }
void KPADSetSensorHeight(s32 chan, f32 height) { (void)chan; (void)height; }

/* --- WENC --- */
s32 WENCGetEncodeData(void* enc, u32 flag, const s16* pcm, s32 samples, u8* out) {
    (void)enc; (void)flag; (void)pcm; (void)samples; (void)out; return 0;
}

#ifdef __cplusplus
}
#endif

/* ========================= C++ stubs ========================= */

/* --- Math: J3DPSMtxArrayConcat (C++ linkage) --- */
#include "JSystem/J3DGraphBase/J3DTransform.h"
void J3DPSMtxArrayConcat(f32 (*a)[4], f32 (*b)[4], f32 (*out)[4], u32 count) {
    /* Each matrix is 3 rows of 4 floats, so stride is 3*4 = 12 floats */
    u32 i;
    for (i = 0; i < count; i++) {
        f32 (*ma)[4] = a + i * 3;
        f32 (*mb)[4] = b + i * 3;
        f32 (*mo)[4] = out + i * 3;
        int r, c;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 4; c++) {
                mo[r][c] = ma[r][0]*mb[0][c] + ma[r][1]*mb[1][c] + ma[r][2]*mb[2][c];
                if (c == 3) mo[r][c] += ma[r][3];
            }
        }
    }
}

/* --- JSUOutputStream / JSURandomOutputStream / JSUMemoryOutputStream --- */
#include "JSystem/JSupport/JSUOutputStream.h"
#include "JSystem/JSupport/JSURandomOutputStream.h"
#include "JSystem/JSupport/JSUMemoryStream.h"

JSUOutputStream::~JSUOutputStream() {}
s32 JSUOutputStream::skip(s32 count, s8 fill) { (void)count; (void)fill; return 0; }
s32 JSUOutputStream::write(const void* src, s32 length) { (void)src; (void)length; return 0; }
void JSUOutputStream::write(const char* str) { (void)str; }

s32 JSURandomOutputStream::seek(s32 offset, JSUStreamSeekFrom origin) {
    (void)offset; (void)origin; return 0;
}
s32 JSURandomOutputStream::getAvailable() const { return 0; }

s32 JSUMemoryOutputStream::getPosition() const { return mPosition; }
s32 JSUMemoryOutputStream::seek(s32 offset, JSUStreamSeekFrom origin) {
    switch (origin) {
    case JSUStreamSeekFrom_SET: mPosition = offset; break;
    case JSUStreamSeekFrom_CUR: mPosition += offset; break;
    case JSUStreamSeekFrom_END: mPosition = mLength + offset; break;
    }
    return mPosition;
}
s32 JSUMemoryOutputStream::getAvailable() const {
    return mLength - mPosition;
}

/* --- JORMContext --- */
#include "JSystem/JHostIO/JORMContext.h"
void JORMContext::genCheckBoxSub(u32 kind, const char* label, u32 id, u32 style,
                                 u16 initValue, u16 mask, JOREventListener* pListener,
                                 u16 posX, u16 posY, u16 width, u16 height) {
    (void)kind; (void)label; (void)id; (void)style; (void)initValue;
    (void)mask; (void)pListener; (void)posX; (void)posY; (void)width; (void)height;
}

/* --- JStudio_JParticle (source files not in Shield splits.txt) --- */
#include "JSystem/JStudio/JStudio_JParticle/control.h"
namespace JStudio_JParticle {
    TCreateObject::~TCreateObject() {}
    bool TCreateObject::create(JStudio::TObject** out,
                               const JStudio::stb::data::TParse_TBlock_object& blk) {
        (void)out; (void)blk; return false;
    }
    JPABaseEmitter* TCreateObject::emitter_create(u32 id) { (void)id; return NULL; }
    void TCreateObject::emitter_destroy(JPABaseEmitter* emitter) { (void)emitter; }
}

/* --- CARD extras (extern "C" in revolution/card.h) --- */
#include "revolution/card.h"
s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    (void)chan; (void)fileNo; (void)stat;
    return -1; /* CARD_RESULT_NOCARD */
}
s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    (void)chan; (void)fileNo; (void)stat;
    return -1; /* CARD_RESULT_NOCARD */
}

/* --- Matrix extras (extern "C" in revolution/mtx.h) --- */
#include "revolution/mtx.h"
void PSMTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    (void)m; (void)srcBase; (void)dstBase; (void)count;
}

/* --- Debug viewer (C++ linkage) --- */
#include "SSystem/SComponent/c_xyz.h"
void dDbVw_drawCircle(int i_bufferType, cXyz& i_pos, f32 i_radius,
                      const GXColor& i_color, u8 i_clipZ, u8 i_width) {
    (void)i_bufferType; (void)i_pos; (void)i_radius;
    (void)i_color; (void)i_clipZ; (void)i_width;
}

/* --- HIO static member --- */
#include "f_ap/f_ap_game.h"
u8 fapGm_HIO_c::mCaptureScreenDivH = 1;

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

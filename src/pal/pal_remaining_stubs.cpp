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

/* --- GF wrappers --- */
void GFSetBlendModeEtc(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor,
                       GXLogicOp logic_op, u8 color_upd, u8 alpha_upd, u8 dither) {
    (void)type; (void)src_factor; (void)dst_factor; (void)logic_op;
    (void)color_upd; (void)alpha_upd; (void)dither;
}
void GFSetFog(GXFogType fog_type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    (void)fog_type; (void)startz; (void)endz; (void)nearz; (void)farz; (void)color;
}
void GFSetZMode(u8 compare_enable, GXCompare func, u8 update_enable) {
    (void)compare_enable; (void)func; (void)update_enable;
}

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
void* OSGetMEM2ArenaHi(void) { static u8 mem2[0x1000]; return mem2 + sizeof(mem2); }
void* OSGetMEM2ArenaLo(void) { static u8 mem2lo[0x1000]; return mem2lo; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSLink(OSModuleInfo* module, void* bss) { (void)module; (void)bss; return TRUE; }
BOOL OSLinkFixed(OSModuleInfo* module, void* bss) { (void)module; (void)bss; return TRUE; }
void OSRestart(u32 resetCode) { (void)resetCode; }
void OSReturnToMenu(void) {}
void OSSetMEM2ArenaHi(void* addr) { (void)addr; }
void OSSetMEM2ArenaLo(void* addr) { (void)addr; }
void OSSetSoundMode(u32 mode) { (void)mode; }
void OSSetStringTable(void* table) { (void)table; }
void OSShutdownSystem(void) {}
BOOL OSUnlink(OSModuleInfo* module) { (void)module; return TRUE; }

/* --- PAD extras --- */
void PADSetAnalogMode(u32 mode) { (void)mode; }

/* --- VI extras --- */
void VIEnableDimming(u8 enable) { (void)enable; }
void VISetGamma(u8 gamma) { (void)gamma; }
void VISetTrapFilter(u8 enable) { (void)enable; }

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

/* --- Math: J3DPSMtxArrayConcat --- */
typedef float Mtx_t[3][4];
void J3DPSMtxArrayConcat(Mtx_t* a, Mtx_t* b, Mtx_t* out, u32 count) {
    u32 i;
    for (i = 0; i < count; i++) {
        float (*ma)[4] = a[i];
        float (*mb)[4] = b[i];
        float (*mo)[4] = out[i];
        int r, c;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 4; c++) {
                mo[r][c] = ma[r][0]*mb[0][c] + ma[r][1]*mb[1][c] + ma[r][2]*mb[2][c];
                if (c == 3) mo[r][c] += ma[r][3];
            }
        }
    }
}

#ifdef __cplusplus
}
#endif

/* ========================= C++ stubs ========================= */

/* --- JSUOutputStream (not in Shield splits.txt) --- */
#include "JSystem/JSupport/JSUOutputStream.h"
JSUOutputStream::~JSUOutputStream() {}
s32 JSUOutputStream::skip(s32 count, s8 fill) { (void)count; (void)fill; return 0; }

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

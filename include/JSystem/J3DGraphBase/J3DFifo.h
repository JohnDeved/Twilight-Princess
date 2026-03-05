#ifndef J3DFIFO_H
#define J3DFIFO_H

#include <dolphin/gx.h>
#include <dolphin/gd.h>

#if PLATFORM_PC || PLATFORM_NX_HB
#include "pal/gx/gx_state.h"
#endif

inline void J3DFifoLoadBPCmd(u32 regval) {
    GXCmd1u8(GX_LOAD_BP_REG);
    GXCmd1u32(regval);
}

inline void J3DFifoWriteXFCmdHdr(u16 addr, u8 len) {
    GXCmd1u8(GX_LOAD_XF_REG);
    GXCmd1u16(len - 1);
    GXCmd1u16(addr);
}

inline void J3DFifoLoadIndx(u8 cmd, u16 indx, u16 addr) {
#if PLATFORM_PC || PLATFORM_NX_HB
    /* On PC, the GXWGFifo is a no-op.  Intercept indexed matrix loads
     * and apply them to g_gx_state so the position/normal matrices are
     * actually populated before draw calls. */
    pal_gx_fifo_load_indx(cmd, indx, addr);
#else
    GXWGFifo.u8 = cmd;
    GXWGFifo.u16 = indx;
    GXWGFifo.u16 = addr;
#endif
}

inline void J3DFifoWriteCPCmd(u8 cmd, u32 param) {
    GXWGFifo.u8 = GX_LOAD_CP_REG;
    GXWGFifo.u8 = cmd;
    GXWGFifo.u32 = param;
}

inline void J3DFifoLoadCPCmd(u8 reg, u32 value) {
    GXCmd1u8(GX_LOAD_CP_REG);
    GXCmd1u8(reg);
    GXCmd1u32(value);
}

inline void J3DFifoWriteXFCmd(u16 cmd, u16 len) {
    GXWGFifo.u8 = GX_LOAD_XF_REG;
    GXWGFifo.u16 = (len - 1);
    GXWGFifo.u16 = cmd;
}

inline void J3DFifoLoadXFCmdHdr(u16 addr, u8 len) {
    GXCmd1u8(GX_LOAD_XF_REG);
    GXCmd1u16(len - 1);
    GXCmd1u16(addr);
}

inline void J3DFifoLoadPosMtxIndx(u16 index, u32 addr) {
#if PLATFORM_PC || PLATFORM_NX_HB
    pal_gx_fifo_load_indx(0x20 /* GX_LOAD_INDX_A */,
                           index,
                           (u16)(((sizeof(Vec) - 1) << 12) | (u16)(addr * 4)));
#else
    GXCmd1u8(GX_LOAD_INDX_A);
    GXCmd1u16(index);
    GXCmd1u16(((sizeof(Vec) - 1) << 12) | (u16)(addr * 4));
#endif
}

inline void J3DFifoLoadNrmMtxIndx3x3(u16 index, u32 addr) {
#if PLATFORM_PC || PLATFORM_NX_HB
    pal_gx_fifo_load_indx(0x28 /* GX_LOAD_INDX_B */,
                           index,
                           (u16)(((9 - 1) << 12) | (u16)((addr * 3) + 0x400)));
#else
    GXCmd1u8(GX_LOAD_INDX_B);
    GXCmd1u16(index);
    GXCmd1u16(((9 - 1) << 12) | (u16)((addr * 3) + 0x400));
#endif
}

void J3DFifoLoadPosMtxImm(f32 (*)[4], u32);
void J3DFifoLoadNrmMtxImm(f32 (*)[4], u32);
void J3DFifoLoadNrmMtxImm3x3(f32 (*)[3], u32);
void J3DFifoLoadNrmMtxToTexMtx(f32 (*)[4], u32);
void J3DFifoLoadNrmMtxToTexMtx3x3(f32 (*)[3], u32);
void J3DFifoLoadTexCached(GXTexMapID, u32, GXTexCacheSize, u32, GXTexCacheSize);

#endif /* J3DFIFO_H */

#include "JSystem/JSystem.h" // IWYU pragma: keep

#include "JSystem/J3DGraphBase/J3DShapeDraw.h"
#include "JSystem/JKernel/JKRHeap.h"
#include "global.h"
#include <cstring>
#include <stdint.h>
#include <dolphin/gx.h>

#if PLATFORM_PC
/* Read a big-endian u16 from raw bytes — DL data is not byte-swapped on PC. */
static inline u16 dl_read_be_u16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}
/* Write a big-endian u16 to raw bytes. */
static inline void dl_write_be_u16(u8* p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v & 0xFF);
}
#endif

u32 J3DShapeDraw::countVertex(u32 stride) {
    u32 count = 0;
    u8* dlStart = (u8*)getDisplayList();

    for (u8* dl = dlStart; (dl - dlStart) < getDisplayListSize();) {
        u8 cmd = *(u8*)dl;
        dl++;
        if (cmd != GX_TRIANGLEFAN && cmd != GX_TRIANGLESTRIP)
            break;
#if PLATFORM_PC
        int vtxNum = dl_read_be_u16(dl);
#else
        int vtxNum = *((u16*)(dl));
#endif
        dl += 2;
        count += vtxNum;
        dl = (u8*)dl + stride * vtxNum;
    }

    return count;
}

void J3DShapeDraw::addTexMtxIndexInDL(u32 stride, u32 attrOffs, u32 valueBase) {
    u32 byteNum = countVertex(stride);
    u32 oldSize = mDisplayListSize;
    u32 newSize = ALIGN_NEXT(oldSize + byteNum, 0x20);
    u8* newDLStart = new (0x20) u8[newSize];
    u8* oldDLStart = (u8*)mDisplayList;
    u8* oldDL = oldDLStart;
    u8* newDL = newDLStart;

    for (; (oldDL - oldDLStart) < mDisplayListSize;) {
        // Copy command
        u8 cmd = *(u8*)oldDL;
        oldDL++;
        *newDL++ = cmd;

        if (cmd != GX_TRIANGLEFAN && cmd != GX_TRIANGLESTRIP)
            break;

        // Copy count (big-endian on GCN, read/write as BE on PC too)
#if PLATFORM_PC
        int vtxNum = dl_read_be_u16(oldDL);
#else
        int vtxNum = *(u16*)oldDL;
#endif
        oldDL += 2;
#if PLATFORM_PC
        dl_write_be_u16(newDL, (u16)vtxNum);
#else
        *(u16*)newDL = vtxNum;
#endif
        newDL += 2;

        for (int i = 0; i < vtxNum; i++) {
            u8* oldDLVtx = &oldDL[stride * i];
            u8 pnmtxidx = *oldDLVtx;
            memcpy(newDL, oldDLVtx, (int)attrOffs);
            newDL += attrOffs;
            *newDL++ = valueBase + pnmtxidx;
            memcpy(newDL, oldDLVtx + attrOffs, stride - attrOffs);
            newDL += (stride - attrOffs);
        }

        oldDL = (u8*)oldDL + stride * vtxNum;
    }

    u32 realSize = ALIGN_NEXT((uintptr_t)newDL - (uintptr_t)newDLStart, 0x20);
    for (; (newDL - newDLStart) < newSize; newDL++)
        *newDL = 0;

    mDisplayListSize = realSize;
    mDisplayList = newDLStart;
    DCStoreRange(newDLStart, mDisplayListSize);
}

J3DShapeDraw::J3DShapeDraw(const u8* displayList, u32 displayListSize) {
    mDisplayList = (void*)displayList;
    mDisplayListSize = displayListSize;
}

void J3DShapeDraw::draw() const {
#if PLATFORM_PC
    static int s_shapedraw_log = 0;
    if (s_shapedraw_log < 5) {
        fprintf(stderr, "{\"shapedraw\":{\"ptr\":\"%p\",\"size\":%u}}\n",
                mDisplayList, mDisplayListSize);
        s_shapedraw_log++;
    }
#endif
    GXCallDisplayList(mDisplayList, mDisplayListSize);
}

J3DShapeDraw::~J3DShapeDraw() {}

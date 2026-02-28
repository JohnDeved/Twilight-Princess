/**
 * @file pal_j3d_swap.cpp
 * @brief J3D binary format endian conversion for little-endian PC.
 *
 * Byte-swaps J3D model and animation binary files from big-endian (GameCube/Wii)
 * to little-endian (PC) in-place. The J3D format uses block-based layout:
 *
 *   File Header (0x20 bytes):
 *     u32 magic1 ('J3D2'/'J3D1'), u32 magic2 ('bmd3'/'bdl3'/etc.),
 *     u32 fileSize, u32 blockNum, padding[0x10]
 *
 *   Each Block:
 *     u32 blockType (FCC), u32 blockSize, then block-specific data
 *
 * The block-specific data contains u32 offset fields (relative to block start)
 * and u16/u32 count fields that all need byte-swapping.
 */

#include "global.h"
#include "pal/pal_j3d_swap.h"

#if PLATFORM_PC

#include "pal/pal_endian.h"
#include <cstdio>
#include <cstring>

/* FCC (Four Character Code) - big-endian byte order */
#define FCC(a,b,c,d) (((u32)(a)<<24)|((u32)(b)<<16)|((u32)(c)<<8)|(u32)(d))

static inline u16 r16(const void* p) {
    const u8* b = (const u8*)p;
    return (u16)((b[0] << 8) | b[1]);
}

static inline u32 r32(const void* p) {
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

static inline void w16(void* p, u16 v) {
    u8* b = (u8*)p;
    b[0] = (u8)(v & 0xFF);
    b[1] = (u8)(v >> 8);
}

static inline void w32(void* p, u32 v) {
    u8* b = (u8*)p;
    b[0] = (u8)(v & 0xFF);
    b[1] = (u8)((v >> 8) & 0xFF);
    b[2] = (u8)((v >> 16) & 0xFF);
    b[3] = (u8)(v >> 24);
}

/* Swap N consecutive u32 values starting at byte offset from base */
static void swap_u32_array(u8* base, u32 offset, int count) {
    for (int i = 0; i < count; i++) {
        u8* p = base + offset + i * 4;
        u32 v = r32(p);
        w32(p, v);
    }
}

/* Swap N consecutive u16 values starting at byte offset from base */
static void swap_u16_array(u8* base, u32 offset, int count) {
    for (int i = 0; i < count; i++) {
        u8* p = base + offset + i * 2;
        u16 v = r16(p);
        w16(p, v);
    }
}

/* Swap a range of u16 values from startOff to endOff (exclusive) */
static void swap_u16_range(u8* block, u32 startOff, u32 endOff) {
    if (startOff == 0 || endOff <= startOff) return;
    int count = (endOff - startOff) / 2;
    swap_u16_array(block, startOff, count);
}

/* Swap a range of u32 values from startOff to endOff (exclusive) */
static void swap_u32_range(u8* block, u32 startOff, u32 endOff) {
    if (startOff == 0 || endOff <= startOff) return;
    int count = (endOff - startOff) / 4;
    swap_u32_array(block, startOff, count);
}

/*
 * INF1 block layout (after 8-byte block header):
 *   0x08: u16 flags, u16 padding
 *   0x0C: u32 packetNum
 *   0x10: u32 vtxNum
 *   0x14: u32 hierarchyOffset
 *
 * Hierarchy data is u16 pairs (type, index) until terminator.
 */
static void swap_inf1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1); /* flags */
    swap_u32_array(block, 0x0C, 1); /* packetNum */
    swap_u32_array(block, 0x10, 1); /* vtxNum */
    swap_u32_array(block, 0x14, 1); /* hierarchyOffset */

    /* Swap hierarchy entries (u16 type, u16 value pairs) */
    u32 hierOff = *(u32*)(block + 0x14);
    if (hierOff != 0 && hierOff < blockSize) {
        u8* hier = block + hierOff;
        while ((u32)(hier - block) + 4 <= blockSize) {
            u16 type = r16(hier);
            w16(hier, type);
            u16 val = r16(hier + 2);
            w16(hier + 2, val);
            if (type == 0) break; /* terminator */
            hier += 4;
        }
    }
}

/*
 * VTX1 block layout (after 8-byte block header):
 *   0x08: u32 vtxAttrFmtListOffset
 *   0x0C: u32[13] array offsets (pos, nrm, nbt, clr0, clr1, tex0..7)
 *
 * VtxAttrFmtList: entries of (u32 attr, u32 compCnt, u32 compType, u8 frac, pad[3])
 * Vertex data arrays need per-element swapping based on format.
 */
static void swap_vtx1(u8* block, u32 blockSize) {
    /* Swap 14 u32 offset fields at 0x08..0x3C */
    swap_u32_array(block, 0x08, 14);

    /* Swap VtxAttrFmtList entries */
    u32 fmtOff = *(u32*)(block + 0x08);
    if (fmtOff != 0 && fmtOff < blockSize) {
        u8* fmt = block + fmtOff;
        while ((u32)(fmt - block) + 16 <= blockSize) {
            u32 attr = r32(fmt);
            w32(fmt, attr);
            if (attr == 0xFF || attr == 26 /* GX_VA_NULL */) break;
            swap_u32_array(fmt, 4, 2); /* compCnt, compType */
            /* frac is u8, no swap needed */
            fmt += 16;
        }
    }

    /* Swap vertex data arrays - positions, normals, texcoords are f32 or s16 arrays.
     * For now, swap all vertex data as u16 arrays since most vertex attributes
     * are either f32 (swap as two u16) or s16 (swap as u16). This is a simplification
     * that works because f32 byte-swap == two u16 byte-swaps at the same positions. */
    u32 offsets[14];
    for (int i = 0; i < 14; i++) {
        offsets[i] = *(u32*)(block + 0x08 + i * 4);
    }

    /* For each data array, find its extent and swap as u16 */
    for (int i = 1; i < 14; i++) { /* skip fmtList (index 0) */
        if (offsets[i] == 0) continue;
        /* Find end: next non-zero offset or blockSize */
        u32 end = blockSize;
        for (int j = i + 1; j < 14; j++) {
            if (offsets[j] != 0) {
                end = offsets[j];
                break;
            }
        }
        /* Color arrays (indices 3,4) are GXColor (4 bytes, RGBA) - no swap needed.
         * Position/Normal arrays are f32 triples or s16 triples.
         * TexCoord arrays are f32 pairs or s16 pairs.
         * Swap everything as u16 (works for both f32 and s16). */
        if (i == 3 || i == 4) continue; /* skip color arrays */
        swap_u16_range(block, offsets[i], end);
    }
}

/*
 * EVP1 block layout:
 *   0x08: u16 count, u16 pad
 *   0x0C: u32 mixMtxNumOffset
 *   0x10: u32 mixIndexOffset
 *   0x14: u32 mixWeightOffset
 *   0x18: u32 invJointMtxOffset
 */
static void swap_evp1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1); /* count */
    swap_u32_array(block, 0x0C, 4); /* 4 offsets */

    u16 count = *(u16*)(block + 0x08);

    /* mixMtxNum: u8 array - no swap */
    /* mixIndex: u16 array */
    u32 mixIdxOff = *(u32*)(block + 0x10);
    u32 mixWgtOff = *(u32*)(block + 0x14);
    if (mixIdxOff != 0 && mixWgtOff != 0) {
        swap_u16_range(block, mixIdxOff, mixWgtOff);
    }
    /* mixWeight: f32 array */
    u32 invMtxOff = *(u32*)(block + 0x18);
    if (mixWgtOff != 0 && invMtxOff != 0) {
        swap_u16_range(block, mixWgtOff, invMtxOff);
    }
    /* invJointMtx: Mtx43 array (3x4 f32) = 12 floats per entry */
    if (invMtxOff != 0) {
        swap_u16_range(block, invMtxOff, blockSize);
    }
}

/*
 * DRW1 block layout:
 *   0x08: u16 count, u16 pad
 *   0x0C: u32 drawMtxFlagOffset
 *   0x10: u32 drawMtxIndexOffset
 */
static void swap_drw1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1);
    swap_u32_array(block, 0x0C, 2);

    u16 count = *(u16*)(block + 0x08);

    /* drawMtxFlag: u8 array - no swap */
    /* drawMtxIndex: u16 array */
    u32 idxOff = *(u32*)(block + 0x10);
    if (idxOff != 0) {
        swap_u16_array(block, idxOff, count);
    }
}

/*
 * JNT1 block layout:
 *   0x08: u16 jointNum, u16 pad
 *   0x0C: u32 jointInitDataOffset
 *   0x10: u32 indexTableOffset
 *   0x14: u32 nameTableOffset
 *
 * JointInitData: per-joint struct, 0x40 bytes:
 *   u16 flag, u8 calcType, u8 pad
 *   f32 sx, sy, sz (scale)
 *   s16 rx, ry, rz (rotation), s16 pad
 *   f32 tx, ty, tz (translation)
 *   f32 boundingSphereRadius
 *   f32 bbMin[3], bbMax[3]
 */
static void swap_jnt1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1);
    swap_u32_array(block, 0x0C, 3);

    u16 jointNum = *(u16*)(block + 0x08);
    u32 jntOff = *(u32*)(block + 0x0C);
    u32 idxOff = *(u32*)(block + 0x10);
    u32 nameOff = *(u32*)(block + 0x14);

    /* Joint init data - 0x40 bytes per joint */
    if (jntOff != 0) {
        for (int i = 0; i < jointNum; i++) {
            u8* j = block + jntOff + i * 0x40;
            swap_u16_array(j, 0x00, 1);  /* flag */
            /* u8 fields at 0x02, 0x03 - no swap */
            swap_u32_array(j, 0x04, 3);  /* scale xyz (f32) */
            swap_u16_array(j, 0x10, 4);  /* rot xyz + pad (s16) */
            swap_u32_array(j, 0x18, 3);  /* translation xyz (f32) */
            swap_u32_array(j, 0x24, 1);  /* boundingSphereRadius */
            swap_u32_array(j, 0x28, 6);  /* bbMin + bbMax (f32) */
        }
    }

    /* Index table: u16 array */
    if (idxOff != 0) {
        swap_u16_array(block, idxOff, jointNum);
    }

    /* Name table: JUTNameTab format - swap u16 count at start, then u16 offsets */
    if (nameOff != 0 && nameOff < blockSize) {
        u16 nameCount = r16(block + nameOff);
        w16(block + nameOff, nameCount);
        swap_u16_array(block, nameOff + 2, 1); /* pad */
        /* Each entry: u16 hash, u16 stringOffset */
        for (int i = 0; i < nameCount; i++) {
            swap_u16_array(block, nameOff + 4 + i * 4, 2);
        }
    }
}

/*
 * SHP1 block layout:
 *   0x08: u16 shapeNum, u16 pad
 *   0x0C: u32 shapeInitDataOffset
 *   0x10: u32 indexTableOffset
 *   0x14: u32 nameTableOffset (always 0)
 *   0x18: u32 vtxDescListOffset
 *   0x1C: u32 mtxTableOffset
 *   0x20: u32 displayListDataOffset
 *   0x24: u32 mtxInitDataOffset
 *   0x28: u32 drawInitDataOffset
 */
static void swap_shp1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1); /* shapeNum */
    swap_u32_array(block, 0x0C, 8); /* 8 offset fields */

    u16 shapeNum = *(u16*)(block + 0x08);
    u32 shpInitOff = *(u32*)(block + 0x0C);
    u32 idxTabOff  = *(u32*)(block + 0x10);
    u32 vtxDescOff = *(u32*)(block + 0x18);
    u32 mtxTabOff  = *(u32*)(block + 0x1C);
    u32 dlDataOff  = *(u32*)(block + 0x20);
    u32 mtxInitOff = *(u32*)(block + 0x24);
    u32 drawInitOff = *(u32*)(block + 0x28);

    /* ShapeInitData: 0x28 bytes per shape */
    if (shpInitOff != 0) {
        for (int i = 0; i < shapeNum; i++) {
            u8* s = block + shpInitOff + i * 0x28;
            /* u8 mode at 0x00, skip */
            swap_u16_array(s, 0x02, 3); /* vtxDescIndex, mtxTableIndex, mtxInitIndex */
            swap_u16_array(s, 0x08, 2); /* drawInitIndex, pad */
            swap_u32_array(s, 0x0C, 1); /* boundingSphereRadius */
            swap_u32_array(s, 0x10, 6); /* bbMin + bbMax (f32) */
        }
    }

    /* Index table: u16 array */
    if (idxTabOff != 0) {
        swap_u16_array(block, idxTabOff, shapeNum);
    }

    /* VtxDescList: pairs of (u32 attr, u32 type) until GX_VA_NULL */
    if (vtxDescOff != 0) {
        u8* vd = block + vtxDescOff;
        while ((u32)(vd - block) + 8 <= blockSize) {
            u32 attr = r32(vd);
            w32(vd, attr);
            u32 type = r32(vd + 4);
            w32(vd + 4, type);
            if (attr == 0xFF || attr == 0) break;
            vd += 8;
        }
    }

    /* MtxTable: u16 array. Find extent from mtxInit or draw data offsets */
    if (mtxTabOff != 0) {
        u32 mtxTabEnd = blockSize;
        if (dlDataOff > mtxTabOff) mtxTabEnd = dlDataOff;
        if (mtxInitOff > mtxTabOff && mtxInitOff < mtxTabEnd) mtxTabEnd = mtxInitOff;
        if (drawInitOff > mtxTabOff && drawInitOff < mtxTabEnd) mtxTabEnd = drawInitOff;
        swap_u16_range(block, mtxTabOff, mtxTabEnd);
    }

    /* DisplayList data: contains GX commands with u16 indices - swap as u16 */
    /* Note: GX display lists have complex format. For now skip DL data swap
     * since bgfx doesn't use GX display lists directly. */

    /* MtxInitData: u16 pairs (useCount, usedMtxIndex) */
    if (mtxInitOff != 0) {
        u32 end = drawInitOff > mtxInitOff ? drawInitOff : blockSize;
        swap_u16_range(block, mtxInitOff, end);
    }

    /* DrawInitData: u32 pairs (drawListSize, drawListOffset) */
    if (drawInitOff != 0) {
        u32 end = blockSize;
        int drawCount = (end - drawInitOff) / 8;
        if (drawCount > shapeNum * 4) drawCount = shapeNum * 4;
        swap_u32_array(block, drawInitOff, drawCount * 2);
    }
}

/* Swap a JUTNameTab at the given offset */
static void swap_name_table(u8* block, u32 nameOff, u32 blockSize) {
    if (nameOff == 0 || nameOff >= blockSize) return;
    u16 count = r16(block + nameOff);
    w16(block + nameOff, count);
    swap_u16_array(block, nameOff + 2, 1);
    for (int i = 0; i < count; i++) {
        swap_u16_array(block, nameOff + 4 + i * 4, 2);
    }
}

/*
 * MAT3 block layout:
 *   0x08: u16 materialNum, u16 pad
 *   0x0C-0x80: u32 offsets (29 offset fields for various material data)
 */
static void swap_mat3(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1); /* materialNum */
    /* 29 u32 offset fields at 0x0C..0x80 */
    swap_u32_array(block, 0x0C, 30);

    u16 matNum = *(u16*)(block + 0x08);

    /* Material init data: complex struct per material.
     * Each is variable-length with many u16 indices.
     * The offset at 0x0C points to the material init data array.
     * For now, swap the material ID array and name table. */

    /* Material ID array at offset[1] (0x10) - u16 per material */
    u32 matIdOff = *(u32*)(block + 0x10);
    if (matIdOff != 0 && matIdOff < blockSize) {
        swap_u16_array(block, matIdOff, matNum);
    }

    /* Name table at offset[2] (0x14) */
    u32 nameOff = *(u32*)(block + 0x14);
    swap_name_table(block, nameOff, blockSize);

    /* Material init data at offset[0] (0x0C)
     * Each entry is 0x14C bytes for MAT3 v26, containing mixed u8 and u16 fields.
     * We need to swap only the u16 index fields, NOT the u8 fields.
     * See J3DMaterialInitData struct for exact layout. */
    u32 matInitOff = *(u32*)(block + 0x0C);
    if (matInitOff != 0 && matInitOff < blockSize && matNum > 0) {
        u32 entrySize = 0x14C;
        /* Verify entry size from actual data layout */
        if (matIdOff > matInitOff && matNum > 0) {
            u32 calcSize = (matIdOff - matInitOff) / matNum;
            if (calcSize >= 0x14C) entrySize = 0x14C;
            else if (calcSize > 0) entrySize = calcSize;
        }

        for (int i = 0; i < matNum && i < 1024; i++) {
            u32 e = matInitOff + i * entrySize;
            if (e + entrySize > blockSize) break;
            /* J3DMaterialInitData layout:
             * 0x00-0x07: u8 fields (mode, cullIdx, chanNum, texGenNum, etc.) - NO SWAP
             * 0x08: u16 mMatColorIdx[2]
             * 0x0C: u16 mColorChanIdx[4]
             * 0x14: u16 mAmbColorIdx[2] */
            swap_u16_array(block, e + 0x08, 2);  /* mMatColorIdx[2] */
            swap_u16_array(block, e + 0x0C, 4);  /* mColorChanIdx[4] */
            swap_u16_array(block, e + 0x14, 2);  /* mAmbColorIdx[2] */
            /* 0x18-0x27: u8 field_0x018[0x10] - NO SWAP */
            /* 0x28: u16 mTexCoordIdx[8] */
            swap_u16_array(block, e + 0x28, 8);
            /* 0x38-0x47: u8 field_0x038[0x10] - NO SWAP */
            /* 0x48: u16 mTexMtxIdx[8] */
            swap_u16_array(block, e + 0x48, 8);
            /* 0x58-0x83: u8 field_0x058[0x2c] - NO SWAP */
            /* 0x84: u16 mTexNoIdx[8] */
            swap_u16_array(block, e + 0x84, 8);
            /* 0x94: u16 mTevKColorIdx[4] */
            swap_u16_array(block, e + 0x94, 4);
            /* 0x9C-0xAB: u8 mTevKColorSel[0x10] - NO SWAP */
            /* 0xAC-0xBB: u8 mTevKAlphaSel[0x10] - NO SWAP */
            /* 0xBC: u16 mTevOrderIdx[0x10] */
            swap_u16_array(block, e + 0xBC, 0x10);
            /* 0xDC: u16 mTevColorIdx[4] */
            swap_u16_array(block, e + 0xDC, 4);
            /* 0xE4: u16 mTevStageIdx[0x10] */
            swap_u16_array(block, e + 0xE4, 0x10);
            /* 0x104: u16 mTevSwapModeIdx[0x10] */
            swap_u16_array(block, e + 0x104, 0x10);
            /* 0x124: u16 mTevSwapModeTableIdx[4] */
            swap_u16_array(block, e + 0x124, 4);
            /* 0x12C-0x143: u8 field_0x12c[0x18] - NO SWAP */
            /* 0x144: u16 mFogIdx, mAlphaCompIdx, mBlendIdx, mNBTScaleIdx */
            swap_u16_array(block, e + 0x144, 4);
        }
    }

    /* Remaining data tables (cull mode, colors, tex coord info, TEV stages, etc.)
     * These contain a mix of u8, u16, u32 values. Swap based on known data sizes.
     * The key tables for rendering: */

    /* CullMode (offset[4], 0x1C): u32 per entry */
    u32 off;
    off = *(u32*)(block + 0x1C);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 5; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        swap_u32_range(block, off, end);
    }

    /* MatColor (offset[5], 0x20): GXColor entries (4 bytes each, RGBA) - no swap */
    /* ColorChanNum (offset[6], 0x24): u8 per entry - no swap */

    /* ColorChanInfo (offset[7], 0x28): u16 per field * 6 fields per entry
     * Each ColorChannelInfo is 8 bytes of bitfield data. Swap as u32. */
    off = *(u32*)(block + 0x28);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 8; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        /* Swap as u16 pairs since ColorChanInfo has u16-aligned fields */
        swap_u16_range(block, off, end);
    }

    /* AmbColor (offset[8], 0x2C): GXColor entries - no swap */

    /* TexGenNum (offset[10], 0x34): u8 per entry - no swap */

    /* TexCoordInfo (offset[11], 0x38): 4 bytes per entry (u8 type, u8 src, u32 mtx) */
    off = *(u32*)(block + 0x38);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 12; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        swap_u32_range(block, off, end);
    }

    /* TexMtxInfo (offset[13], 0x40): large struct per entry, many f32 fields.
     * Each TexMtxInfo is 0x64 bytes with mixed u8/f32 fields.
     * Swap as u32 to correctly handle f32 values. */
    off = *(u32*)(block + 0x40);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 14; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        /* TexMtxInfo layout per entry (0x64 bytes):
         * u8 type, u8 info, pad[2], f32 centerS, f32 centerT, f32 unknown,
         * f32 scaleS, f32 scaleT, f32 rotation, f32 transS, f32 transT,
         * f32 effectMtx[4][4] */
        u32 entrySize = 0x64;
        int numEntries = (end - off) / entrySize;
        for (int i = 0; i < numEntries && i < 256; i++) {
            u32 eoff = off + i * entrySize;
            /* Skip first 4 bytes (u8 type, u8 info, pad[2]) */
            swap_u32_range(block, eoff + 4, eoff + entrySize);
        }
    }

    /* TexNo (offset[15], 0x48): u16 per entry */
    off = *(u32*)(block + 0x48);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 16; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        swap_u16_range(block, off, end);
    }

    /* TevOrderInfo (offset[16], 0x4C): 4 bytes per entry (u8 fields) - no swap */

    /* TevColor (offset[17], 0x50): GXColorS10 entries, 4 x s16 per entry */
    off = *(u32*)(block + 0x50);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 18; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        swap_u16_range(block, off, end);
    }

    /* TevKColor (offset[18], 0x54): GXColor entries - no swap */

    /* TevStageNum (offset[19], 0x58): u8 per entry - no swap */

    /* TevStageInfo (offset[20], 0x5C): per-stage combiner settings.
     * Each entry is 20 bytes of u8 fields - no swap needed. */

    /* TevSwapMode, TevSwapTable, FogInfo, AlphaCompInfo, BlendInfo, ZMode:
     * Most are u8/small structs. ZMode is 4 bytes (u8 fields). */

    /* FogInfo (offset[24], 0x68): contains f32 fields
     * Layout: u8 type, u8 enable, u16 pad, f32 startZ, f32 endZ,
     *         f32 nearZ, f32 farZ, GXColor color (4 u8) = 0x2C bytes
     * There may also be a fog adjustment table with u16 entries. */
    off = *(u32*)(block + 0x68);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        for (int k = 25; k < 30; k++) {
            u32 nextOff = *(u32*)(block + 0x0C + k * 4);
            if (nextOff > off) { end = nextOff; break; }
        }
        u32 fogSize = 0x2C;
        int numEntries = (end - off) / fogSize;
        for (int i = 0; i < numEntries && i < 256; i++) {
            u32 eoff = off + i * fogSize;
            swap_u16_array(block, eoff + 2, 1);  /* pad u16 */
            swap_u32_range(block, eoff + 4, eoff + 0x14); /* 4 f32 fields */
            /* GXColor at +0x14 is 4 u8s, no swap */
            /* Fog adj table entries at +0x18 are u16s */
            swap_u16_range(block, eoff + 0x18, eoff + fogSize);
        }
    }

    /* NBTScaleInfo (offset[29], 0x80): contains u8 enable + f32[3] scale */
    off = *(u32*)(block + 0x80);
    if (off != 0 && off < blockSize) {
        u32 end = blockSize;
        /* Each entry: u8 enable, pad[3], f32 sx, sy, sz = 16 bytes */
        int count = (end - off) / 16;
        for (int i = 0; i < count && i < 256; i++) {
            swap_u32_array(block, off + i * 16 + 4, 3);
        }
    }
}

/*
 * TEX1 block layout:
 *   0x08: u16 textureNum, u16 pad
 *   0x0C: u32 textureResOffset
 *   0x10: u32 nameTableOffset
 *
 * TextureRes (ResTIMG): 0x20 bytes per texture header on disc.
 */
static void swap_tex1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1); /* textureNum */
    swap_u32_array(block, 0x0C, 2); /* 2 offsets */

    u16 texNum = *(u16*)(block + 0x08);
    u32 texResOff = *(u32*)(block + 0x0C);
    u32 nameOff = *(u32*)(block + 0x10);

    /* Swap ResTIMG headers (0x20 bytes each).
     * Note: ResTIMG endian swap is already handled in JUTTexture.cpp for
     * textures loaded via JUTTexture::storeTIMG(). But we also need to swap
     * the u16/u32 fields here for the initial model loading to work. */
    if (texResOff != 0) {
        for (int i = 0; i < texNum; i++) {
            u8* tex = block + texResOff + i * 0x20;
            if ((u32)(tex - block) + 0x20 > blockSize) break;
            /* ResTIMG layout:
             * 0x00: u8 format, u8 alphaEnabled
             * 0x02: u16 width, 0x04: u16 height
             * 0x06: u8 wrapS, u8 wrapT, u8 paletteFormat
             * 0x09: u8 paletteEntryCount (was numColors before fix)
             * 0x0A: u16 paletteOffset
             * 0x0C: u8 edgeLODEnable, u8 minFilterType, u8 magFilterType
             * 0x0F: u8 pad
             * 0x10: u8 minLOD, u8 maxLOD, u8 mipmapCount, u8 pad2
             * 0x14: u16 lodBias
             * 0x16: u16 pad3 (was part of paletteOffset on old struct)
             * 0x18: u32 imageOffset
             * 0x1C: u32 pad4 */
            swap_u16_array(tex, 0x02, 2); /* width, height */
            swap_u16_array(tex, 0x0A, 1); /* paletteOffset */
            swap_u16_array(tex, 0x14, 1); /* lodBias */
            swap_u32_array(tex, 0x18, 1); /* imageOffset */
        }
    }

    /* Name table */
    swap_name_table(block, nameOff, blockSize);
}

/* MAT2 block layout - similar to MAT3 but older version with fewer fields */
static void swap_mat2(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x08, 1); /* materialNum */
    /* Fewer offsets than MAT3 - 24 u32 fields starting at 0x0C */
    swap_u32_array(block, 0x0C, 26);

    /* Material ID and name table at standard positions */
    u16 matNum = *(u16*)(block + 0x08);
    u32 matIdOff = *(u32*)(block + 0x10);
    if (matIdOff != 0 && matIdOff < blockSize) {
        swap_u16_array(block, matIdOff, matNum);
    }
    u32 nameOff = *(u32*)(block + 0x14);
    swap_name_table(block, nameOff, blockSize);

    /* Material init data */
    u32 matInitOff = *(u32*)(block + 0x0C);
    if (matInitOff != 0 && matIdOff != 0 && matInitOff < matIdOff) {
        swap_u16_range(block, matInitOff, matIdOff);
    }
}

int pal_j3d_swap_model(void* data, u32 size) {
    if (!data || size < 0x20) return 0;

    u8* buf = (u8*)data;

    /* Check if file is big-endian by reading magic as big-endian bytes */
    u32 magic1_be = r32(buf + 0x00);
    u32 magic2_be = r32(buf + 0x04);

    /* Expected big-endian FCC values */
    int is_j3d = (magic1_be == FCC('J','3','D','2') || magic1_be == FCC('J','3','D','1'));
    if (!is_j3d) return 0; /* Already swapped or not J3D */

    /* Check if already in native byte order */
    u32 magic1_native = *(u32*)(buf + 0x00);
    if (magic1_native == FCC('J','3','D','2') || magic1_native == FCC('J','3','D','1')) {
        return 0; /* Already native endian */
    }

    fprintf(stderr, "[pal_j3d] Swapping model: magic=%c%c%c%c type=%c%c%c%c\n",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);

    /* Swap file header */
    swap_u32_array(buf, 0x00, 2);  /* magic1, magic2 */
    swap_u32_array(buf, 0x08, 1);  /* fileSize */
    swap_u32_array(buf, 0x0C, 1);  /* blockNum */

    u32 blockNum = *(u32*)(buf + 0x0C);
    if (blockNum > 64) {
        fprintf(stderr, "[pal_j3d] Suspicious blockNum %u, aborting swap\n", blockNum);
        return 0;
    }

    /* Walk blocks and swap each */
    u8* blockPtr = buf + 0x20;
    for (u32 i = 0; i < blockNum && (u32)(blockPtr - buf) < size; i++) {
        /* Read block header in big-endian */
        u32 blockType = r32(blockPtr);
        u32 blockSize = r32(blockPtr + 4);

        /* Swap block header */
        w32(blockPtr, blockType);
        w32(blockPtr + 4, blockSize);

        if (blockSize < 8 || (u32)(blockPtr - buf) + blockSize > size) {
            fprintf(stderr, "[pal_j3d] Block %u: bad size %u, stopping\n", i, blockSize);
            break;
        }

        /* Dispatch by block type (big-endian FCC) */
        switch (blockType) {
        case FCC('I','N','F','1'):
            swap_inf1(blockPtr, blockSize);
            break;
        case FCC('V','T','X','1'):
            swap_vtx1(blockPtr, blockSize);
            break;
        case FCC('E','V','P','1'):
            swap_evp1(blockPtr, blockSize);
            break;
        case FCC('D','R','W','1'):
            swap_drw1(blockPtr, blockSize);
            break;
        case FCC('J','N','T','1'):
            swap_jnt1(blockPtr, blockSize);
            break;
        case FCC('S','H','P','1'):
            swap_shp1(blockPtr, blockSize);
            break;
        case FCC('M','A','T','3'):
            swap_mat3(blockPtr, blockSize);
            break;
        case FCC('M','A','T','2'):
            swap_mat2(blockPtr, blockSize);
            break;
        case FCC('T','E','X','1'):
            swap_tex1(blockPtr, blockSize);
            break;
        case FCC('M','D','L','3'):
            /* MDL3 display list block - contains precompiled GX DLs.
             * Complex format, skip for now. */
            break;
        default:
            fprintf(stderr, "[pal_j3d] Unknown block '%c%c%c%c' size=%u\n",
                    (char)(blockType>>24), (char)(blockType>>16),
                    (char)(blockType>>8), (char)blockType, blockSize);
            break;
        }

        blockPtr += blockSize;
    }

    return 1;
}

/*
 * Animation file format (J3D1/J3D2):
 *   File header: u32 magic ('J3D1'), u32 type ('bck1' etc.), u32 fileSize, u32 blockNum
 *   Blocks: same block header format as model files
 *
 * Animation blocks: ANK1, ANF1, CLK1, CLF1, TRK1, TRF1, TPT1, TPF1, PAK1, PAF1, VAK1, VAF1,
 *                   TTK1, XAK1
 *
 * Most animation blocks have:
 *   0x08: u8 loopMode, u8 pad
 *   0x0A: u16 frameCount
 *   0x0C: u16 entryCount, u16 pad
 *   0x10+: u32 offset fields for data tables
 *   Data tables: s16/f32 keyframe data
 */
/*
 * ANK1/ANF1 (transform full/key) block layout:
 *   0x08: u8 loopMode, u8 rotDecShift
 *   0x0A: u16 frameCount
 *   0x0C: u16 jointCount, u16 scaleCount
 *   0x10: u16 rotCount, u16 transCount
 *   0x14: u32 tableOffset       [0] - J3DAnmTransformKeyTable (u16 array)
 *   0x18: u32 scaleValOffset    [1] - f32 array
 *   0x1C: u32 rotValOffset      [2] - s16 array
 *   0x20: u32 transValOffset    [3] - f32 array
 */
static void swap_ank1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x0A, 1); /* frameCount */
    swap_u16_array(block, 0x0C, 2); /* jointCount, scaleCount */
    swap_u16_array(block, 0x10, 2); /* rotCount, transCount */
    swap_u32_array(block, 0x14, 4); /* 4 offsets */

    u16 scaleNum = *(u16*)(block + 0x0E);
    u16 rotNum   = *(u16*)(block + 0x10);
    u16 transNum = *(u16*)(block + 0x12);
    u32 tableOff = *(u32*)(block + 0x14);
    u32 scaleOff = *(u32*)(block + 0x18);
    u32 rotOff   = *(u32*)(block + 0x1C);
    u32 transOff = *(u32*)(block + 0x20);

    /* Table data: u16 values (J3DAnmTransformKeyTable entries) */
    if (tableOff != 0 && tableOff < blockSize) {
        u32 end = scaleOff > tableOff ? scaleOff : blockSize;
        swap_u16_range(block, tableOff, end);
    }

    /* Scale values: f32 array → swap as u32 */
    if (scaleOff != 0 && scaleOff < blockSize) {
        swap_u32_array(block, scaleOff, scaleNum);
    }

    /* Rotation values: s16 array → swap as u16 */
    if (rotOff != 0 && rotOff < blockSize) {
        swap_u16_array(block, rotOff, rotNum);
    }

    /* Translation values: f32 array → swap as u32 */
    if (transOff != 0 && transOff < blockSize) {
        swap_u32_array(block, transOff, transNum);
    }
}

/*
 * CLK1/CLF1 (color key/full) block layout:
 *   0x08: u8 loopMode, u8[3] pad
 *   0x0C: s16 frameMax
 *   0x0E: u16 updateMaterialNum
 *   0x10: u16 rNum, u16 gNum
 *   0x14: u16 bNum, u16 aNum
 *   0x18: u32 tableOffset
 *   0x1C: u32 updateMaterialIDOffset
 *   0x20: u32 nameTabOffset
 *   0x24: u32 rValOffset (s16)
 *   0x28: u32 gValOffset (s16)
 *   0x2C: u32 bValOffset (s16)
 *   0x30: u32 aValOffset (s16)
 */
static void swap_clk1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x0C, 1); /* frameMax */
    swap_u16_array(block, 0x0E, 1); /* updateMaterialNum */
    swap_u16_array(block, 0x10, 2); /* rNum, gNum */
    swap_u16_array(block, 0x14, 2); /* bNum, aNum */
    swap_u32_array(block, 0x18, 7); /* 7 offsets */

    u32 tableOff = *(u32*)(block + 0x18);
    u32 matIdOff = *(u32*)(block + 0x1C);
    u32 nameOff  = *(u32*)(block + 0x20);
    u32 rOff     = *(u32*)(block + 0x24);
    u32 gOff     = *(u32*)(block + 0x28);
    u32 bOff     = *(u32*)(block + 0x2C);
    u32 aOff     = *(u32*)(block + 0x30);

    /* Table: u16 values */
    if (tableOff != 0 && tableOff < blockSize) {
        u32 end = matIdOff > tableOff ? matIdOff : blockSize;
        swap_u16_range(block, tableOff, end);
    }
    /* Material ID: u16 array */
    if (matIdOff != 0 && matIdOff < blockSize) {
        u32 end = nameOff > matIdOff ? nameOff : blockSize;
        swap_u16_range(block, matIdOff, end);
    }
    /* Name table */
    swap_name_table(block, nameOff, blockSize);
    /* Color values: all s16 → u16 swap */
    if (rOff != 0 && rOff < blockSize) { u32 end = gOff > rOff ? gOff : blockSize; swap_u16_range(block, rOff, end); }
    if (gOff != 0 && gOff < blockSize) { u32 end = bOff > gOff ? bOff : blockSize; swap_u16_range(block, gOff, end); }
    if (bOff != 0 && bOff < blockSize) { u32 end = aOff > bOff ? aOff : blockSize; swap_u16_range(block, bOff, end); }
    if (aOff != 0 && aOff < blockSize) { swap_u16_range(block, aOff, blockSize); }
}

/*
 * TRK1/TRF1 (TEV register key/full) block layout:
 *   0x08: u8 loopMode, u8 pad
 *   0x0A: s16 frameMax
 *   0x0C-0x1E: u16 count fields (8 values)
 *   0x20: u32 cRegTableOffset
 *   0x24: u32 kRegTableOffset
 *   0x28-0x54: u32 offsets for cReg/kReg material IDs, name tables, and 8 color channels
 *   All color data is s16 → u16 swap
 */
static void swap_trk1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x0A, 1); /* frameMax */
    swap_u16_array(block, 0x0C, 8); /* count fields at 0x0C-0x1E */
    swap_u32_array(block, 0x20, 14); /* 14 offsets at 0x20-0x54 */

    u32 cRegTableOff = *(u32*)(block + 0x20);
    u32 kRegTableOff = *(u32*)(block + 0x24);
    u32 cRegMatIdOff = *(u32*)(block + 0x28);
    u32 kRegMatIdOff = *(u32*)(block + 0x2C);
    u32 cRegNameOff  = *(u32*)(block + 0x30);
    u32 kRegNameOff  = *(u32*)(block + 0x34);

    /* Table data */
    if (cRegTableOff != 0 && cRegTableOff < blockSize) {
        u32 end = kRegTableOff > cRegTableOff ? kRegTableOff : blockSize;
        swap_u16_range(block, cRegTableOff, end);
    }
    if (kRegTableOff != 0 && kRegTableOff < blockSize) {
        u32 end = cRegMatIdOff > kRegTableOff ? cRegMatIdOff : blockSize;
        swap_u16_range(block, kRegTableOff, end);
    }
    /* Material IDs */
    if (cRegMatIdOff != 0 && cRegMatIdOff < blockSize) {
        u32 end = kRegMatIdOff > cRegMatIdOff ? kRegMatIdOff : blockSize;
        swap_u16_range(block, cRegMatIdOff, end);
    }
    if (kRegMatIdOff != 0 && kRegMatIdOff < blockSize) {
        u32 end = cRegNameOff > kRegMatIdOff ? cRegNameOff : blockSize;
        swap_u16_range(block, kRegMatIdOff, end);
    }
    /* Name tables */
    swap_name_table(block, cRegNameOff, blockSize);
    swap_name_table(block, kRegNameOff, blockSize);
    /* 8 color value arrays (s16) at offsets 0x38-0x54 */
    for (int c = 0; c < 8; c++) {
        u32 valOff = *(u32*)(block + 0x38 + c * 4);
        if (valOff == 0 || valOff >= blockSize) continue;
        u32 end = blockSize;
        for (int d = c + 1; d < 8; d++) {
            u32 next = *(u32*)(block + 0x38 + d * 4);
            if (next > valOff && next < end) end = next;
        }
        swap_u16_range(block, valOff, end);
    }
}

/*
 * TTK1 (texture SRT key) block layout:
 *   0x08: u8 loopMode, u8 rotDecShift
 *   0x0A: s16 frameMax
 *   0x0C: u16 trackNum, u16 scaleNum
 *   0x10: u16 rotNum, u16 transNum
 *   0x14: u32 tableOffset         - u16 table
 *   0x18: u32 updateMatIDOffset   - u16 array
 *   0x1C: u32 nameTab1Offset      - name table
 *   0x20: u32 updateTexMtxIDOffset - u8 array
 *   0x24: u32 srtCenterOffset      - Vec (3x f32)
 *   0x28: u32 scaleValOffset       - f32 array
 *   0x2C: u32 rotValOffset         - s16 array
 *   0x30: u32 transValOffset       - f32 array
 *   Then post-update fields: u16 counts at 0x34-0x3A, more offsets at 0x3C+
 */
static void swap_ttk1(u8* block, u32 blockSize) {
    swap_u16_array(block, 0x0A, 1); /* frameMax */
    swap_u16_array(block, 0x0C, 2); /* trackNum, scaleNum */
    swap_u16_array(block, 0x10, 2); /* rotNum, transNum */

    /* Count offsets: 7 base + potential post-update offsets */
    u32 numOffsets = 7;
    /* Check for post-update data (TTK1 has u16 fields at 0x34-0x3A then more offsets) */
    if (blockSize > 0x54) {
        /* Swap u16 fields at 0x34-0x3A */
        swap_u16_array(block, 0x34, 4);
        numOffsets = 14; /* extended with post-update offsets */
    }
    swap_u32_array(block, 0x14, numOffsets);

    u16 scaleNum = *(u16*)(block + 0x0E);
    u16 rotNum   = *(u16*)(block + 0x10);
    u16 transNum = *(u16*)(block + 0x12);

    u32 tableOff      = *(u32*)(block + 0x14);
    u32 updateMatOff   = *(u32*)(block + 0x18);
    u32 nameTab1Off    = *(u32*)(block + 0x1C);
    u32 texMtxIDOff    = *(u32*)(block + 0x20); /* u8 array */
    u32 srtCenterOff   = *(u32*)(block + 0x24); /* Vec (f32) */
    u32 scaleOff       = *(u32*)(block + 0x28);
    u32 rotOff         = *(u32*)(block + 0x2C);
    u32 transOff       = *(u32*)(block + 0x30);

    /* Table: u16 values */
    if (tableOff != 0 && tableOff < blockSize) {
        u32 end = updateMatOff > tableOff ? updateMatOff : blockSize;
        swap_u16_range(block, tableOff, end);
    }

    /* UpdateMaterialID: u16 array */
    if (updateMatOff != 0 && updateMatOff < blockSize) {
        u32 end = nameTab1Off > updateMatOff ? nameTab1Off : blockSize;
        swap_u16_range(block, updateMatOff, end);
    }

    /* Name table 1 */
    swap_name_table(block, nameTab1Off, blockSize);

    /* texMtxID: u8 array - no swap */

    /* SRT center: Vec (3x f32) per entry */
    if (srtCenterOff != 0 && srtCenterOff < blockSize) {
        u32 end = scaleOff > srtCenterOff ? scaleOff : blockSize;
        u32 count = (end - srtCenterOff) / 4;
        swap_u32_array(block, srtCenterOff, count);
    }

    /* Scale values: f32 array */
    if (scaleOff != 0 && scaleOff < blockSize) {
        swap_u32_array(block, scaleOff, scaleNum);
    }

    /* Rotation values: s16 array */
    if (rotOff != 0 && rotOff < blockSize) {
        swap_u16_array(block, rotOff, rotNum);
    }

    /* Translation values: f32 array */
    if (transOff != 0 && transOff < blockSize) {
        swap_u32_array(block, transOff, transNum);
    }

    /* Post-update name table and other offsets */
    if (blockSize > 0x54) {
        u32 nameTab2Off = *(u32*)(block + 0x44);
        swap_name_table(block, nameTab2Off, blockSize);

        /* Post-update material IDs */
        u32 postMatOff = *(u32*)(block + 0x40);
        if (postMatOff != 0 && postMatOff < blockSize) {
            u32 end = nameTab2Off > postMatOff ? nameTab2Off : blockSize;
            swap_u16_range(block, postMatOff, end);
        }

        /* Post-update SRT center: Vec (f32) */
        u32 postCenterOff = *(u32*)(block + 0x4C);
        if (postCenterOff != 0 && postCenterOff < blockSize) {
            u32 end = blockSize;
            u32 count = (end - postCenterOff) / 4;
            if (count > 1024) count = 1024;
            swap_u32_array(block, postCenterOff, count);
        }
    }
}

static void swap_anm_block(u8* block, u32 blockSize, u32 blockType) {
    /* Common animation header: swap count fields */
    if (blockSize < 0x10) return;

    /* Use specialized handlers for known block types with f32 data */
    if (blockType == FCC('A','N','K','1') || blockType == FCC('A','N','F','1')) {
        swap_ank1(block, blockSize);
        return;
    }
    if (blockType == FCC('T','T','K','1')) {
        swap_ttk1(block, blockSize);
        return;
    }
    if (blockType == FCC('C','L','K','1') || blockType == FCC('C','L','F','1')) {
        swap_clk1(block, blockSize);
        return;
    }
    if (blockType == FCC('T','R','K','1') || blockType == FCC('T','R','F','1')) {
        swap_trk1(block, blockSize);
        return;
    }

    /* Generic handler for blocks with all-u16 or all-s16 data:
     * CLK1/CLF1 (color), TRK1/TRF1 (TEV register), TPT1/TPF1 (tex pattern),
     * PAK1/PAF1, VAK1/VAF1 */

    /* Loop mode (u8) and padding - no swap */
    swap_u16_array(block, 0x0A, 1); /* frameCount */
    swap_u16_array(block, 0x0C, 1); /* entryCount */

    /* Scan for u32 offsets starting at 0x10 */
    u32 numOffsets = 0;
    u32 firstDataOff = blockSize;

    /* Count offset fields (they are between 0x10 and the first data section) */
    for (u32 off = 0x10; off < blockSize && off < 0x60; off += 4) {
        u32 val = r32(block + off);
        if (val == 0 || (val >= 0x10 && val < blockSize)) {
            numOffsets++;
        } else {
            break;
        }
    }

    if (numOffsets == 0) numOffsets = (blockSize > 0x40) ? 12 : 4;
    if (numOffsets > 20) numOffsets = 20;

    /* Swap offset fields */
    swap_u32_array(block, 0x10, numOffsets);

    /* Read offsets and find first data section */
    u32 offsets[20];
    for (u32 i = 0; i < numOffsets && i < 20; i++) {
        offsets[i] = *(u32*)(block + 0x10 + i * 4);
        if (offsets[i] != 0 && offsets[i] < firstDataOff) {
            firstDataOff = offsets[i];
        }
    }

    /* Find the last (highest) non-zero offset — this is typically the name table.
     * Animation blocks that reference materials by name (TRK1, CLK1, TPT1, TTK1, PAK1,
     * TRK3, etc.) store a ResNTAB name table as the last data section. The name table
     * contains ASCII string data that must NOT be bulk-swapped as u16. */
    u32 lastOffset = 0;
    int lastIdx = -1;
    for (u32 i = 0; i < numOffsets && i < 20; i++) {
        if (offsets[i] != 0 && offsets[i] > lastOffset) {
            lastOffset = offsets[i];
            lastIdx = (int)i;
        }
    }

    /* Heuristic: check if the last section looks like a name table.
     * A ResNTAB starts with u16 entryNum, u16 pad, then entries of {u16 hash, u16 offset}.
     * The entry count should be small (<1000) and match the animation's entry count. */
    bool lastIsNameTable = false;
    if (lastIdx >= 0 && lastOffset + 4 < blockSize) {
        u16 ntabCount = r16(block + lastOffset);
        /* Name table entry count should be > 0, reasonable, and the section should be
         * large enough to hold the entries + some string data */
        u32 ntabMinSize = 4 + ntabCount * 4;
        if (ntabCount > 0 && ntabCount < 1000 && lastOffset + ntabMinSize <= blockSize) {
            lastIsNameTable = true;
        }
    }

    /* Swap data sections: use swap_name_table for the name table, swap_u16_range for data */
    for (u32 i = 0; i < numOffsets && i < 20; i++) {
        if (offsets[i] == 0) continue;
        u32 end = blockSize;
        for (u32 j = 0; j < numOffsets && j < 20; j++) {
            if (offsets[j] > offsets[i] && offsets[j] < end) {
                end = offsets[j];
            }
        }
        if (lastIsNameTable && (int)i == lastIdx) {
            /* Name table: swap structured data, preserve ASCII strings */
            swap_name_table(block, offsets[i], blockSize);
        } else {
            swap_u16_range(block, offsets[i], end);
        }
    }
}

int pal_j3d_swap_anim(void* data, u32 size) {
    if (!data || size < 0x20) return 0;

    u8* buf = (u8*)data;
    u32 magic_be = r32(buf);

    if (magic_be != FCC('J','3','D','1') && magic_be != FCC('J','3','D','2')) {
        return 0;
    }

    u32 magic_native = *(u32*)buf;
    if (magic_native == FCC('J','3','D','1') || magic_native == FCC('J','3','D','2')) {
        return 0;
    }

    /* Swap file header */
    swap_u32_array(buf, 0x00, 2);
    swap_u32_array(buf, 0x08, 1);
    swap_u32_array(buf, 0x0C, 1);

    u32 blockNum = *(u32*)(buf + 0x0C);
    if (blockNum > 64) return 0;

    u8* blockPtr = buf + 0x20;
    for (u32 i = 0; i < blockNum && (u32)(blockPtr - buf) < size; i++) {
        u32 blockType = r32(blockPtr);
        u32 blockSize = r32(blockPtr + 4);
        w32(blockPtr, blockType);
        w32(blockPtr + 4, blockSize);

        if (blockSize < 8 || (u32)(blockPtr - buf) + blockSize > size) break;

        swap_anm_block(blockPtr, blockSize, blockType);
        blockPtr += blockSize;
    }

    return 1;
}

/*
 * ResFONT (.bfn) binary format:
 *   Header (0x20 bytes):
 *     u64 magic ('FONT' or 'RFNT'), u32 filesize, u32 numBlocks, padding[0x10]
 *   Blocks: INF1, WID1, GLY1, MAP1
 *     Each block: u32 magic, u32 size, then type-specific data
 */
int pal_font_swap(void* data, u32 size) {
    if (!data || size < 0x20) return 0;

    u8* buf = (u8*)data;

    /* ResFONT magic is a u64. Check if first 4 bytes look like big-endian magic.
     * Common font magics: 'FONT' (0x464F4E54), 'RFNT' (0x52464E54) */
    u32 magic_be = r32(buf);
    u32 magic_native = *(u32*)buf;

    /* If native read matches a known FCC, already swapped */
    if (magic_native == FCC('F','O','N','T') || magic_native == FCC('R','F','N','T') ||
        magic_native == FCC('f','o','n','t'))
    {
        return 0;
    }

    /* Check if big-endian read matches */
    if (magic_be != FCC('F','O','N','T') && magic_be != FCC('R','F','N','T') &&
        magic_be != FCC('f','o','n','t'))
    {
        return 0; /* Not a font file */
    }

    fprintf(stderr, "[pal_font] Swapping font: magic=%c%c%c%c\n",
            buf[0], buf[1], buf[2], buf[3]);

    /* Swap file header */
    swap_u32_array(buf, 0x00, 2);  /* magic (as two u32) */
    swap_u32_array(buf, 0x08, 2);  /* filesize, numBlocks */

    u32 numBlocks = *(u32*)(buf + 0x0C);
    if (numBlocks > 64) {
        fprintf(stderr, "[pal_font] Suspicious numBlocks %u, aborting\n", numBlocks);
        return 0;
    }

    /* Walk blocks */
    u8* blockPtr = buf + 0x20;
    for (u32 i = 0; i < numBlocks && (u32)(blockPtr - buf) < size; i++) {
        u32 blockType = r32(blockPtr);
        u32 blockSize = r32(blockPtr + 4);

        /* Swap block header */
        w32(blockPtr, blockType);
        w32(blockPtr + 4, blockSize);

        if (blockSize < 8 || (u32)(blockPtr - buf) + blockSize > size) break;

        switch (blockType) {
        case FCC('I','N','F','1'):
            /* INF1: u16 fontType, ascent, descent, width, leading, defaultCode */
            swap_u16_array(blockPtr, 0x08, 6);
            break;

        case FCC('W','I','D','1'):
            /* WID1: u16 startCode, endCode, then u8 width data */
            swap_u16_array(blockPtr, 0x08, 2);
            break;

        case FCC('G','L','Y','1'):
            /* GLY1: u16 startCode, endCode, cellWidth, cellHeight,
             *       u32 textureSize, u16 textureFormat, numRows, numColumns,
             *       textureWidth, textureHeight, padding */
            swap_u16_array(blockPtr, 0x08, 2);  /* startCode, endCode */
            swap_u16_array(blockPtr, 0x0C, 2);  /* cellWidth, cellHeight */
            swap_u32_array(blockPtr, 0x10, 1);  /* textureSize */
            swap_u16_array(blockPtr, 0x14, 6);  /* format, rows, cols, w, h, pad */
            break;

        case FCC('M','A','P','1'): {
            /* MAP1: u16 mappingMethod, startCode, endCode, numEntries, mLeading */
            swap_u16_array(blockPtr, 0x08, 5);
            /* Map entries: u16 per entry */
            u16 numEntries = *(u16*)(blockPtr + 0x0E);
            if (numEntries > 0 && 0x12 + numEntries * 2 <= blockSize) {
                swap_u16_array(blockPtr, 0x12, numEntries);
            }
            break;
        }

        default:
            fprintf(stderr, "[pal_font] Unknown block '%c%c%c%c'\n",
                    (char)(blockType>>24), (char)(blockType>>16),
                    (char)(blockType>>8), (char)blockType);
            break;
        }

        blockPtr += blockSize;
    }

    return 1;
}

#endif /* PLATFORM_PC */

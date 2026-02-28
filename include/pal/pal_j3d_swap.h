/**
 * @file pal_j3d_swap.h
 * @brief J3D binary format endian conversion for little-endian PC.
 *
 * J3D files (.bmd, .bdl, .bck, .bpk, .brk, .btk, etc.) are big-endian
 * binary from the GameCube/Wii disc. On PC (little-endian), the binary
 * must be byte-swapped in-place before the J3D loader can parse it.
 *
 * This module handles:
 * - File header (magic, block count, etc.)
 * - Block headers (type, size) for INF1, VTX1, EVP1, DRW1, JNT1, SHP1, MAT3, TEX1
 * - Block-internal offset fields (u32 offsets that become pointers)
 * - Count fields (u16/u32 element counts)
 */
#ifndef PAL_J3D_SWAP_H
#define PAL_J3D_SWAP_H

#include "global.h"
#include "dolphin/types.h"

#if PLATFORM_PC

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Byte-swap a J3D binary file (bmd/bdl) from big-endian to little-endian in-place.
 * Must be called before J3DModelLoaderDataBase::load().
 * Returns 1 if the data was swapped, 0 if already little-endian or not J3D.
 */
int pal_j3d_swap_model(void* data, u32 size);

/**
 * Byte-swap a J3D animation file (bck/bpk/brk/btk/btp/bva/etc.)
 * from big-endian to little-endian in-place.
 * Must be called before J3DAnmLoaderDataBase::load().
 * Returns 1 if the data was swapped, 0 if already little-endian or not J3D.
 */
int pal_j3d_swap_anim(void* data, u32 size);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC */
#endif /* PAL_J3D_SWAP_H */

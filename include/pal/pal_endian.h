/**
 * @file pal_endian.h
 * @brief Byte-swap utilities for loading big-endian Wii data on little-endian PC.
 *
 * All game data from the disc is big-endian (PowerPC byte order).
 * On PC (little-endian), binary structures must be byte-swapped after loading.
 */
#ifndef PAL_ENDIAN_H
#define PAL_ENDIAN_H

#include "dolphin/types.h"

#if PLATFORM_PC

#ifdef __cplusplus
extern "C" {
#endif

static inline u16 pal_swap16(u16 v) {
    return (u16)((v >> 8) | (v << 8));
}

static inline u32 pal_swap32(u32 v) {
    return ((v >> 24) & 0xFF)
         | ((v >>  8) & 0xFF00)
         | ((v <<  8) & 0xFF0000)
         | ((v << 24) & 0xFF000000u);
}

static inline s32 pal_swap32s(s32 v) {
    return (s32)pal_swap32((u32)v);
}

/**
 * Swap the header of a RARC archive in-place.
 * Call after loading from disc, before parsing.
 */
void pal_swap_rarc(void* arcData, u32 loadedSize);

/**
 * Get the separately-allocated repacked file entries from the last pal_swap_rarc call.
 * Returns NULL if no repacking was needed (32-bit build or no files).
 * The caller should use this pointer for mFiles instead of the in-buffer one.
 */
void* pal_swap_rarc_get_repacked_files(void);

/**
 * Swap a Yaz0-compressed header in-place (just the 16-byte header).
 */
void pal_swap_yaz0_header(void* data);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC */
#endif /* PAL_ENDIAN_H */

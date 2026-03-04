/**
 * pal_save.h - File-based save/load for PC port
 *
 * Replaces Wii NAND filesystem with host filesystem save files.
 * Save data is stored in a configurable directory (default: "save/").
 *
 * NAND paths like "/title/00010000/52534445/data/tp.dat" are mapped
 * to host paths like "save/tp.dat" (flattened to basename).
 */

#ifndef PAL_SAVE_H
#define PAL_SAVE_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize save system. Creates save directory if needed.
 * Returns 1 on success, 0 on failure.
 */
int pal_save_init(void);

/**
 * Open a save file by NAND path. Returns a handle >= 0, or -1 on error.
 * accType: 1 = read, 2 = write, 3 = read/write
 */
int pal_save_open(const char* nand_path, u8 accType);

/**
 * Close a save file handle.
 */
int pal_save_close(int handle);

/**
 * Read from a save file. Returns bytes read, or -1 on error.
 */
int pal_save_read(int handle, void* buf, u32 length);

/**
 * Write to a save file. Returns bytes written, or -1 on error.
 */
int pal_save_write(int handle, const void* buf, u32 length);

/**
 * Seek in a save file. Returns new position, or -1 on error.
 */
int pal_save_seek(int handle, s32 offset, int whence);

/**
 * Get the length of a save file. Returns 0 on success, -1 on error.
 */
int pal_save_get_length(int handle, u32* out_length);

/**
 * Create a new save file by NAND path. Returns 0 on success, -1 on error.
 */
int pal_save_create(const char* nand_path);

/**
 * Delete a save file by NAND path. Returns 0 on success, -1 on error.
 */
int pal_save_delete(const char* nand_path);

/**
 * Get save directory path.
 */
const char* pal_save_get_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_SAVE_H */

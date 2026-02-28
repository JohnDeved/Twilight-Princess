/**
 * gx_displaylist.h - GX Display List replay for PC port (Step 5e)
 *
 * Parses and replays pre-recorded GX display lists from J3D model data.
 * Display lists contain raw GX FIFO command bytes in big-endian format.
 */

#ifndef PAL_GX_DISPLAYLIST_H
#define PAL_GX_DISPLAYLIST_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Replay a GX display list by parsing the raw FIFO byte stream.
 * This is called from GXCallDisplayList on PC instead of sending
 * bytes to hardware.
 *
 * @param list   Pointer to display list data (big-endian GX FIFO commands)
 * @param nbytes Size of display list in bytes
 */
void pal_gx_call_display_list(const void* list, u32 nbytes);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GX_DISPLAYLIST_H */

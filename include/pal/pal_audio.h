/**
 * pal_audio.h - Phase A audio for PC port
 *
 * Provides SDL3 audio output with silence (Phase A).
 * Phase B will add software mixing from J-Audio engine.
 *
 * Phase A design:
 *   - Opens an SDL3 audio stream
 *   - Outputs silence so the audio subsystem is active
 *   - Reports status to verification system
 *   - Provides framework for Phase B mixing
 */

#ifndef PAL_AUDIO_H
#define PAL_AUDIO_H

#include "dolphin/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SDL3 audio subsystem and open an audio stream.
 * Returns 1 on success, 0 on failure.
 * In headless mode, succeeds without opening audio.
 */
int pal_audio_init(void);

/**
 * Mix audio for one frame. Call once per game frame.
 * In Phase A, this is a no-op (silence output).
 * In Phase B, this will drive the software mixer.
 */
void pal_audio_mix_frame(void);

/**
 * Get audio status for verification.
 * Returns number of samples mixed since init.
 */
u32 pal_audio_get_samples_mixed(void);

/**
 * Check if audio has produced any non-silent output.
 * Returns 0 in Phase A (always silent).
 */
int pal_audio_has_nonsilence(void);

/**
 * Shut down SDL3 audio subsystem.
 */
void pal_audio_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_AUDIO_H */

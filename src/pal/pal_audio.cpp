/**
 * pal_audio.cpp - Phase A audio for PC port
 *
 * Opens an SDL3 audio stream and outputs silence. This ensures the
 * audio subsystem is active and the game loop doesn't hang waiting
 * for audio callbacks that will never fire.
 *
 * Phase A: Silence output only. The J-Audio engine is not initialized
 * on PC because the BAA/BMS data is big-endian and can't be parsed yet.
 *
 * Phase B (future): Software mixer reads decoded audio samples and
 * feeds them through this SDL3 audio stream.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_audio.h>

#include "pal/pal_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Audio stream state */
static SDL_AudioStream* s_audio_stream = NULL;
static int s_audio_initialized = 0;
static int s_audio_headless = 0;
static u32 s_samples_mixed = 0;

/* Audio format: 32kHz stereo 16-bit (matches GC/Wii DSP output) */
#define PAL_AUDIO_FREQ     32000
#define PAL_AUDIO_CHANNELS 2
#define PAL_AUDIO_SAMPLES  512  /* ~16ms buffer at 32kHz */

int pal_audio_init(void) {
    if (s_audio_initialized) return 1;

    const char* headless_env = getenv("TP_HEADLESS");
    s_audio_headless = (headless_env && headless_env[0] == '1');

    if (s_audio_headless) {
        fprintf(stderr, "{\"pal_audio\":\"headless\",\"skip\":true}\n");
        s_audio_initialized = 1;
        return 1;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "{\"pal_audio\":\"sdl_init_failed\",\"error\":\"%s\"}\n", SDL_GetError());
        /* Non-fatal: audio failure shouldn't prevent the game from running */
        s_audio_initialized = 1;
        return 1;
    }

    /* Create audio stream: app format → device format (SDL handles conversion) */
    SDL_AudioSpec spec;
    spec.freq = PAL_AUDIO_FREQ;
    spec.format = SDL_AUDIO_S16;
    spec.channels = PAL_AUDIO_CHANNELS;

    s_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec,
        NULL,  /* No callback — we push silence manually */
        NULL
    );

    if (!s_audio_stream) {
        fprintf(stderr, "{\"pal_audio\":\"stream_failed\",\"error\":\"%s\"}\n", SDL_GetError());
        s_audio_initialized = 1;
        return 1; /* Non-fatal */
    }

    /* Start playback */
    SDL_ResumeAudioStreamDevice(s_audio_stream);

    s_audio_initialized = 1;
    fprintf(stderr, "{\"pal_audio\":\"ready\",\"freq\":%d,\"channels\":%d}\n",
            PAL_AUDIO_FREQ, PAL_AUDIO_CHANNELS);
    return 1;
}

void pal_audio_mix_frame(void) {
    if (!s_audio_initialized || s_audio_headless || !s_audio_stream) return;

    /* Phase A: Push silence buffer.
     * This keeps SDL3 audio stream fed so it doesn't underrun.
     * Phase B will replace this with actual mixed audio data. */
    s16 silence[PAL_AUDIO_SAMPLES * PAL_AUDIO_CHANNELS];
    memset(silence, 0, sizeof(silence));

    SDL_PutAudioStreamData(s_audio_stream, silence, sizeof(silence));
    s_samples_mixed += PAL_AUDIO_SAMPLES;
}

u32 pal_audio_get_samples_mixed(void) {
    return s_samples_mixed;
}

int pal_audio_has_nonsilence(void) {
    /* Phase A: always silent */
    return 0;
}

void pal_audio_shutdown(void) {
    if (s_audio_stream) {
        SDL_DestroyAudioStream(s_audio_stream);
        s_audio_stream = NULL;
    }
    s_audio_initialized = 0;
    s_samples_mixed = 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

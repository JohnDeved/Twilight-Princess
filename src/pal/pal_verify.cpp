/**
 * pal_verify.cpp - Subsystem verification for automated testing
 *
 * Provides structured JSON logging for rendering, input, and audio
 * verification. Activated by TP_VERIFY=1 environment variable.
 *
 * Frame captures are saved to TP_VERIFY_DIR (default: "verify_output").
 * Capture frames configured by TP_VERIFY_CAPTURE_FRAMES (e.g. "30,60,120").
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

extern "C" {
#include "pal/pal_verify.h"
#include "pal/gx/gx_screenshot.h"
}

/* ================================================================ */
/* Internal state                                                   */
/* ================================================================ */

#define VERIFY_MAX_CAPTURES 32

static int s_verify_active = 0;
static const char* s_verify_dir = NULL;

/* Frame capture schedule */
static u32 s_capture_frames[VERIFY_MAX_CAPTURES];
static int s_num_capture_frames = 0;

/* Running statistics */
static u32 s_total_frames = 0;
static u32 s_total_draw_calls = 0;
static u32 s_frames_with_draws = 0;
static u32 s_frames_nonblack = 0;
static u32 s_total_input_events = 0;
static u32 s_total_input_responses = 0;
static u32 s_audio_frames_active = 0;
static u32 s_audio_frames_nonsilent = 0;
static int s_peak_draw_calls = 0;
static int s_peak_verts = 0;

static void parse_capture_frames(const char* spec) {
    if (!spec || spec[0] == '\0')
        return;

    /* Parse comma-separated frame numbers */
    const char* p = spec;
    while (*p && s_num_capture_frames < VERIFY_MAX_CAPTURES) {
        char* end;
        long val = strtol(p, &end, 10);
        if (end != p && val > 0) {
            s_capture_frames[s_num_capture_frames++] = (u32)val;
        }
        if (*end == ',')
            end++;
        else if (*end != '\0')
            break;
        p = end;
    }
}

static int should_capture(u32 frame_num) {
    int i;
    for (i = 0; i < s_num_capture_frames; i++) {
        if (s_capture_frames[i] == frame_num)
            return 1;
    }
    return 0;
}

/* ================================================================ */
/* Initialization                                                   */
/* ================================================================ */

extern "C" {

void pal_verify_init(void) {
    const char* env = getenv("TP_VERIFY");
    if (!env || env[0] != '1') {
        s_verify_active = 0;
        return;
    }

    s_verify_active = 1;

    s_verify_dir = getenv("TP_VERIFY_DIR");
    if (!s_verify_dir || s_verify_dir[0] == '\0')
        s_verify_dir = "verify_output";

    /* Create output directory (ignore EEXIST) */
    if (mkdir(s_verify_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "{\"verify\":\"warning\",\"msg\":\"mkdir failed: %s\"}\n", s_verify_dir);
    }

    /* Parse capture frame schedule */
    const char* capture_spec = getenv("TP_VERIFY_CAPTURE_FRAMES");
    if (capture_spec) {
        parse_capture_frames(capture_spec);
    } else {
        /* Default: capture at frames 30, 60, 120, 300, 600, 1200, 1800 */
        static const u32 defaults[] = {30, 60, 120, 300, 600, 1200, 1800};
        s_num_capture_frames = 7;
        memcpy(s_capture_frames, defaults, sizeof(defaults));
    }

    fprintf(stdout, "{\"verify\":\"init\",\"dir\":\"%s\",\"capture_frames\":%d}\n",
            s_verify_dir, s_num_capture_frames);
    fflush(stdout);
}

int pal_verify_active(void) {
    return s_verify_active;
}

/* ================================================================ */
/* Rendering verification                                           */
/* ================================================================ */

void pal_verify_frame(u32 frame_num, u32 draw_calls, u32 total_verts,
                      u32 stub_count, u32 valid) {
    if (!s_verify_active)
        return;

    s_total_frames++;
    s_total_draw_calls += draw_calls;
    if (draw_calls > 0)
        s_frames_with_draws++;
    if ((int)draw_calls > s_peak_draw_calls)
        s_peak_draw_calls = (int)draw_calls;
    if ((int)total_verts > s_peak_verts)
        s_peak_verts = (int)total_verts;

    /* Emit detailed frame report every 60 frames, or on capture frames */
    if (frame_num % 60 == 0 || should_capture(frame_num) || frame_num <= 5) {
        /* Include framebuffer hash for deterministic rendering comparison */
        uint32_t fb_hash = pal_screenshot_hash_fb();
        int has_draws = pal_screenshot_has_draws();
        fprintf(stdout,
            "{\"verify_frame\":{\"frame\":%u,\"draw_calls\":%u,\"verts\":%u,"
            "\"stub_count\":%u,\"valid\":%u,\"fb_hash\":\"0x%08X\","
            "\"fb_has_draws\":%d}}\n",
            frame_num, draw_calls, total_verts, stub_count, valid,
            fb_hash, has_draws);
        fflush(stdout);
    }

    /* Capture frame if scheduled */
    if (should_capture(frame_num)) {
        pal_verify_capture_frame(frame_num);
        pal_verify_analyze_fb(frame_num);
    }
}

void pal_verify_capture_frame(u32 frame_num) {
    if (!s_verify_active || !s_verify_dir)
        return;

    /* Use the software framebuffer from gx_screenshot.cpp */
    uint8_t* fb = pal_screenshot_get_fb();
    if (!fb)
        return;

    int fb_w = pal_screenshot_get_fb_width();
    int fb_h = pal_screenshot_get_fb_height();

    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%04u.bmp", s_verify_dir, frame_num);

    /* Write BMP file (same format as gx_screenshot.cpp) */
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stdout, "{\"verify_capture\":\"write_failed\",\"frame\":%u,\"path\":\"%s\"}\n",
                frame_num, path);
        fflush(stdout);
        return;
    }

    uint32_t row_bytes = (uint32_t)(fb_w * 3);
    uint32_t row_padding = (4 - (row_bytes % 4)) % 4;
    uint32_t pixel_data_size = (row_bytes + row_padding) * (uint32_t)fb_h;
    uint32_t file_size = 54 + pixel_data_size;

    uint8_t bmp_header[54];
    memset(bmp_header, 0, 54);
    bmp_header[0] = 'B'; bmp_header[1] = 'M';
    bmp_header[2] = (uint8_t)(file_size & 0xFF);
    bmp_header[3] = (uint8_t)((file_size >> 8) & 0xFF);
    bmp_header[4] = (uint8_t)((file_size >> 16) & 0xFF);
    bmp_header[5] = (uint8_t)((file_size >> 24) & 0xFF);
    bmp_header[10] = 54;
    bmp_header[14] = 40;
    bmp_header[18] = (uint8_t)(fb_w & 0xFF);
    bmp_header[19] = (uint8_t)((fb_w >> 8) & 0xFF);
    bmp_header[22] = (uint8_t)(fb_h & 0xFF);
    bmp_header[23] = (uint8_t)((fb_h >> 8) & 0xFF);
    bmp_header[26] = 1;
    bmp_header[28] = 24;
    fwrite(bmp_header, 1, 54, f);

    uint8_t pad[4] = {0, 0, 0, 0};
    int y, x;
    for (y = fb_h - 1; y >= 0; y--) {
        for (x = 0; x < fb_w; x++) {
            int idx = (y * fb_w + x) * 4;
            uint8_t bgr[3];
            bgr[0] = fb[idx + 2];
            bgr[1] = fb[idx + 1];
            bgr[2] = fb[idx + 0];
            fwrite(bgr, 1, 3, f);
        }
        if (row_padding > 0)
            fwrite(pad, 1, row_padding, f);
    }

    fclose(f);
    fprintf(stdout, "{\"verify_capture\":\"saved\",\"frame\":%u,\"path\":\"%s\"}\n",
            frame_num, path);
    fflush(stdout);
}

int pal_verify_analyze_fb(u32 frame_num) {
    if (!s_verify_active)
        return 0;

    uint8_t* fb = pal_screenshot_get_fb();
    if (!fb)
        return 0;

    int fb_w = pal_screenshot_get_fb_width();
    int fb_h = pal_screenshot_get_fb_height();
    u32 total_pixels = (u32)(fb_w * fb_h);
    u32 nonblack = 0;
    u32 unique_colors = 0;
    u64 avg_r = 0, avg_g = 0, avg_b = 0;

    u32 color_set[256];
    memset(color_set, 0, sizeof(color_set));
    int i;

    for (i = 0; i < (int)total_pixels; i++) {
        int idx = i * 4;
        uint8_t r = fb[idx + 0];
        uint8_t g = fb[idx + 1];
        uint8_t b = fb[idx + 2];

        if (r > 2 || g > 2 || b > 2)
            nonblack++;

        avg_r += r;
        avg_g += g;
        avg_b += b;

        /* Track unique colors via 3-bit R, 3-bit G, 2-bit B quantization
         * (256 buckets) — samples all pixels for accurate diversity count */
        {
            uint8_t hash = (uint8_t)(((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6));
            color_set[hash] = 1;
        }
    }

    for (i = 0; i < 256; i++) {
        if (color_set[i])
            unique_colors++;
    }

    int pct_nonblack = (int)((nonblack * 100) / total_pixels);
    avg_r /= total_pixels;
    avg_g /= total_pixels;
    avg_b /= total_pixels;

    if (pct_nonblack > 0)
        s_frames_nonblack++;

    uint32_t fb_hash = pal_screenshot_hash_fb();

    fprintf(stdout,
        "{\"verify_fb\":{\"frame\":%u,\"pct_nonblack\":%d,\"unique_colors\":%u,"
        "\"avg_color\":[%u,%u,%u],\"total_pixels\":%u,\"fb_hash\":\"0x%08X\"}}\n",
        frame_num, pct_nonblack, unique_colors,
        (u32)avg_r, (u32)avg_g, (u32)avg_b, total_pixels, fb_hash);
    fflush(stdout);

    return pct_nonblack;
}

/* ================================================================ */
/* Input verification                                               */
/* ================================================================ */

void pal_verify_input(u32 frame_num, u16 buttons, s8 stick_x, s8 stick_y) {
    if (!s_verify_active)
        return;

    s_total_input_events++;
    fprintf(stdout,
        "{\"verify_input\":{\"frame\":%u,\"buttons\":\"0x%04X\","
        "\"stick_x\":%d,\"stick_y\":%d}}\n",
        frame_num, buttons, stick_x, stick_y);
    fflush(stdout);
}

void pal_verify_input_response(u32 frame_num, const char* event,
                               const char* detail) {
    if (!s_verify_active)
        return;

    s_total_input_responses++;
    fprintf(stdout,
        "{\"verify_input_response\":{\"frame\":%u,\"event\":\"%s\","
        "\"detail\":\"%s\"}}\n",
        frame_num, event ? event : "", detail ? detail : "");
    fflush(stdout);
}

/* ================================================================ */
/* Audio verification                                               */
/* ================================================================ */

void pal_verify_audio(u32 frame_num, int audio_active, u32 samples_mixed,
                      u32 buffers_queued, int has_nonsilence) {
    if (!s_verify_active)
        return;

    if (audio_active)
        s_audio_frames_active++;
    if (has_nonsilence)
        s_audio_frames_nonsilent++;

    /* Log audio status every 60 frames */
    if (frame_num % 60 == 0 || frame_num <= 5) {
        fprintf(stdout,
            "{\"verify_audio\":{\"frame\":%u,\"active\":%d,"
            "\"samples_mixed\":%u,\"buffers_queued\":%u,"
            "\"has_nonsilence\":%d}}\n",
            frame_num, audio_active, samples_mixed,
            buffers_queued, has_nonsilence);
        fflush(stdout);
    }
}

int pal_verify_audio_check_silence(const s16* samples, u32 num_samples,
                                   s16 threshold) {
    u32 i;
    if (!samples || num_samples == 0)
        return 0;

    for (i = 0; i < num_samples; i++) {
        if (samples[i] > threshold || samples[i] < -threshold)
            return 1;  /* non-silent */
    }
    return 0;  /* silent */
}

/* ================================================================ */
/* Summary                                                          */
/* ================================================================ */

void pal_verify_summary(void) {
    if (!s_verify_active)
        return;

    int render_health = 0;
    int input_health = 0;
    int audio_health = 0;

    /* Rendering health: percentage of frames that had draws AND non-black pixels */
    if (s_total_frames > 0) {
        u32 good_render = (s_frames_with_draws < s_frames_nonblack) ?
                           s_frames_with_draws : s_frames_nonblack;
        render_health = (int)((good_render * 100) / s_total_frames);
    }

    /* Input health: were input events processed and did game respond? */
    if (s_total_input_events > 0) {
        input_health = (s_total_input_responses > 0) ? 100 : 50;
    }

    /* Audio health: were audio buffers non-silent? */
    if (s_audio_frames_active > 0) {
        audio_health = (int)((s_audio_frames_nonsilent * 100) / s_audio_frames_active);
    }

    fprintf(stdout,
        "{\"verify_summary\":{"
        "\"total_frames\":%u,"
        "\"frames_with_draws\":%u,"
        "\"frames_nonblack\":%u,"
        "\"peak_draw_calls\":%d,"
        "\"peak_verts\":%d,"
        "\"total_draw_calls\":%u,"
        "\"input_events\":%u,"
        "\"input_responses\":%u,"
        "\"audio_frames_active\":%u,"
        "\"audio_frames_nonsilent\":%u,"
        "\"render_health_pct\":%d,"
        "\"input_health_pct\":%d,"
        "\"audio_health_pct\":%d"
        "}}\n",
        s_total_frames, s_frames_with_draws, s_frames_nonblack,
        s_peak_draw_calls, s_peak_verts, s_total_draw_calls,
        s_total_input_events, s_total_input_responses,
        s_audio_frames_active, s_audio_frames_nonsilent,
        render_health, input_health, audio_health);
    fflush(stdout);
}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

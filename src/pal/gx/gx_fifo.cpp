/**
 * gx_fifo.cpp
 * GX FIFO write stubs -- captures all GX FIFO writes to a RAM buffer.
 * This is the software equivalent of Shield's hardware FIFO intercept.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <string.h>

/* FIFO buffer -- 1 MB ring buffer for GX command capture */
#define GX_FIFO_SIZE (1024 * 1024)

static unsigned char gx_fifo_buf[GX_FIFO_SIZE];
static unsigned int gx_fifo_pos = 0;

#ifdef __cplusplus
extern "C" {
#endif

void gx_fifo_write_u8(unsigned char ub) {
    if (gx_fifo_pos + 1 <= GX_FIFO_SIZE) {
        gx_fifo_buf[gx_fifo_pos++] = ub;
    }
}

void gx_fifo_write_u16(unsigned short us) {
    if (gx_fifo_pos + 2 <= GX_FIFO_SIZE) {
        gx_fifo_buf[gx_fifo_pos++] = (unsigned char)(us >> 8);
        gx_fifo_buf[gx_fifo_pos++] = (unsigned char)(us & 0xFF);
    }
}

void gx_fifo_write_u32(unsigned int ui) {
    if (gx_fifo_pos + 4 <= GX_FIFO_SIZE) {
        gx_fifo_buf[gx_fifo_pos++] = (unsigned char)(ui >> 24);
        gx_fifo_buf[gx_fifo_pos++] = (unsigned char)((ui >> 16) & 0xFF);
        gx_fifo_buf[gx_fifo_pos++] = (unsigned char)((ui >> 8) & 0xFF);
        gx_fifo_buf[gx_fifo_pos++] = (unsigned char)(ui & 0xFF);
    }
}

void gx_fifo_write_f32(float f) {
    unsigned int ui;
    memcpy(&ui, &f, sizeof(ui));
    gx_fifo_write_u32(ui);
}

void gx_fifo_reset(void) {
    gx_fifo_pos = 0;
}

unsigned int gx_fifo_get_pos(void) {
    return gx_fifo_pos;
}

const unsigned char* gx_fifo_get_buf(void) {
    return gx_fifo_buf;
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

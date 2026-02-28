/**
 * pal_save.cpp - File-based save/load for PC port
 *
 * Replaces Wii NAND filesystem with host filesystem I/O.
 * Save files are stored in a configurable directory (TP_SAVE_DIR env var,
 * defaults to "save/").
 *
 * NAND paths are mapped to host filesystem by extracting the basename:
 *   "/title/00010000/52534445/data/tp.dat" → "save/tp.dat"
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#include "pal/pal_save.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ */
/* Configuration                                                    */
/* ================================================================ */

#define PAL_SAVE_MAX_OPEN 16
#define PAL_SAVE_PATH_MAX 512

static FILE* s_save_files[PAL_SAVE_MAX_OPEN];
static char  s_save_dir[PAL_SAVE_PATH_MAX] = "save";
static int   s_save_initialized = 0;

/* ================================================================ */
/* Path mapping: NAND path → host path                              */
/* ================================================================ */

/**
 * Extract basename from NAND path and build host save path.
 * "/title/00010000/52534445/data/tp.dat" → "save/tp.dat"
 */
static void save_build_path(char* out, int out_size, const char* nand_path) {
    /* Find last '/' to get basename */
    const char* basename = nand_path;
    const char* p = nand_path;
    while (*p) {
        if (*p == '/') basename = p + 1;
        p++;
    }

    /* If basename is empty, use the whole path */
    if (*basename == '\0') basename = nand_path;

    snprintf(out, out_size, "%s/%s", s_save_dir, basename);
}

/* ================================================================ */
/* Public API                                                       */
/* ================================================================ */

int pal_save_init(void) {
    if (s_save_initialized) return 1;

    /* Check for custom save directory */
    const char* save_dir = getenv("TP_SAVE_DIR");
    if (save_dir && save_dir[0]) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    }

    /* Create save directory if it doesn't exist */
#ifdef _WIN32
    _mkdir(s_save_dir);
#else
    mkdir(s_save_dir, 0755);
#endif

    memset(s_save_files, 0, sizeof(s_save_files));
    s_save_initialized = 1;

    fprintf(stderr, "{\"pal_save\":\"ready\",\"dir\":\"%s\"}\n", s_save_dir);
    return 1;
}

int pal_save_open(const char* nand_path, u8 accType) {
    if (!s_save_initialized) pal_save_init();
    if (!nand_path) return -1;

    /* Find free handle */
    int handle = -1;
    for (int i = 0; i < PAL_SAVE_MAX_OPEN; i++) {
        if (!s_save_files[i]) {
            handle = i;
            break;
        }
    }
    if (handle < 0) return -1;

    /* Build host path */
    char host_path[PAL_SAVE_PATH_MAX];
    save_build_path(host_path, sizeof(host_path), nand_path);

    /* Determine file mode */
    const char* mode;
    switch (accType) {
    case 1:  mode = "rb"; break;          /* Read only */
    case 2:  mode = "wb"; break;          /* Write only */
    case 3:  mode = "r+b"; break;         /* Read/write */
    default: mode = "r+b"; break;
    }

    FILE* fp = fopen(host_path, mode);
    if (!fp && accType == 3) {
        /* Read/write mode fails if file doesn't exist; create it */
        fp = fopen(host_path, "w+b");
    }
    if (!fp) return -1;

    s_save_files[handle] = fp;
    return handle;
}

int pal_save_close(int handle) {
    if (handle < 0 || handle >= PAL_SAVE_MAX_OPEN) return -1;
    if (!s_save_files[handle]) return -1;

    fclose(s_save_files[handle]);
    s_save_files[handle] = NULL;
    return 0;
}

int pal_save_read(int handle, void* buf, u32 length) {
    if (handle < 0 || handle >= PAL_SAVE_MAX_OPEN) return -1;
    if (!s_save_files[handle] || !buf) return -1;

    size_t read = fread(buf, 1, length, s_save_files[handle]);
    return (int)read;
}

int pal_save_write(int handle, const void* buf, u32 length) {
    if (handle < 0 || handle >= PAL_SAVE_MAX_OPEN) return -1;
    if (!s_save_files[handle] || !buf) return -1;

    size_t written = fwrite(buf, 1, length, s_save_files[handle]);
    fflush(s_save_files[handle]);
    return (int)written;
}

int pal_save_seek(int handle, s32 offset, int whence) {
    if (handle < 0 || handle >= PAL_SAVE_MAX_OPEN) return -1;
    if (!s_save_files[handle]) return -1;

    if (fseek(s_save_files[handle], offset, whence) != 0) return -1;
    return (int)ftell(s_save_files[handle]);
}

int pal_save_get_length(int handle, u32* out_length) {
    if (handle < 0 || handle >= PAL_SAVE_MAX_OPEN) return -1;
    if (!s_save_files[handle]) return -1;

    long cur = ftell(s_save_files[handle]);
    fseek(s_save_files[handle], 0, SEEK_END);
    long len = ftell(s_save_files[handle]);
    fseek(s_save_files[handle], cur, SEEK_SET);

    if (out_length) *out_length = (u32)len;
    return 0;
}

int pal_save_create(const char* nand_path) {
    if (!nand_path) return -1;
    if (!s_save_initialized) pal_save_init();

    char host_path[PAL_SAVE_PATH_MAX];
    save_build_path(host_path, sizeof(host_path), nand_path);

    FILE* fp = fopen(host_path, "wb");
    if (!fp) return -1;
    fclose(fp);
    return 0;
}

int pal_save_delete(const char* nand_path) {
    if (!nand_path) return -1;
    if (!s_save_initialized) pal_save_init();

    char host_path[PAL_SAVE_PATH_MAX];
    save_build_path(host_path, sizeof(host_path), nand_path);

    return (remove(host_path) == 0) ? 0 : -1;
}

const char* pal_save_get_dir(void) {
    return s_save_dir;
}

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

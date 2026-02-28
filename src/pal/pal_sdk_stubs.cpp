/**
 * pal_sdk_stubs.cpp
 * Stub implementations for Dolphin/Revolution SDK functions (OS, DVD, VI, PAD,
 * AI, AR, DSP, NAND, SC, WPAD, KPAD, HBM) needed to link the PC port.
 * Functions already stubbed in pal_os_stubs.cpp are NOT duplicated here.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <time.h>

#include "pal/pal_milestone.h"
#include "dolphin/types.h"
#include "revolution/os/OSThread.h"

/* ---------------------------------------------------------------- */
/* Forward declarations for SDK types used in signatures.           */
/* We avoid pulling in full headers to prevent conflicts.           */
/* ---------------------------------------------------------------- */

/* OSThread, OSThreadQueue, OSContext are included from OSThread.h */
struct OSMutex;
struct OSCond;
struct OSMessageQueue;
struct OSAlarm;
struct OSStopwatch;

#include "dolphin/dvd.h" /* DVDDiskID, DVDCommandBlock, DVDFileInfo, etc. */

struct GXRenderModeObj;

struct PADStatus;
struct PADClampRegion;

struct NANDFileInfo;
struct NANDCommandBlock;
struct NANDStatus;

struct DSPTaskInfo;

struct WPADStatus;
struct WPADInfo;

struct KPADStatus;

struct HBMDataInfo;
struct HBMControllerData;

struct ARQRequest;

typedef void (*DVDCBCallback)(s32 result, DVDCommandBlock* block);
typedef void (*DVDOptionalCommandChecker)(DVDCommandBlock* block, void* addr);
typedef void (*OSAlarmHandler)(OSAlarm* alarm, OSContext* context);
typedef s32  OSPriority;
typedef void (*OSIdleFunction)(void* param);
typedef void (*OSSwitchThreadCallback)(OSThread* from, OSThread* to);
typedef void (*OSErrorHandler)(u8 error, OSContext* context, ...);
typedef void (*AIDCallback)(void);
typedef void (*ARQCallback)(u32);
typedef void (*DSPCallback)(void*);
typedef void (*SCFlushCallback)(s32);
typedef s32  (*VIRetraceCallback)(u32 retraceCount);
typedef void (*WPADCallback)(s32 chan, s32 result);
typedef void (*NANDCallback)(s32 result, NANDCommandBlock* block);
typedef void* OSMessage;

extern "C" {

/* Forward declaration */
static void pal_init_main_thread(void);

/* ================================================================ */
/* Fake MEM1 region for OSPhysicalToCached/OSCachedToPhysical       */
/* On GC/Wii, physical 0 maps to cached 0x80000000. On PC, we      */
/* provide a zeroed buffer so code reading low-memory globals works. */
/* ================================================================ */

/* Pre-initialized fake MEM1 region.
 * On GC/Wii, physical address 0 maps to cached 0x80000000.
 * Critical values that are read during C++ static initialization:
 *   +0x00: OSBootInfo (DVDDiskID + magic + version + memorySize)
 *   +0xF8: __OSBusClock (read by OS_TIMER_CLOCK macro)
 *   +0xFC: __OSCoreClock
 * These must be non-zero BEFORE any static constructors run.
 *
 * We use a constructor with the lowest possible priority (101) and
 * also ensure CMakeLists puts pal_sdk_stubs.cpp first in link order.
 * The real solution uses .init_array ordering.
 */
#define PAL_MEM1_SIZE 0x4000
unsigned char pal_fake_mem1[PAL_MEM1_SIZE] __attribute__((aligned(4096)));

/* This must run before ANY other static initializer in the program.
 * By placing it in .init_array with priority 101 (just after glibc's
 * own priority-100 entries), it runs before C++ static initializers
 * which have the default priority 65535. */
static void __attribute__((constructor(101))) pal_early_init(void) {
    /* Wii bus clock: 243 MHz */
    *(u32*)(pal_fake_mem1 + 0xF8) = 243000000u;
    /* Wii core clock: 729 MHz */
    *(u32*)(pal_fake_mem1 + 0xFC) = 729000000u;
    /* OSBootInfo at offset 0x20 (after 32-byte DVDDiskID) */
    *(u32*)(pal_fake_mem1 + 0x20) = 0x0D15EA5Eu; /* magic */
    *(u32*)(pal_fake_mem1 + 0x24) = 1u;          /* version */
    *(u32*)(pal_fake_mem1 + 0x28) = 24u * 1024u * 1024u; /* memorySize */
}

/* ================================================================ */
/* OS Init / System                                                 */
/* ================================================================ */

void OSInit(void) {
    pal_init_main_thread();
    /* pal_fake_mem1 is pre-initialized at compile time with:
     *   OSBootInfo (memorySize=24MB), __OSBusClock=243MHz, __OSCoreClock=729MHz */
}
void OSRegisterVersion(const char* id) { (void)id; }
/* Return GameCube/Wii-compatible memory sizes for SDK compatibility */
u32 OSGetPhysicalMemSize(void) { return 24 * 1024 * 1024; }
u32 OSGetPhysicalMem2Size(void) { return 64 * 1024 * 1024; }

/* ================================================================ */
/* OS Arena / Memory                                                */
/* ================================================================ */

static u8 s_arena_mem[64 * 1024 * 1024]; /* 64 MB arena — headroom for 64-bit pointer overhead + heap structure padding */
static u8* s_arena_lo = s_arena_mem;
static u8* s_arena_hi = s_arena_mem + sizeof(s_arena_mem);

void* OSGetArenaHi(void) { return s_arena_hi; }
void* OSGetArenaLo(void) { return s_arena_lo; }
void  OSSetArenaHi(void* newHi) { s_arena_hi = (u8*)newHi; }
void  OSSetArenaLo(void* newLo) { s_arena_lo = (u8*)newLo; }

void* OSAllocFromArenaLo(u32 size, u32 align) {
    uintptr_t p = (uintptr_t)s_arena_lo;
    if (align > 0) p = (p + align - 1) & ~((uintptr_t)align - 1);
    void* ret = (void*)p;
    s_arena_lo = (u8*)(p + size);
    return ret;
}

void* OSAllocFromArenaHi(u32 size, u32 align) {
    uintptr_t p = (uintptr_t)s_arena_hi;
    p -= size;
    if (align > 0) p = p & ~((uintptr_t)align - 1);
    s_arena_hi = (u8*)p;
    return s_arena_hi;
}

void* OSAllocFromMEM1ArenaLo(u32 size, u32 align) {
    return OSAllocFromArenaLo(size, align);
}

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
    (void)arenaEnd; (void)maxHeaps;
    return arenaStart;
}

/* ================================================================ */
/* OS Thread                                                        */
/* ================================================================ */

/* Simple single-threaded thread emulation for PC port.
 * We track which thread is the "main game thread" (main01) and call it
 * directly. Other threads (DVD, audio) are not actually started — their
 * work is handled synchronously through message queue processing. */

static u8 s_main_thread_storage[0x320] __attribute__((aligned(32)));
/* Fake stack for the main thread — JKRThread constructor reads stackBase/stackEnd */
static u8 s_fake_stack[0x10000];
static OSThread* s_current_thread = NULL;

static void pal_init_main_thread(void) {
    if (s_current_thread) return;
    s_current_thread = (OSThread*)s_main_thread_storage;
    memset(s_main_thread_storage, 0, sizeof(s_main_thread_storage));
    /* Set stackBase and stackEnd so JKRThread constructor doesn't crash.
     * stackBase is at offset 0x304, stackEnd at 0x308 in the OSThread struct. */
    s_current_thread->stackBase = s_fake_stack;
    s_current_thread->stackEnd = (u32*)(s_fake_stack + sizeof(s_fake_stack));
    s_current_thread->state = 2; /* OS_THREAD_STATE_RUNNING */
    s_current_thread->priority = 16;
}

/* Track the main game thread — the first one created from main() */
static OSThread* s_game_thread = NULL;
static void* (*s_game_thread_func)(void*) = NULL;
static void* s_game_thread_param = NULL;

OSThread* OSGetCurrentThread(void) {
    if (!s_current_thread) pal_init_main_thread();
    return s_current_thread;
}

int OSCreateThread(OSThread* thread, void* (*func)(void*), void* param,
                   void* stack, u32 stackSize, OSPriority priority, u16 attr) {
    (void)stack; (void)stackSize; (void)priority; (void)attr;
    /* Store the first thread as the main game thread */
    if (s_game_thread == NULL) {
        s_game_thread = thread;
        s_game_thread_func = func;
        s_game_thread_param = param;
    }
    /* Other threads (DVD, audio) are just tracked but not started */
    return 1;
}

void OSExitThread(void* val) { (void)val; }
int OSJoinThread(OSThread* thread, void* val) { (void)thread; (void)val; return 0; }
void OSDetachThread(OSThread* thread) { (void)thread; }

s32 OSSuspendThread(OSThread* thread) {
    (void)thread;
    return 0;
}

s32 OSResumeThread(OSThread* thread) {
    /* Only call the main game thread (main01) directly.
     * The game's main() creates main01 as a thread then suspends itself.
     * We just call main01() directly here — it never returns. */
    if (thread == s_game_thread && s_game_thread_func) {
        void* (*func)(void*) = s_game_thread_func;
        void* param = s_game_thread_param;
        s_game_thread_func = NULL; /* prevent re-entry */
        s_current_thread = thread;
        func(param);
        /* main01 has an infinite loop, so we only reach here if it exits */
    }
    /* Other threads (DVD, audio) are skipped — single-threaded mode */
    return 0;
}
int OSSetThreadPriority(OSThread* thread, OSPriority priority) { (void)thread; (void)priority; return 1; }
s32 OSGetThreadPriority(OSThread* thread) { (void)thread; return 16; }
void OSCancelThread(OSThread* thread) { (void)thread; }
void OSClearStack(u8 val) { (void)val; }
BOOL OSIsThreadSuspended(OSThread* thread) { (void)thread; return FALSE; }
BOOL OSIsThreadTerminated(OSThread* thread) { (void)thread; return FALSE; }
void OSYieldThread(void) {}
void OSInitThreadQueue(OSThreadQueue* queue) { if (queue) memset(queue, 0, 8); }
void OSSleepThread(OSThreadQueue* queue) { (void)queue; }
void OSWakeupThread(OSThreadQueue* queue) { (void)queue; }
s32 OSEnableScheduler(void) { return 0; }
s32 OSDisableScheduler(void) { return 0; }
OSThread* OSSetIdleFunction(OSIdleFunction idleFunction, void* param, void* stack, u32 stackSize) {
    (void)idleFunction; (void)param; (void)stack; (void)stackSize;
    return NULL;
}
OSSwitchThreadCallback OSSetSwitchThreadCallback(OSSwitchThreadCallback callback) { (void)callback; return NULL; }
void OSSleepTicks(s64 ticks) { (void)ticks; }

/* ================================================================ */
/* OS Mutex / Cond                                                  */
/* ================================================================ */

void OSInitMutex(OSMutex* mutex) { if (mutex) memset(mutex, 0, 0x18); }
void OSLockMutex(OSMutex* mutex) { (void)mutex; }
void OSUnlockMutex(OSMutex* mutex) { (void)mutex; }
BOOL OSTryLockMutex(OSMutex* mutex) { (void)mutex; return TRUE; }
void OSInitCond(OSCond* cond) { if (cond) memset(cond, 0, 8); }
void OSWaitCond(OSCond* cond, OSMutex* mutex) { (void)cond; (void)mutex; }
void OSSignalCond(OSCond* cond) { (void)cond; }

/* ================================================================ */
/* OS Message Queue                                                 */
/* ================================================================ */

/* Message queue — simple synchronous implementation for single-threaded mode.
 * LIMITATION: Uses global state — all message queues share one slot. This works
 * because in single-threaded mode, only one queue is active at a time. If
 * multi-threading is added later, this must be changed to per-queue storage. */
void OSInitMessageQueue(OSMessageQueue* mq, void* msgArray, s32 msgCount) {
    if (mq) memset(mq, 0, 32);
}

static void* s_mq_pending_msg = NULL;
static int s_mq_has_msg = 0;

int OSSendMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    (void)mq; (void)flags;
    s_mq_pending_msg = msg;
    s_mq_has_msg = 1;
    return 1;
}

int OSReceiveMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    (void)mq;
    if (s_mq_has_msg) {
        if (msg) *(void**)msg = s_mq_pending_msg;
        s_mq_has_msg = 0;
        return 1;
    }
    /* Non-blocking: return 0 (no message). Blocking: also return 0 since
     * in single-threaded mode, blocking would deadlock. */
    return 0;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, s32 flags) {
    (void)mq; (void)msg; (void)flags;
    return 1;
}

/* ================================================================ */
/* OS Alarm                                                         */
/* ================================================================ */

BOOL OSCheckAlarmQueue(void) { return FALSE; }
void OSCreateAlarm(OSAlarm* alarm) { if (alarm) memset(alarm, 0, 0x30); }
void OSSetAlarm(OSAlarm* alarm, s64 tick, OSAlarmHandler handler) { (void)alarm; (void)tick; (void)handler; }
void OSSetAbsAlarm(OSAlarm* alarm, s64 time, OSAlarmHandler handler) { (void)alarm; (void)time; (void)handler; }
void OSSetPeriodicAlarm(OSAlarm* alarm, s64 start, s64 period, OSAlarmHandler handler) {
    (void)alarm; (void)start; (void)period; (void)handler;
}
void OSCancelAlarm(OSAlarm* alarm) { (void)alarm; }
void OSSetAlarmTag(OSAlarm* alarm, u32 tag) { (void)alarm; (void)tag; }
void OSCancelAlarms(u32 tag) { (void)tag; }
void OSSetAlarmUserData(OSAlarm* alarm, void* userData) { (void)alarm; (void)userData; }
void* OSGetAlarmUserData(const OSAlarm* alarm) { (void)alarm; return NULL; }

/* ================================================================ */
/* OS Context                                                       */
/* ================================================================ */

static u8 s_context_storage[0x2D0] __attribute__((aligned(32)));

OSContext* OSGetCurrentContext(void) { return (OSContext*)s_context_storage; }
void OSSetCurrentContext(OSContext* context) { (void)context; }
void OSLoadContext(OSContext* context) { (void)context; }
u32 OSSaveContext(OSContext* context) { (void)context; return 1; }
void OSClearContext(OSContext* context) { if (context) memset(context, 0, 0x2D0); }
void OSDumpContext(OSContext* context) { (void)context; }
void OSLoadFPUContext(OSContext* fpucontext) { (void)fpucontext; }
void OSSaveFPUContext(OSContext* fpucontext) { (void)fpucontext; }
void OSInitContext(OSContext* context, u32 pc, u32 newsp) { (void)context; (void)pc; (void)newsp; }
void OSFillFPUContext(OSContext* context) { (void)context; }

static u8 s_stack_buf[4096] __attribute__((aligned(32)));

u32 OSGetStackPointer(void) {
    /* Return the actual stack pointer so bounds checks pass on 64-bit.
     * This avoids falling into OSSwitchFiberEx path in mDoPrintf. */
    void* sp;
#if defined(__x86_64__)
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
#elif defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(sp));
#else
    sp = s_stack_buf + sizeof(s_stack_buf);
#endif
    return (u32)(uintptr_t)sp;
}
u32 OSSwitchStack(u32 newsp) { (void)newsp; return OSGetStackPointer(); }
int OSSwitchFiber(u32 pc, u32 newsp) { (void)pc; (void)newsp; return 0; }
void OSSwitchFiberEx(u32 arg0, u32 arg1, u32 arg2, u32 arg3, u32 pc, u32 newsp) {
    /* On PC, function pointers don't fit in u32. Just call vprintf directly
     * since that's the only caller (mDoPrintf_vprintf_Interrupt). */
    (void)arg2; (void)arg3; (void)pc; (void)newsp;
    vprintf((const char*)(uintptr_t)arg0, *(va_list*)(uintptr_t)arg1);
}

/* ================================================================ */
/* OS Error                                                         */
/* ================================================================ */

u32 __OSFpscrEnableBits = 0;

OSErrorHandler OSSetErrorHandler(u8 error, OSErrorHandler handler) { (void)error; (void)handler; return NULL; }

/* ================================================================ */
/* OS Misc                                                          */
/* ================================================================ */

void OSFatal(const char* msg) {
    fprintf(stderr, "OSFatal: %s\n", msg);
    abort();
}

void OSRegisterShutdownFunction(void* info) { (void)info; }
u32 OSGetResetCode(void) { return 0; }
void OSResetSystem(u32 param0, u32 param1, u32 param2) { (void)param0; (void)param1; (void)param2; exit(0); }
int OSGetProgressiveMode(void) { return 0; }
void OSSetProgressiveMode(int mode) { (void)mode; }
int OSGetEuRgb60Mode(void) { return 0; }
void OSSetEuRgb60Mode(int mode) { (void)mode; }
BOOL OSGetResetButtonState(void) { return FALSE; }

void __OSUnhandledException(u8 exception, OSContext* context, u32 dsisr, u32 dar) {
    (void)exception; (void)context; (void)dsisr; (void)dar;
    fprintf(stderr, "__OSUnhandledException: %d\n", exception);
    abort();
}

void OSSetFontEncode(u16 encode) { (void)encode; }
u16 OSGetFontEncode(void) { return 0; }
u32 OSGetFontTexel(const char* string, void* image, s32 pos, s32 stride, u32* width) {
    (void)string; (void)image; (void)pos; (void)stride;
    if (width) *width = 0;
    return 0;
}
u32 OSGetFontTexture(const char* string, void** image, u32* x, u32* y, u32* width) {
    (void)string;
    if (image) *image = NULL;
    if (x) *x = 0;
    if (y) *y = 0;
    if (width) *width = 0;
    return 0;
}
u32 OSGetFontWidth(const char* string) { (void)string; return 0; }
void OSInitFont(void* fontData) { (void)fontData; }

void __OSStopPlayRecord(void) {}
void __OSStartPlayRecord(void) {}
u32 __OSGetDIConfig(void) { return 0; }

/* ================================================================ */
/* OS Calendar                                                      */
/* ================================================================ */

void OSTicksToCalendarTime(s64 ticks, void* td) { (void)ticks; if (td) memset(td, 0, 40); }
s64 OSCalendarTimeToTicks(void* td) { (void)td; return 0; }

/* ================================================================ */
/* OS Stopwatch                                                     */
/* ================================================================ */

void OSInitStopwatch(OSStopwatch* sw, const char* name) { (void)sw; (void)name; }
void OSStartStopwatch(OSStopwatch* sw) { (void)sw; }
void OSStopStopwatch(OSStopwatch* sw) { (void)sw; }
void OSResetStopwatch(OSStopwatch* sw) { (void)sw; }
void OSCheckStopwatch(OSStopwatch* sw) { (void)sw; }
void OSDumpStopwatch(OSStopwatch* sw) { (void)sw; }

/* ================================================================ */
/* DVD — Host filesystem implementation                             */
/* Maps DVD paths to files in a "data/" directory on the host.      */
/* Paths like "/res/Object/Lk.arc" → "data/res/Object/Lk.arc"      */
/* ================================================================ */

static u8 s_dvd_disk_id_storage[32];
static int s_dvd_disk_id_init = 0;

/* Simple path→entrynum table. Entry 0 = unused. */
#define PAL_DVD_MAX_ENTRIES 2048
static char  s_dvd_paths[PAL_DVD_MAX_ENTRIES][256];
static s32   s_dvd_next_entry = 1; /* 0 = invalid */

/* Per-DVDFileInfo host file state (stored via startAddr/length reuse) */
/* We store a FILE* handle table keyed by a simple index stored in startAddr */
#define PAL_DVD_MAX_OPEN 128
static FILE* s_dvd_files[PAL_DVD_MAX_OPEN];
static char  s_dvd_open_paths[PAL_DVD_MAX_OPEN][256];
static s32   s_dvd_next_handle = 1;

/* Base directory for game data files */
static const char* pal_dvd_get_data_dir(void) {
    const char* env = getenv("TP_DATA_DIR");
    return env ? env : "data";
}

/* Build host path from DVD path */
static void pal_dvd_build_host_path(char* out, size_t outSize, const char* dvdPath) {
    const char* dataDir = pal_dvd_get_data_dir();
    /* Strip leading slash from dvdPath if present */
    if (dvdPath && dvdPath[0] == '/') dvdPath++;
    snprintf(out, outSize, "%s/%s", dataDir, dvdPath ? dvdPath : "");
}

void DVDInit(void) {}

DVDDiskID* DVDGetCurrentDiskID(void) {
    if (!s_dvd_disk_id_init) {
        s_dvd_disk_id_init = 1;
        /* Set up a plausible disk ID for Twilight Princess USA */
        s_dvd_disk_id_storage[0] = 'R'; /* gameName */
        s_dvd_disk_id_storage[1] = 'Z';
        s_dvd_disk_id_storage[2] = 'D';
        s_dvd_disk_id_storage[3] = 'E';
        s_dvd_disk_id_storage[4] = '0'; /* company */
        s_dvd_disk_id_storage[5] = '1';
        s_dvd_disk_id_storage[6] = 0;   /* diskNumber */
        s_dvd_disk_id_storage[7] = 0;   /* gameVersion: 0 = retail */
    }
    return (DVDDiskID*)s_dvd_disk_id_storage;
}

s32 DVDConvertPathToEntrynum(const char* pathPtr) {
    if (!pathPtr) return -1;
    /* Check if we already have this path */
    for (s32 i = 1; i < s_dvd_next_entry; i++) {
        if (strcmp(s_dvd_paths[i], pathPtr) == 0) return i;
    }
    /* Check if the file exists on the host filesystem */
    char hostPath[512];
    pal_dvd_build_host_path(hostPath, sizeof(hostPath), pathPtr);
    FILE* test = fopen(hostPath, "rb");
    if (!test) {
        /* File not found — return -1 (normal for assets not yet extracted) */
        return -1;
    }
    fclose(test);
    /* Register new entry */
    if (s_dvd_next_entry >= PAL_DVD_MAX_ENTRIES) return -1;
    s32 entry = s_dvd_next_entry++;
    strncpy(s_dvd_paths[entry], pathPtr, 255);
    s_dvd_paths[entry][255] = '\0';
    return entry;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    if (!fileName || !fileInfo) return FALSE;
    char hostPath[512];
    pal_dvd_build_host_path(hostPath, sizeof(hostPath), fileName);
    FILE* fp = fopen(hostPath, "rb");
    if (!fp) return FALSE;
    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    /* Store handle */
    if (s_dvd_next_handle >= PAL_DVD_MAX_OPEN) { fclose(fp); return FALSE; }
    s32 handle = s_dvd_next_handle++;
    s_dvd_files[handle] = fp;
    strncpy(s_dvd_open_paths[handle], hostPath, 255);
    s_dvd_open_paths[handle][255] = '\0';
    /* Encode handle index into DVDFileInfo fields */
    fileInfo->startAddr = (u32)handle;
    fileInfo->length = (u32)sz;
    return TRUE;
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    if (!fileInfo) return TRUE;
    s32 handle = (s32)fileInfo->startAddr;
    if (handle > 0 && handle < PAL_DVD_MAX_OPEN && s_dvd_files[handle]) {
        fclose(s_dvd_files[handle]);
        s_dvd_files[handle] = NULL;
    }
    fileInfo->startAddr = 0;
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    (void)prio;
    if (!fileInfo || !addr) return -1;
    s32 handle = (s32)fileInfo->startAddr;
    if (handle <= 0 || handle >= PAL_DVD_MAX_OPEN || !s_dvd_files[handle]) return -1;
    FILE* fp = s_dvd_files[handle];
    if (fseek(fp, offset, SEEK_SET) != 0) return -1;
    size_t bytesRead = fread(addr, 1, (size_t)length, fp);
    if (bytesRead > 0 && !pal_milestone_was_reached(MILESTONE_DVD_READ_OK)) {
        pal_milestone("DVD_READ_OK", MILESTONE_DVD_READ_OK, s_dvd_open_paths[handle]);
    }
    return (s32)bytesRead;
}

int DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* addr, s32 length, s32 offset,
                        DVDCBCallback callback, s32 prio) {
    (void)block; (void)addr; (void)length; (void)offset; (void)callback; (void)prio;
    return 0;
}

int DVDSeekAbsAsyncPrio(DVDCommandBlock* block, s32 offset, DVDCBCallback callback, s32 prio) {
    (void)block; (void)offset; (void)callback; (void)prio;
    return 0;
}

int DVDReadDiskID(DVDCommandBlock* block, DVDDiskID* diskID, DVDCBCallback callback) {
    (void)block; (void)diskID; (void)callback;
    return 0;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { (void)block; return 0; }
s32 DVDGetDriveStatus(void) { return 0; }
void DVDReset(void) {}
void DVDPause(void) {}
void DVDResume(void) {}
s32 DVDCancel(DVDCommandBlock* block) { (void)block; return 0; }
s32 DVDCancelAll(void) { return 0; }
BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) { (void)block; (void)callback; return FALSE; }
s32 DVDGetFileInfoStatus(const DVDFileInfo* fileInfo) { (void)fileInfo; return 0; }

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    if (entrynum <= 0 || entrynum >= s_dvd_next_entry || !fileInfo) return FALSE;
    /* Use the stored path to open the file */
    return DVDOpen(s_dvd_paths[entrynum], fileInfo);
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio) {
    /* Execute synchronously on PC */
    s32 result = DVDReadPrio(fileInfo, addr, length, offset, prio);
    if (callback) callback(result, fileInfo);
    return result >= 0 ? TRUE : FALSE;
}

BOOL DVDChangeDir(const char* dirName) { (void)dirName; return TRUE; }
BOOL DVDGetCurrentDir(char* path, u32 maxlen) {
    if (path && maxlen > 1) { path[0] = '/'; path[1] = '\0'; }
    return TRUE;
}

BOOL DVDSetAutoInvalidation(BOOL autoInval) { (void)autoInval; return FALSE; }

BOOL DVDCompareDiskID(const DVDDiskID* id1, const DVDDiskID* id2) {
    (void)id1; (void)id2;
    return FALSE;
}

/* ================================================================ */
/* VI (Video Interface)                                             */
/* ================================================================ */

static u8 s_framebuffer[640 * 480 * 2]; /* XFB placeholder */

void VIInit(void) {}
void VIConfigure(const GXRenderModeObj* rm) { (void)rm; }
void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height) {
    (void)xOrg; (void)yOrg; (void)width; (void)height;
}
void VIFlush(void) {}
void VIWaitForRetrace(void) {}
void VISetNextFrameBuffer(void* fb) { (void)fb; }
void VISetNextRightFrameBuffer(void* fb) { (void)fb; }
void VISetBlack(BOOL black) { (void)black; }
void VISet3D(BOOL flag) { (void)flag; }
u32 VIGetRetraceCount(void) {
    /* Simulate retrace count based on elapsed time (~60 Hz) */
    static u32 s_start_ms = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u32 now_ms = (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    if (s_start_ms == 0) s_start_ms = now_ms;
    return (now_ms - s_start_ms) * 60 / 1000; /* ~60 Hz */
}
u32 VIGetNextField(void) { return 0; }
u32 VIGetCurrentLine(void) { return 0; }
u32 VIGetTvFormat(void) { return 0; /* VI_NTSC */ }
u32 VIGetScanMode(void) { return 0; }
u32 VIGetDTVStatus(void) { return 0; }
void* VIGetNextFrameBuffer(void) { return s_framebuffer; }
void* VIGetCurrentFrameBuffer(void) { return s_framebuffer; }
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) { (void)cb; return NULL; }
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) { (void)cb; return NULL; }

/* ================================================================ */
/* PAD (GameCube Controller)                                        */
/* ================================================================ */

BOOL PADInit(void) { return TRUE; }
u32 PADRead(PADStatus* status) { if (status) memset(status, 0, 12 * 4); return 0; }
int PADReset(u32 mask) { (void)mask; return 0; }
void PADClamp(PADStatus* status) { (void)status; }
void PADClampCircle(PADStatus* status) { (void)status; }
void PADSetSamplingRate(u32 msec) { (void)msec; }
void PADControlMotor(s32 chan, u32 command) { (void)chan; (void)command; }
void PADSetSpec(u32 spec) { (void)spec; }
int PADGetType(s32 chan, u32* type) { (void)chan; if (type) *type = 0; return 0; }
void PADRecalibrate(u32 mask) { (void)mask; }

/* ================================================================ */
/* AI (Audio Interface)                                             */
/* ================================================================ */

void AIInit(u8* stack) { (void)stack; }
void AIReset(void) {}
AIDCallback AIRegisterDMACallback(AIDCallback callback) { (void)callback; return NULL; }
void AIInitDMA(u32 start_addr, u32 length) { (void)start_addr; (void)length; }
BOOL AIGetDMAEnableFlag(void) { return FALSE; }
void AIStartDMA(void) {}
void AIStopDMA(void) {}
u32 AIGetDMABytesLeft(void) { return 0; }
u32 AIGetDMAStartAddr(void) { return 0; }
u32 AIGetDMALength(void) { return 0; }
BOOL AICheckInit(void) { return TRUE; }
void AISetDSPSampleRate(u32 rate) { (void)rate; }
u32 AIGetDSPSampleRate(void) { return 32000; }

/* ================================================================ */
/* AR (Audio RAM) — emulated with host malloc                       */
/* ================================================================ */

/* ARAM is 16 MB on GameCube. We emulate it with a flat host buffer.
 * AR addresses are offsets into this buffer. */
#define PAL_ARAM_SIZE (16 * 1024 * 1024)
static u8* s_aram_mem = NULL;
static u32 s_aram_base = 0;
static u32 s_aram_top = 0;

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    (void)stack_index_addr; (void)num_entries;
    if (!s_aram_mem) {
        s_aram_mem = (u8*)malloc(PAL_ARAM_SIZE);
        if (s_aram_mem) memset(s_aram_mem, 0, PAL_ARAM_SIZE);
    }
    s_aram_base = 0;
    s_aram_top = 0;
    return 0; /* base address */
}
void ARReset(void) {}
BOOL ARCheckInit(void) { return TRUE; }
u32 ARAlloc(u32 length) {
    u32 addr = s_aram_top;
    s_aram_top += (length + 31) & ~31; /* 32-byte align */
    if (s_aram_top > PAL_ARAM_SIZE) s_aram_top = PAL_ARAM_SIZE;
    return addr;
}
u32 ARFree(u32* length) { (void)length; return 0; }
u32 ARGetBaseAddress(void) { return 0; }
u32 ARGetSize(void) { return PAL_ARAM_SIZE; }
u32 ARGetInternalSize(void) { return PAL_ARAM_SIZE; }
void ARSetSize(void) {}
void ARClear(u32 flag) { (void)flag; }
u32 ARGetDMAStatus(void) { return 0; }
void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length) {
    /* type 0 = ARAM→main, type 1 = main→ARAM.
     * Note: On Wii, mainmem_addr is a physical address. On PC, ARAM DMA is
     * not really used (audio goes through stubs), so this is mostly a no-op. */
    if (!s_aram_mem || aram_addr >= PAL_ARAM_SIZE || length > PAL_ARAM_SIZE - aram_addr) return;
    /* Skip mainmem operations — u32 addresses can't represent 64-bit pointers.
     * Real ARAM DMA is not needed for the PC port. */
    (void)type; (void)mainmem_addr;
}
ARQCallback ARRegisterDMACallback(ARQCallback callback) { (void)callback; return NULL; }

/* ARQ (Audio RAM Queue) */
void ARQInit(void) {}
void ARQReset(void) {}
void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority,
                    u32 source, u32 dest, u32 length, ARQCallback callback) {
    (void)request; (void)owner; (void)type; (void)priority;
    (void)source; (void)dest; (void)length; (void)callback;
}
void ARQRemoveRequest(ARQRequest* request) { (void)request; }
void ARQRemoveOwnerRequest(u32 owner) { (void)owner; }
void ARQFlushQueue(void) {}
void ARQSetChunkSize(u32 size) { (void)size; }
u32 ARQGetChunkSize(void) { return 4096; }
BOOL ARQCheckInit(void) { return TRUE; }

/* ================================================================ */
/* DSP                                                              */
/* ================================================================ */

void DSPInit(void) {}
void DSPReset(void) {}
void DSPHalt(void) {}
void DSPUnhalt(void) {}
BOOL DSPCheckInit(void) { return TRUE; }
u32 DSPCheckMailToDSP(void) { return 0; }
u32 DSPCheckMailFromDSP(void) { return 0; }
u32 DSPReadCPUToDSPMbox(void) { return 0; }
u32 DSPReadMailFromDSP(void) { return 0; }
void DSPSendMailToDSP(u32 mail) { (void)mail; }
void DSPAssertInt(void) {}
u32 DSPGetDMAStatus(void) { return 0; }
DSPTaskInfo* DSPAddTask(DSPTaskInfo* task) { (void)task; return task; }
DSPTaskInfo* DSPCancelTask(DSPTaskInfo* task) { (void)task; return NULL; }
DSPTaskInfo* DSPAssertTask(DSPTaskInfo* task) { (void)task; return task; }

/* __DSP internal functions */
DSPTaskInfo* __DSP_curr_task = NULL;
DSPTaskInfo* __DSP_first_task = NULL;
DSPTaskInfo* __DSP_last_task = NULL;
DSPTaskInfo* __DSP_tmp_task = NULL;

void __DSP_debug_printf(const char* fmt, ...) { (void)fmt; }
void __DSP_exec_task(DSPTaskInfo* curr, DSPTaskInfo* next) { (void)curr; (void)next; }
void __DSP_boot_task(DSPTaskInfo* task) { (void)task; }
void __DSP_insert_task(DSPTaskInfo* task) { (void)task; }
void __DSP_remove_task(DSPTaskInfo* task) { (void)task; }
DSPTaskInfo* __DSPGetCurrentTask(void) { return NULL; }

/* ================================================================ */
/* NAND                                                             */
/* ================================================================ */

s32 NANDInit(void) { return 0; }
s32 NANDOpen(const char* path, NANDFileInfo* info, u8 accType) {
    (void)path; (void)info; (void)accType;
    return -128; /* NAND_RESULT_FATAL_ERROR - signals not available */
}
s32 NANDClose(NANDFileInfo* info) { (void)info; return 0; }
s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) { (void)info; (void)buf; (void)length; return -1; }
s32 NANDWrite(NANDFileInfo* info, void* buf, u32 length) { (void)info; (void)buf; (void)length; return -1; }
s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) { (void)info; (void)offset; (void)whence; return -1; }
s32 NANDCreate(const char* path, u8 perm, u8 attr) { (void)path; (void)perm; (void)attr; return -1; }
s32 NANDDelete(const char* path) { (void)path; return -1; }
s32 NANDGetLength(NANDFileInfo* info, u32* length) { (void)info; if (length) *length = 0; return -1; }
s32 NANDGetStatus(const char* path, NANDStatus* stat) { (void)path; (void)stat; return -1; }
s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer) { (void)fsBlock; (void)inode; (void)answer; return 0; }
s32 NANDCreateDir(const char* path, u8 perm, u8 attr) { (void)path; (void)perm; (void)attr; return -1; }

/* NAND Async */
s32 NANDOpenAsync(const char* path, NANDFileInfo* info, u8 accType,
                  NANDCallback callback, NANDCommandBlock* block) {
    (void)path; (void)info; (void)accType; (void)callback; (void)block;
    return -1;
}
s32 NANDCloseAsync(NANDFileInfo* info, NANDCallback callback, NANDCommandBlock* block) {
    (void)info; (void)callback; (void)block;
    return -1;
}
s32 NANDReadAsync(NANDFileInfo* info, void* buf, u32 length,
                  NANDCallback callback, NANDCommandBlock* block) {
    (void)info; (void)buf; (void)length; (void)callback; (void)block;
    return -1;
}
s32 NANDWriteAsync(NANDFileInfo* info, void* buf, u32 length,
                   NANDCallback callback, NANDCommandBlock* block) {
    (void)info; (void)buf; (void)length; (void)callback; (void)block;
    return -1;
}

/* ================================================================ */
/* SC (System Config)                                               */
/* ================================================================ */

void SCInit(void) {}
u32 SCCheckStatus(void) { return 0; /* SC_STATUS_OK */ }
u8 SCGetLanguage(void) { return 0; /* SC_LANG_JAPANESE */ }
u8 SCGetAspectRatio(void) { return 0; }
u8 SCGetSoundMode(void) { return 0; }
u8 SCGetProgressiveMode(void) { return 0; }
u8 SCGetScreenSaverMode(void) { return 0; }
BOOL SCGetIdleMode(void* data) { (void)data; return FALSE; }
u32 SCGetCounterBias(void) { return 0; }
BOOL SCFindByteArrayItem(void* data, u32 size, u32 item) { (void)data; (void)size; (void)item; return FALSE; }
BOOL SCReplaceByteArrayItem(const void* data, u32 size, u32 item) { (void)data; (void)size; (void)item; return FALSE; }
BOOL SCFindIntegerItem(s32* data, u32 item) { (void)data; (void)item; return FALSE; }
BOOL SCReplaceIntegerItem(s32 data, u32 item) { (void)data; (void)item; return FALSE; }
BOOL SCFlush(void) { return TRUE; }
void SCFlushAsync(SCFlushCallback callback) { (void)callback; }
BOOL SCGetBtDeviceInfoArray(void* data) { (void)data; return FALSE; }
BOOL SCSetBtDeviceInfoArray(const void* data) { (void)data; return FALSE; }

/* ================================================================ */
/* WPAD (Wii Remote)                                                */
/* ================================================================ */

void WPADInit(void) {}
void WPADRead(s32 chan, WPADStatus* status) { (void)chan; if (status) memset(status, 0, 0x2A); }
s32 WPADProbe(s32 chan, u32* devType) { (void)chan; if (devType) *devType = 0; return -1; }
s32 WPADControlMotor(s32 chan, u32 command) { (void)chan; (void)command; return 0; }
s32 WPADControlDpd(s32 chan, u32 command, WPADCallback callback) { (void)chan; (void)command; (void)callback; return 0; }
s32 WPADSetDataFormat(s32 chan, u32 fmt) { (void)chan; (void)fmt; return 0; }
u32 WPADGetDataFormat(s32 chan) { (void)chan; return 0; }
void WPADSetAutoSamplingBuf(s32 chan, void* buf, u32 length) { (void)chan; (void)buf; (void)length; }
u8 WPADGetRadioSensitivity(s32 chan) { (void)chan; return 0; }
BOOL WPADStartSimpleSync(void) { return FALSE; }
BOOL WPADStopSimpleSync(void) { return FALSE; }
void WPADGetAddress(s32 chan, void* addr) { (void)chan; (void)addr; }
void WPADDisconnect(s32 chan) { (void)chan; }
s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback callback) {
    (void)chan; (void)info; (void)callback;
    return -1;
}
BOOL WPADCanAlloc(u32 size) { (void)size; return FALSE; }
void WPADRegisterAllocator(void* (*alloc)(u32), void (*free)(void*)) { (void)alloc; (void)free; }
void WPADSetIdleMode(BOOL mode) { (void)mode; }
BOOL WPADIsSpeakerEnabled(s32 chan) { (void)chan; return FALSE; }
s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback callback) {
    (void)chan; (void)command; (void)callback;
    return 0;
}
s32 WPADSendStreamData(s32 chan, void* buf, u16 length) { (void)chan; (void)buf; (void)length; return 0; }
BOOL WPADIsDpdEnabled(s32 chan) { (void)chan; return FALSE; }

/* ================================================================ */
/* KPAD (Unified Wii Remote API)                                    */
/* ================================================================ */

void KPADInit(void) {}
void KPADReset(void) {}
s32 KPADRead(s32 chan, KPADStatus* sampling_bufs, s32 length) {
    (void)chan; (void)sampling_bufs; (void)length;
    return 0;
}
void KPADSetFSStickClamp(s8 min, s8 max) { (void)min; (void)max; }
void KPADSetBtnRepeat(s32 chan, f32 delay_sec, f32 pulse_sec) { (void)chan; (void)delay_sec; (void)pulse_sec; }
void KPADSetPosParam(s32 chan, f32 play_radius, f32 sensitivity) { (void)chan; (void)play_radius; (void)sensitivity; }
void KPADSetHoriParam(s32 chan, f32 play_radius, f32 sensitivity) { (void)chan; (void)play_radius; (void)sensitivity; }
void KPADSetDistParam(s32 chan, f32 play_radius, f32 sensitivity) { (void)chan; (void)play_radius; (void)sensitivity; }
void KPADSetAccParam(s32 chan, f32 play_radius, f32 sensitivity) { (void)chan; (void)play_radius; (void)sensitivity; }
void KPADEnableDPD(s32 chan) { (void)chan; }
void KPADDisableDPD(s32 chan) { (void)chan; }

/* ================================================================ */
/* HBM (Home Button Menu)                                           */
/* ================================================================ */

void HBMCreate(const HBMDataInfo* pHBInfo) { (void)pHBInfo; }
void HBMDelete(void) {}
void HBMInit(void) {}
s32 HBMCalc(const HBMControllerData* pController) { (void)pController; return 0; }
void HBMDraw(void) {}
s32 HBMGetSelectBtnNum(void) { return 0; }
void HBMSetAdjustFlag(BOOL flag) { (void)flag; }
void HBMStartBlackOut(void) {}
BOOL HBMIsReassignedControllers(void) { return FALSE; }
void HBMCreateSound(void* soundData, void* memBuf, u32 memSize) { (void)soundData; (void)memBuf; (void)memSize; }
void HBMDeleteSound(void) {}
void HBMUpdateSound(void) {}

/* ================================================================ */
/* OS Interrupt (beyond what pal_os_stubs.cpp provides)             */
/* ================================================================ */

typedef void (*__OSInterruptHandler)(u8, OSContext*);

__OSInterruptHandler __OSSetInterruptHandler(u8 interrupt, __OSInterruptHandler handler) {
    (void)interrupt; (void)handler;
    return NULL;
}

__OSInterruptHandler __OSGetInterruptHandler(u8 interrupt) { (void)interrupt; return NULL; }

u32 OSMaskInterrupts(u32 mask) { (void)mask; return 0; }
u32 OSUnmaskInterrupts(u32 mask) { (void)mask; return 0; }

/* ================================================================ */
/* OS Memory protection                                             */
/* ================================================================ */

void OSProtectRange(u32 chan, void* addr, u32 nBytes, u32 control) {
    (void)chan; (void)addr; (void)nBytes; (void)control;
}

/* ================================================================ */
/* EXI (External Interface)                                         */
/* ================================================================ */

BOOL EXIProbe(s32 chan) { (void)chan; return FALSE; }
BOOL EXIProbeEx(s32 chan) { (void)chan; return FALSE; }
s32 EXIGetID(s32 chan, u32 dev, u32* id) { (void)chan; (void)dev; if (id) *id = 0; return 0; }

/* ================================================================ */
/* Card (Memory Card - used in legacy/debug paths)                  */
/* ================================================================ */

s32 CARDInit(void) { return 0; }
s32 CARDGetResultCode(s32 chan) { (void)chan; return 0; }
s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    (void)chan;
    if (byteNotUsed) *byteNotUsed = 0;
    if (filesNotUsed) *filesNotUsed = 0;
    return 0;
}
s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    (void)chan;
    if (memSize) *memSize = 0;
    if (sectorSize) *sectorSize = 0;
    return 0;
}
s32 CARDUnmount(s32 chan) { (void)chan; return 0; }
s32 CARDMountAsync(s32 chan, void* workArea, void* detachCallback, void* attachCallback) {
    (void)chan; (void)workArea; (void)detachCallback; (void)attachCallback;
    return 0;
}

/* ================================================================ */
/* GX Render Mode related (link-time globals from SDK)              */
/* ================================================================ */

/* GXNtsc480Prog and other render modes */
/* (GXNtsc480Int, GXNtsc480IntDf are in pal_gx_stubs.cpp) */

/* ================================================================ */
/* Misc OS functions                                                */
/* ================================================================ */

void __OSInitMemoryProtection(void) {}
void __OSInitSram(void) {}
u32 __OSGetBootInfo(void) { return 0; }
void __OSInitAudioSystem(void) {}

} /* extern "C" */

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

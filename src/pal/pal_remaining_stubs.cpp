/**
 * pal_remaining_stubs.cpp
 * Stubs for remaining undefined references after main stub files.
 * Covers GF/GD extras, misc SDK functions, and a few game-specific stubs.
 */

#include "global.h"

#if PLATFORM_PC || PLATFORM_NX_HB

#include "dolphin/types.h"
#include "revolution/gx/GXEnum.h"
#include "revolution/gx/GXStruct.h"
#include "revolution/gx/GXGeometry.h"
#include "revolution/gd/GDBase.h"
#include "revolution/os/OSModule.h"
#include "dolphin/dvd.h"
#include "revolution/nand.h"
#include <cstring>
#include <cmath>

#ifdef __cplusplus
extern "C" {
#endif

/* --- GX extras --- */
void GXPeekZ(u16 x, u16 y, u32* z) { if (z) *z = 0; }
void GXSetCoPlanar(u8 enable) { (void)enable; }

/* --- GD extras --- */
static u8 s_gd_dummy_buf[4096] __attribute__((aligned(32)));
static GDLObj s_gd_dummy_dl;
GDLObj* __GDCurrentDL = NULL;

/* Initialize the dummy GDLObj on startup. Block load() methods (TevBlock,
 * ColorBlock, etc.) call GDGetCurrOffset/GDOverflowCheck which dereference
 * __GDCurrentDL. On GCN these are only called inside beginDL/endDL, but on
 * PC we call load() directly from J3DMatPacket::draw. The writes are no-ops
 * (GX state is set by J3DGDSet* → GX stubs), but __GDCurrentDL must be valid. */
static void __attribute__((constructor)) pal_gd_init_dummy(void) {
    s_gd_dummy_dl.start  = s_gd_dummy_buf;
    s_gd_dummy_dl.length = sizeof(s_gd_dummy_buf);
    s_gd_dummy_dl.ptr    = s_gd_dummy_buf;
    s_gd_dummy_dl.top    = s_gd_dummy_buf + sizeof(s_gd_dummy_buf);
    __GDCurrentDL = &s_gd_dummy_dl;
}

/* Reset the dummy GDLObj pointer so it never overflows.
 * Called from J3DMatPacket::draw before block load() on PC.
 * Also re-points __GDCurrentDL if it was set to NULL by endDL(). */
void pal_gd_reset_dummy(void) {
    s_gd_dummy_dl.ptr = s_gd_dummy_buf;
    __GDCurrentDL = &s_gd_dummy_dl;
}

void GDSetArray(GXAttr attr, void* base_ptr, u8 stride) {
    /* Wire through to GX state so indexed vertex access works */
    GXSetArray(attr, base_ptr, stride);
}
void GDSetArrayRaw(GXAttr attr, u32 base_ptr_raw, u8 stride) {
    /* Raw address — can't resolve on PC. Set NULL so callers detect it. */
    GXSetArray(attr, NULL, stride);
}
void GDSetVtxDescv(const GXVtxDescList* attrPtr) {
    /* Wire through to GX state so display list parser knows vertex layout */
    GXSetVtxDescv(attrPtr);
}

/* --- DVD extras --- */
BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    (void)block; (void)callback; return FALSE;
}
BOOL DVDOpenDir(const char* dirName, DVDDir* dir) {
    (void)dirName; if (dir) memset(dir, 0, sizeof(DVDDir)); return FALSE;
}
BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirent) {
    (void)dir; (void)dirent; return FALSE;
}
BOOL DVDCloseDir(DVDDir* dir) { (void)dir; return TRUE; }

/* --- NAND extras --- */
void NANDInitBanner(NANDBanner* bnr, u32 flag, const u16* title, const u16* comment) {
    (void)bnr; (void)flag; (void)title; (void)comment;
}
s32 NANDSimpleSafeOpen(const char* path, NANDFileInfo* info, u8 accType, void* buf, u32 length) {
    (void)path; (void)info; (void)accType; (void)buf; (void)length; return -128;
}
s32 NANDSimpleSafeClose(NANDFileInfo* info) { (void)info; return 0; }

/* --- OS extras --- */
s32 OSCheckActiveThreads(void) { return 1; }
u8 OSGetLanguage(void) { return 1; }

/* MEM2 arena: On Wii, this is 64MB of external memory starting at 0x90000000.
 * On PC, we provide a 128MB malloc-backed buffer to account for 64-bit overhead
 * and ARAM archives loaded into main RAM instead of dedicated audio memory. */
static u8 s_mem2_arena[128 * 1024 * 1024];
static u8* s_mem2_lo = s_mem2_arena;
static u8* s_mem2_hi = s_mem2_arena + sizeof(s_mem2_arena);
void* OSGetMEM2ArenaHi(void) { return s_mem2_hi; }
void* OSGetMEM2ArenaLo(void) { return s_mem2_lo; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSLink(OSModuleInfo* module, void* bss) { (void)module; (void)bss; return TRUE; }
BOOL OSLinkFixed(OSModuleInfo* module, void* bss) { (void)module; (void)bss; return TRUE; }
void OSRestart(u32 resetCode) { (void)resetCode; }
void OSReturnToMenu(void) {}
void OSSetMEM2ArenaHi(void* addr) { s_mem2_hi = (u8*)addr; }
void OSSetMEM2ArenaLo(void* addr) { s_mem2_lo = (u8*)addr; }
void OSSetSoundMode(u32 mode) { (void)mode; }
void OSSetStringTable(void* table) { (void)table; }
void OSShutdownSystem(void) {}
BOOL OSUnlink(OSModuleInfo* module) { (void)module; return TRUE; }

/* --- PAD extras --- */
void PADSetAnalogMode(u32 mode) { (void)mode; }

/* --- VI extras (use proper types from headers) --- */
#include "revolution/vi/vifuncs.h"
void VISetTrapFilter(u8 enable) { (void)enable; }
BOOL VIEnableDimming(BOOL enable) { (void)enable; return FALSE; }
void VISetGamma(VIGamma gamma) { (void)gamma; }

/* --- WPAD extras --- */
u32 WPADGetAcceptConnection(void) { return 0; }
u32 WPADGetDpdSensitivity(void) { return 0; }
u32 WPADGetSpeakerVolume(void) { return 0; }
void WPADSetAcceptConnection(u32 accept) { (void)accept; }
void WPADSetAutoSleepTime(u8 minutes) { (void)minutes; }
typedef void (*WPADConnectCallback_t)(s32, s32);
typedef void (*WPADExtensionCallback_t)(s32, s32);
void WPADSetConnectCallback(s32 chan, WPADConnectCallback_t cb) { (void)chan; (void)cb; }
void WPADSetExtensionCallback(s32 chan, WPADExtensionCallback_t cb) { (void)chan; (void)cb; }

/* --- KPAD extras --- */
void KPADSetObjInterval(f32 interval) { (void)interval; }
void KPADSetSensorHeight(s32 chan, f32 height) { (void)chan; (void)height; }

/* --- WENC --- */
s32 WENCGetEncodeData(void* enc, u32 flag, const s16* pcm, s32 samples, u8* out) {
    (void)enc; (void)flag; (void)pcm; (void)samples; (void)out; return 0;
}

#ifdef __cplusplus
}
#endif

/* ========================= C++ stubs ========================= */

/* --- Math: J3DPSMtxArrayConcat (C++ linkage) --- */
#include "JSystem/J3DGraphBase/J3DTransform.h"
void J3DPSMtxArrayConcat(f32 (*a)[4], f32 (*b)[4], f32 (*out)[4], u32 count) {
    /* Each matrix is 3 rows of 4 floats, so stride is 3*4 = 12 floats */
    u32 i;
    for (i = 0; i < count; i++) {
        f32 (*ma)[4] = a + i * 3;
        f32 (*mb)[4] = b + i * 3;
        f32 (*mo)[4] = out + i * 3;
        int r, c;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 4; c++) {
                mo[r][c] = ma[r][0]*mb[0][c] + ma[r][1]*mb[1][c] + ma[r][2]*mb[2][c];
                if (c == 3) mo[r][c] += ma[r][3];
            }
        }
    }
}

/* --- JSUOutputStream / JSURandomOutputStream / JSUMemoryOutputStream --- */
#include "JSystem/JSupport/JSUOutputStream.h"
#include "JSystem/JSupport/JSURandomOutputStream.h"
#include "JSystem/JSupport/JSUMemoryStream.h"

JSUOutputStream::~JSUOutputStream() {}
s32 JSUOutputStream::skip(s32 count, s8 fill) { (void)count; (void)fill; return 0; }
s32 JSUOutputStream::write(const void* src, s32 length) { (void)src; (void)length; return 0; }
void JSUOutputStream::write(const char* str) { (void)str; }

s32 JSURandomOutputStream::seek(s32 offset, JSUStreamSeekFrom origin) {
    (void)offset; (void)origin; return 0;
}
s32 JSURandomOutputStream::getAvailable() const { return 0; }

s32 JSUMemoryOutputStream::getPosition() const { return mPosition; }
s32 JSUMemoryOutputStream::seek(s32 offset, JSUStreamSeekFrom origin) {
    switch (origin) {
    case JSUStreamSeekFrom_SET: mPosition = offset; break;
    case JSUStreamSeekFrom_CUR: mPosition += offset; break;
    case JSUStreamSeekFrom_END: mPosition = mLength + offset; break;
    }
    return mPosition;
}
s32 JSUMemoryOutputStream::getAvailable() const {
    return mLength - mPosition;
}

/* --- JORMContext --- */
#include "JSystem/JHostIO/JORMContext.h"
void JORMContext::genCheckBoxSub(u32 kind, const char* label, u32 id, u32 style,
                                 u16 initValue, u16 mask, JOREventListener* pListener,
                                 u16 posX, u16 posY, u16 width, u16 height) {
    (void)kind; (void)label; (void)id; (void)style; (void)initValue;
    (void)mask; (void)pListener; (void)posX; (void)posY; (void)width; (void)height;
}

/* --- JStudio_JParticle (source files not in Shield splits.txt) --- */
#include "JSystem/JStudio/JStudio_JParticle/control.h"
namespace JStudio_JParticle {
    TCreateObject::~TCreateObject() {}
    bool TCreateObject::create(JStudio::TObject** out,
                               const JStudio::stb::data::TParse_TBlock_object& blk) {
        (void)out; (void)blk; return false;
    }
    JPABaseEmitter* TCreateObject::emitter_create(u32 id) { (void)id; return NULL; }
    void TCreateObject::emitter_destroy(JPABaseEmitter* emitter) { (void)emitter; }
}

/* --- CARD extras (extern "C" in revolution/card.h) --- */
#include "revolution/card.h"
s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    (void)chan; (void)fileNo; (void)stat;
    return -1; /* CARD_RESULT_NOCARD */
}
s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    (void)chan; (void)fileNo; (void)stat;
    return -1; /* CARD_RESULT_NOCARD */
}

/* --- Matrix extras (extern "C" in revolution/mtx.h) --- */
#include "revolution/mtx.h"
void PSMTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    (void)m; (void)srcBase; (void)dstBase; (void)count;
}

/* --- Debug viewer (C++ linkage) --- */
#include "SSystem/SComponent/c_xyz.h"
void dDbVw_drawCircle(int i_bufferType, cXyz& i_pos, f32 i_radius,
                      const GXColor& i_color, u8 i_clipZ, u8 i_width) {
    (void)i_bufferType; (void)i_pos; (void)i_radius;
    (void)i_color; (void)i_clipZ; (void)i_width;
}

/* --- HIO static member — now defined in f_ap_game.cpp --- */

/* --- JORReflexible virtual methods (DEBUG build only) --- */
#if DEBUG
#include "JSystem/JHostIO/JORReflexible.h"

void JORReflexible::listen(u32, const JOREvent*) {}
void JORReflexible::genObjectInfo(const JORGenEvent*) {}
void JORReflexible::listenNodeEvent(const JORNodeEvent*) {}
void JORReflexible::listenPropertyEvent(const JORPropertyEvent*) {}

JORServer* JORReflexible::getJORServer() { return NULL; }

/* mDoHIO stubs — m_Do_hostIO.cpp not in Shield splits */
#include "m_Do/m_Do_hostIO.h"
void mDoHIO_deleteChild(s8) {}

mDoHIO_root_c mDoHIO_root;
void mDoHIO_root_c::update() {}
void mDoHIO_root_c::deleteChild(s8) {}
void mDoHIO_root_c::updateChild(s8) {}
void mDoHIO_root_c::genMessage(JORMContext*) {}
mDoHIO_root_c::~mDoHIO_root_c() {}

s8 mDoHIO_subRoot_c::createChild(const char*, JORReflexible*) { return -1; }
void mDoHIO_subRoot_c::genMessage(JORMContext*) {}
void mDoHIO_subRoot_c::updateChild(s8) {}
void mDoHIO_subRoot_c::deleteChild(s8) {}
mDoHIO_subRoot_c::~mDoHIO_subRoot_c() {}

mDoHIO_child_c::~mDoHIO_child_c() {}

mDoHIO_entry_c::mDoHIO_entry_c() : mNo(-1), mCount(0) {}
mDoHIO_entry_c::~mDoHIO_entry_c() {}
void mDoHIO_entry_c::entryHIO(const char*) {}
void mDoHIO_entry_c::removeHIO() {}
#endif

/* ================================================================ */
/* JGadget_outMessage — debug output message class                  */
/* ================================================================ */
#include "JSystem/JGadget/define.h"

JGadget_outMessage::JGadget_outMessage(MessageFunc, const char*, int) : mMsgFunc(NULL), mWrite_p(mBuffer), mFile(NULL), mLine(0) {
    mBuffer[0] = 0;
}
JGadget_outMessage::~JGadget_outMessage() {}
JGadget_outMessage& JGadget_outMessage::operator<<(const char*) { return *this; }
JGadget_outMessage& JGadget_outMessage::operator<<(char) { return *this; }
JGadget_outMessage& JGadget_outMessage::operator<<(s32) { return *this; }
JGadget_outMessage& JGadget_outMessage::operator<<(u32) { return *this; }
JGadget_outMessage& JGadget_outMessage::operator<<(u16) { return *this; }
JGadget_outMessage& JGadget_outMessage::operator<<(const void*) { return *this; }
void JGadget_outMessage::warning(const char*, int, const char*) {}

/* ================================================================ */
/* Debug / HIO stubs                                                */
/* ================================================================ */

/* dDbVw — debug view */
void dDbVw_Report(int, int, const char*, ...) {}
void dDbVw_deleteDrawPacketList() {}
void dDbVw_drawCylinderM(int, float (*)[4], const GXColor&, unsigned char) {}

/* dDbgCamera */
#include "d/d_camera.h"
#include "d/d_bg_s.h"

class dDbgCamera_c {
public:
    void DrawShape();
    void Finish();
    void Init(dCamera_c*);
    int InitlChk();
    void Reset(dCamera_c*);
    void Run();
    void SetlChk(dBgS_LinChk);
};
void dDbgCamera_c::DrawShape() {}
void dDbgCamera_c::Finish() {}
void dDbgCamera_c::Init(dCamera_c*) {}
int dDbgCamera_c::InitlChk() { return 0; }
void dDbgCamera_c::Reset(dCamera_c*) {}
void dDbgCamera_c::Run() {}
void dDbgCamera_c::SetlChk(dBgS_LinChk) {}
dDbgCamera_c dDbgCamera;

/* dDebugPad */
class dDebugPad_c {
public:
    int Enable(int);
    int Trigger();
};
int dDebugPad_c::Enable(int) { return 0; }
int dDebugPad_c::Trigger() { return 0; }
dDebugPad_c dDebugPad;

/* GX debug draw functions */
extern "C" {
void GXDrawCylinder(u8 numSides) { (void)numSides; }
void GXDrawSphere(u8 numMajor, u8 numMinor) { (void)numMajor; (void)numMinor; }
}

/* PPC floating-point stubs */
extern "C" {
unsigned int PPCMffpscr(void) { return 0; }
void PPCMtfpscr(unsigned int val) { (void)val; }
}

/* Linker section symbols (not used on PC) */
extern "C" {
char _f_text, _e_text, _f_ctors, _e_ctors, _f_dtors, _e_dtors, _f_rodata, _e_rodata;
}

/* DynamicModuleControlBase */
#include "DynamicLink.h"
JKRHeap* DynamicModuleControlBase::createHeap(u32, JKRHeap*) { return NULL; }
void DynamicModuleControlBase::dump(char*) {}
#if DEBUG
u8 DynamicModuleControlBase::verbose = 0;
#endif

/* FixedMemoryCheck */
class FixedMemoryCheck {
public:
    static void checkAll();
    static void diffAll();
    static void easyCreate(void*, unsigned int);
    static void saveAll();
};
void FixedMemoryCheck::checkAll() {}
void FixedMemoryCheck::diffAll() {}
void FixedMemoryCheck::easyCreate(void*, unsigned int) {}
void FixedMemoryCheck::saveAll() {}

/* CaptureScreen, COrthoDivider, CPerspDivider — class definitions from CaptureScreen.h */
#include "CaptureScreen.h"
CProjectionDivider::CProjectionDivider(s32, s32) : field_0x0(0), field_0x4(0), field_0x8(0), field_0xc(0), field_0x10(0), field_0x14(0), field_0x18(0), field_0x1c(0) {}
CaptureScreen::CaptureScreen(const JFWDisplay*) : mpDisplay(NULL), field_0x4(0) {}
COrthoDivider::COrthoDivider(const Mtx44&, int, int) : CProjectionDivider(0, 0) {}
void COrthoDivider::divide(Mtx44&, int, int) const {}
CPerspDivider::CPerspDivider(const Mtx44&, int, int) : CProjectionDivider(0, 0) {}
void CPerspDivider::divide(Mtx44&, int, int) const {}

/* dBgS_HIO */
dBgS_HIO::~dBgS_HIO() {}
int dBgS_HIO::ChkCheckCounter() { return 0; }
int dBgS_HIO::ChkGroundCheckTimer() { return 0; }
int dBgS_HIO::ChkLineOff() { return 0; }
int dBgS_HIO::ChkLineTimer() { return 0; }

/* Collision counters and switches */
int g_ground_counter = 0;
int g_line_counter = 0;
OSStopwatch s_ground_sw;
OSStopwatch s_line_sw;

/* fpcDbSv — debug service */
const char* fpcDbSv_getNameString(short) { return ""; }
extern "C" {
int g_fpcDbSv_service = 0;
}

/* Various global stubs */
u8 g_printOtherHeapDebug = 0;

/* mDoHIO_updateChild */
void mDoHIO_updateChild(s8) {}

/* ================================================================ */
/* JOR / JHI Host-IO debug stubs (all no-op on PC)                  */
/* ================================================================ */
#include "JSystem/JHostIO/JORServer.h"
#include "JSystem/JHostIO/JORFile.h"
#include "JSystem/JHostIO/JHIComm.h"
#include "JSystem/JHostIO/JHIhioASync.h"

void JORInit() {}

u32 JORMessageBox(const char*, const char*, u32) { return 0; }

JORServer* JORServer::instance = NULL;
JORMContext* JORServer::attachMCTX(u32) { return NULL; }
void JORServer::doneEvent() {}
void JORServer::releaseMCTX(JORMContext*) {}
void JORServer::setRootNode(const char*, JORReflexible*, u32, u32) {}

void JORMContext::endNode() {}
void JORMContext::endSelectorSub() {}
void JORMContext::genButton(const char*, u32, u32, JOREventListener*, u16, u16, u16, u16) {}
void JORMContext::genLabel(const char*, u32, u32, JOREventListener*, u16, u16, u16, u16) {}
void JORMContext::genNodeSub(const char*, JORReflexible*, u32, u32) {}
void JORMContext::genSelectorItemSub(const char*, int, u32, u16, u16, u16, u16) {}
void JORMContext::genSliderSub(u32, const char*, u32, u32, s32, s32, s32, JOREventListener*, u16, u16, u16, u16) {}
void JORMContext::invalidNode(JORReflexible*, u32) {}
void JORMContext::startSelectorSub(u32, u32, const char*, u32, u32, s32, JOREventListener*, u16, u16, u16, u16) {}
void JORMContext::updateCheckBoxSub(u32, u32, u16, u16, u32) {}
void JORMContext::updateControl(u32, u32, const char*) {}
void JORMContext::updateSelectorSub(u32, u32, s32, u32) {}
void JORMContext::updateSliderSub(u32, u32, s32, s32, s32, u32) {}

JOREventCallbackListNode::JOREventCallbackListNode(u32, u32, bool) {}
JOREventCallbackListNode::~JOREventCallbackListNode() {}
int JOREventCallbackListNode::JORAct(u32, const char*) { return 0; }

JORFile::JORFile() : mDataStream(NULL, 0) {
    mHandle = 0;
    mFileLength = 0;
    mStatus = 0;
    mNFileName = 0;
    mNBaseName = 0;
    mNExtensionName = 0;
    mFlags = 0;
    field_0x18 = 0;
    mFilename[0] = '\0';
}
void JORFile::close() {}
int JORFile::open(const char*, u32, const char*, const char*, const char*, const char*) { return 0; }
s32 JORFile::readData(void*, s32) { return 0; }
s32 JORFile::writeData(const void*, s32) { return 0; }
void JORFile::readBegin_(s32) {}
void JORFile::readLoop_() {}
void JORFile::writeBegin_(s32) {}
void JORFile::writeLoop_(const void*, s32, u32) {}
void JORFile::writeDone_(s32) {}
void JORFile::waitMessage_() {}

void JHICommBufHeader::init() {}
int JHICommBufHeader::load() { return 0; }
u32 JHICommBufReader::Header::getReadableSize() const { return 0; }
int JHICommBufReader::read(void*, int) { return 0; }
void JHICommBufReader::readEnd() {}

u32 JHIEventLoop() { return 0; }

template<> JHIComPortManager<JHICmnMem>* JHIComPortManager<JHICmnMem>::instance = NULL;

/* ================================================================ */
/* Audio debug stubs                                                */
/* ================================================================ */
#include "Z2AudioLib/Z2DebugSys.h"
#include "JSystem/JAudio2/JASGadget.h"

Z2DebugSys::Z2DebugSys() : JASGlobalInstance<Z2DebugSys>(false) {}
void Z2DebugSys::debugframework() {}
void Z2DebugSys::initJAW() {}
JAISeqDataMgr* Z2DebugSys::initSeSeqDataMgr(const void*) { return NULL; }
void Z2DebugSys::initSoundHioNode() {}

template<> Z2DebugSys* JASGlobalInstance<Z2DebugSys>::sInstance = NULL;

#include "JSystem/JAWExtSystem/JAWExtSystem.h"
void JAWExtSystem::padProc(const JUTGamePad&) {}

/* ================================================================ */
/* GX verify stubs                                                  */
/* ================================================================ */
#include "revolution/gx/GXVerify.h"
extern "C" {
GXVerifyCallback GXSetVerifyCallback(GXVerifyCallback) { return NULL; }
void GXSetVerifyLevel(GXWarningLevel) {}
}

/* ================================================================ */
/* J3DModelSaver stub                                               */
/* ================================================================ */
#include "JSystem/J3DGraphLoader/J3DModelSaver.h"
namespace J3DModelSaverDataBase {
    void* saveBinaryDisplayList(const J3DModel*, J3DBinaryDisplayListSaverFlag, u32) { return NULL; }
}

/* ================================================================ */
/* Game debug HIO classes — vtable + genMessage stubs               */
/* ================================================================ */
#include "d/d_event_debug.h"
#include "d/d_vibration.h"
#include "d/d_s_play.h"
#include "d/d_meter_HIO.h"
#include "d/actor/d_a_npc_cd.h"
#include "d/actor/d_a_npc_cd2.h"
#include "d/d_bg_s.h"
#include "d/d_bg_parts.h"
#include "d/d_s_logo.h"
#include "d/d_map.h"
#include "d/d_menu_dmap.h"
#include "d/d_menu_fmap_map.h"
#include "d/d_menu_window_HIO.h"
#include "d/d_kankyo_debug.h"
#include "f_ap/f_ap_game.h"

/* dEvM HIO */
dEvM_HIO_c::dEvM_HIO_c() : field_0x004(0) {}
void dEvM_HIO_c::genMessage(JORMContext*) {}
void dEvM_HIO_c::listenPropertyEvent(const JORPropertyEvent*) {}

dEvM_play_HIO_c::dEvM_play_HIO_c() : mTargetEvent(-1), mEventIdx(-1), mEventCameraMode(0), field_0xA(0) {}
void dEvM_play_HIO_c::genMessage(JORMContext*) {}
void dEvM_play_HIO_c::listenPropertyEvent(const JORPropertyEvent*) {}

dEvM_reg_HIO_c::dEvM_reg_HIO_c() : mFlagTables(NULL), field_0x008(0), mRootRegIdx(0) {}
void dEvM_reg_HIO_c::genMessage(JORMContext*) {}
void dEvM_reg_HIO_c::listenPropertyEvent(const JORPropertyEvent*) {}
void dEvM_reg_HIO_c::update() {}

dEvM_bit_HIO_c::dEvM_bit_HIO_c() : mFlagTables(NULL), field_0x008(0), mRootBitIdx(0) {}
void dEvM_bit_HIO_c::genMessage(JORMContext*) {}
void dEvM_bit_HIO_c::listenPropertyEvent(const JORPropertyEvent*) {}
void dEvM_bit_HIO_c::update() {}

void dEvM_root_bit_HIO_c::genMessage(JORMContext*) {}
void dEvM_root_reg_HIO_c::genMessage(JORMContext*) {}

/* dScnPly HIO */
dScnPly_preset_HIO_c::dScnPly_preset_HIO_c() : field_0x4(0), field_0x5(0), field_0x2716(0), field_0x2717(0) {
    memset(mPresetData, 0, sizeof(mPresetData));
    memset(filename_buf, 0, sizeof(filename_buf));
}
void dScnPly_preset_HIO_c::exePreset() {}
void dScnPly_preset_HIO_c::listenPropertyEvent(const JORPropertyEvent*) {}
void dScnPly_preset_HIO_c::genMessage(JORMContext*) {}

void dScnPly_preLoad_HIO_c::genMessage(JORMContext*) {}

/* dVibTest */
dVibTest_c::dVibTest_c() : field_0x4(0), m_pattern(0), m_pattern2(0), field_0xa(0), m_randombit(0), m_length(0), field_0x10(0) {}
void dVibTest_c::listenPropertyEvent(const JORPropertyEvent*) {}
void dVibTest_c::genMessage(JORMContext*) {}

/* dMeter_drawHIO_c */
void dMeter_drawHIO_c::listenPropertyEvent(const JORPropertyEvent*) {}

/* daNpcCd HIO */
void daNpcCd_HIO_Jnt_c::genMessage(JORMContext*) {}
void daNpcCd_HIO_Child_c::genMessage(JORMContext*) {}
void daNpcCd_HIO_c::genMessage(JORMContext*) {}

/* daNpcCd2 HIO */
void daNpcCd2_HIO_Jnt_c::genMessage(JORMContext*) {}
void daNpcCd2_HIO_c::genMessage(JORMContext*) {}

/* dBgS_HIO — genMessage was missing (vtable anchor) */
void dBgS_HIO::genMessage(JORMContext*) {}

/* dGov_HIO_c — defined locally in d_gameover.cpp, provide genMessage in that file */

/* ================================================================ */
/* Static data members                                              */
/* ================================================================ */
const u8 dMap_HIO_c::l_listData[] = { 0 };
dMdm_HIO_c* dMdm_HIO_c::mMySelfPointer = NULL;
dMenu_FmapMap_c* dMenu_FmapMap_c::mMySelfPointer = NULL;
dMpath_HIO_n::list_s dMfm_HIO_c::l_list = { NULL, 0 };
dMfm_HIO_c* dMfm_HIO_c::mMySelfPointer = NULL;
const void* dMfm_HIO_prm_res_dst_s::m_res = NULL;
u8 dScnLogo_c::mOpeningCut = 0;
u8 fapGm_HIO_c::m_CpuTimerOff = 0;
u8 fapGm_HIO_c::m_CpuTimerOn = 0;
u8 fapGm_HIO_c::m_CpuTimerStart = 0;
u32 fapGm_HIO_c::m_CpuTimerTick = 0;

/* ================================================================ */
/* dKydb — kankyo debug functions (all no-op)                       */
/* ================================================================ */
void dKydb_HIO_debug_TVdsp(f32, f32, int, int, u16) {}
void dKydb_HIO_debug_Wind() {}
void dKydb_HIO_debug_draw() {}
void dKydb_HIO_kcolor_debug(u8*, u8*, u8*, u8*) {}
void dKydb_HIO_vrbox_debug(u8*, u8*, u8*, u8*) {}
void dKydb_HIO_winddebug_draw() {}
void dKydb_color_HIO_update() {}
void dKydb_dungeonlight_draw() {}
void dKydb_efplight_monitor() {}
void dKydb_plight_monitor() {}
void dKydb_timedisp() {}
void dKydb_vrbox_HIO_update() {}
void dKydb_winddisp_draw() {}
void dMw_HIO_c::update() {}

/* ================================================================ */
/* Other stubs                                                      */
/* ================================================================ */
extern "C" {
void* OSCachedToUncached(void* caddr) { return caddr; }
void* OSPhysicalToCached(u32 paddr) { return (void*)(uintptr_t)paddr; }
}

#include "JSystem/JKernel/JKRAramHeap.h"
u32 JKRAramHeap::getUsedSize(u8) { return 0; }

void dBgp_c::packet_c::draw() {}

#endif /* PLATFORM_PC || PLATFORM_NX_HB */

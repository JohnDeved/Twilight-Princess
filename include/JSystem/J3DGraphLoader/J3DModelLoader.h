#ifndef J3DMODELLOADER_H
#define J3DMODELLOADER_H

#include "JSystem/J3DGraphBase/J3DSys.h"
#include <dolphin/mtx.h>

class J3DModelData;
class J3DMaterialTable;
struct J3DModelHierarchy;

/**
 * On 64-bit PC, the J3D binary format uses 4-byte offsets where the original
 * structs have void* fields (8 bytes on 64-bit). Use u32 for offset fields
 * on PC so the struct layout matches the 32-bit binary format on disc.
 */
#if PLATFORM_PC
#define J3D_PTR_T u32
#else
#define J3D_PTR_T void*
#endif

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DModelBlock {
    /* 0x00 */ u32 mBlockType;
    /* 0x04 */ u32 mBlockSize;
};

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DModelFileData {
    /* 0x00 */ u32 mMagic1;
    /* 0x04 */ u32 mMagic2;
    /* 0x08 */ u8 field_0x08[4];
    /* 0x0C */ u32 mBlockNum;
    /* 0x10 */ u8 field_0x10[0x1C - 0x10];
    /* 0x1C */ int field_0x1c;
    /* 0x20 */ J3DModelBlock mBlocks[1];
};

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DModelInfoBlock : public J3DModelBlock {
    /* 0x08 */ u16 mFlags;
    /* 0x0C */ u32 mPacketNum;
    /* 0x10 */ u32 mVtxNum;
    /* 0x14 */ J3D_PTR_T mpHierarchy;
}; // size 0x18

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DVertexBlock : public J3DModelBlock {
    /* 0x08 */ J3D_PTR_T mpVtxAttrFmtList;
    /* 0x0C */ J3D_PTR_T mpVtxPosArray;
    /* 0x10 */ J3D_PTR_T mpVtxNrmArray;
    /* 0x14 */ J3D_PTR_T mpVtxNBTArray;
    /* 0x18 */ J3D_PTR_T mpVtxColorArray[2];
    /* 0x20 */ J3D_PTR_T mpVtxTexCoordArray[8];
}; // size 0x40

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DEnvelopeBlock : public J3DModelBlock {
    /* 0x08 */ u16 mWEvlpMtxNum;
    /* 0x0C */ J3D_PTR_T mpWEvlpMixMtxNum;
    /* 0x10 */ J3D_PTR_T mpWEvlpMixIndex;
    /* 0x14 */ J3D_PTR_T mpWEvlpMixWeight;
    /* 0x18 */ J3D_PTR_T mpInvJointMtx;
}; // size 0x1C

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DDrawBlock : public J3DModelBlock {
    /* 0x08 */ u16 mMtxNum;
    /* 0x0C */ J3D_PTR_T mpDrawMtxFlag;
    /* 0x10 */ J3D_PTR_T mpDrawMtxIndex;
}; // size 0x14

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DJointBlock : public J3DModelBlock {
    /* 0x08 */ u16 mJointNum;
    /* 0x0C */ J3D_PTR_T mpJointInitData;
    /* 0x10 */ J3D_PTR_T mpIndexTable;
    /* 0x14 */ J3D_PTR_T mpNameTable;
}; // size 0x18

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DMaterialBlock : public J3DModelBlock {
    /* 0x08 */ u16 mMaterialNum;
    /* 0x0C */ J3D_PTR_T mpMaterialInitData;
    /* 0x10 */ J3D_PTR_T mpMaterialID;
    /* 0x14 */ J3D_PTR_T mpNameTable;
    /* 0x18 */ J3D_PTR_T mpIndInitData;
    /* 0x1C */ J3D_PTR_T mpCullMode;
    /* 0x20 */ J3D_PTR_T mpMatColor;
    /* 0x24 */ J3D_PTR_T mpColorChanNum;
    /* 0x28 */ J3D_PTR_T mpColorChanInfo;
    /* 0x2C */ J3D_PTR_T mpAmbColor;
    /* 0x30 */ J3D_PTR_T mpLightInfo;
    /* 0x34 */ J3D_PTR_T mpTexGenNum;
    /* 0x38 */ J3D_PTR_T mpTexCoordInfo;
    /* 0x3C */ J3D_PTR_T mpTexCoord2Info;
    /* 0x40 */ J3D_PTR_T mpTexMtxInfo;
    /* 0x44 */ J3D_PTR_T field_0x44;
    /* 0x48 */ J3D_PTR_T mpTexNo;
    /* 0x4C */ J3D_PTR_T mpTevOrderInfo;
    /* 0x50 */ J3D_PTR_T mpTevColor;
    /* 0x54 */ J3D_PTR_T mpTevKColor;
    /* 0x58 */ J3D_PTR_T mpTevStageNum;
    /* 0x5C */ J3D_PTR_T mpTevStageInfo;
    /* 0x60 */ J3D_PTR_T mpTevSwapModeInfo;
    /* 0x64 */ J3D_PTR_T mpTevSwapModeTableInfo;
    /* 0x68 */ J3D_PTR_T mpFogInfo;
    /* 0x6C */ J3D_PTR_T mpAlphaCompInfo;
    /* 0x70 */ J3D_PTR_T mpBlendInfo;
    /* 0x74 */ J3D_PTR_T mpZModeInfo;
    /* 0x78 */ J3D_PTR_T mpZCompLoc;
    /* 0x7C */ J3D_PTR_T mpDither;
    /* 0x80 */ J3D_PTR_T mpNBTScaleInfo;
};

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DMaterialBlock_v21 : public J3DModelBlock {
    /* 0x08 */ u16 mMaterialNum;
    /* 0x0C */ J3D_PTR_T mpMaterialInitData;
    /* 0x10 */ J3D_PTR_T mpMaterialID;
    /* 0x14 */ J3D_PTR_T mpNameTable;
    /* 0x18 */ J3D_PTR_T mpCullMode;
    /* 0x1C */ J3D_PTR_T mpMatColor;
    /* 0x20 */ J3D_PTR_T mpColorChanNum;
    /* 0x24 */ J3D_PTR_T mpColorChanInfo;
    /* 0x28 */ J3D_PTR_T mpTexGenNum;
    /* 0x2C */ J3D_PTR_T mpTexCoordInfo;
    /* 0x30 */ J3D_PTR_T mpTexCoord2Info;
    /* 0x34 */ J3D_PTR_T mpTexMtxInfo;
    /* 0x38 */ J3D_PTR_T field_0x38;
    /* 0x3C */ J3D_PTR_T mpTexNo;
    /* 0x40 */ J3D_PTR_T mpTevOrderInfo;
    /* 0x44 */ J3D_PTR_T mpTevColor;
    /* 0x48 */ J3D_PTR_T mpTevKColor;
    /* 0x4C */ J3D_PTR_T mpTevStageNum;
    /* 0x50 */ J3D_PTR_T mpTevStageInfo;
    /* 0x54 */ J3D_PTR_T mpTevSwapModeInfo;
    /* 0x58 */ J3D_PTR_T mpTevSwapModeTableInfo;
    /* 0x5C */ J3D_PTR_T mpFogInfo;
    /* 0x60 */ J3D_PTR_T mpAlphaCompInfo;
    /* 0x64 */ J3D_PTR_T mpBlendInfo;
    /* 0x68 */ J3D_PTR_T mpZModeInfo;
    /* 0x6C */ J3D_PTR_T mpZCompLoc;
    /* 0x70 */ J3D_PTR_T mpDither;
    /* 0x74 */ J3D_PTR_T mpNBTScaleInfo;
};

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DMaterialDLBlock : public J3DModelBlock {
    /* 0x08 */ u16 mMaterialNum;
    /* 0x0C */ J3D_PTR_T mpDisplayListInit;
    /* 0x10 */ J3D_PTR_T mpPatchingInfo;
    /* 0x14 */ J3D_PTR_T mpCurrentMtxInfo;
    /* 0x18 */ J3D_PTR_T mpMaterialMode;
    /* 0x1C */ J3D_PTR_T field_0x1c;
    /* 0x20 */ J3D_PTR_T mpNameTable;
};

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DShapeBlock : public J3DModelBlock {
    /* 0x08 */ u16 mShapeNum;
    /* 0x0C */ J3D_PTR_T mpShapeInitData;
    /* 0x10 */ J3D_PTR_T mpIndexTable;
    /* 0x14 */ J3D_PTR_T mpNameTable;
    /* 0x18 */ J3D_PTR_T mpVtxDescList;
    /* 0x1C */ J3D_PTR_T mpMtxTable;
    /* 0x20 */ J3D_PTR_T mpDisplayListData;
    /* 0x24 */ J3D_PTR_T mpMtxInitData;
    /* 0x28 */ J3D_PTR_T mpDrawInitData;
}; // size 0x2C

/**
 * @ingroup jsystem-j3d
 * 
 */
struct J3DTextureBlock : public J3DModelBlock {
    /* 0x08 */ u16 mTextureNum;
    /* 0x0C */ J3D_PTR_T mpTextureRes;
    /* 0x10 */ J3D_PTR_T mpNameTable;
};

enum J3DModelLoaderFlagTypes {
    J3DMLF_None = 0x00000000,

    J3DMLF_MtxSoftImageCalc = 0x00000001,
    J3DMLF_MtxMayaCalc = 0x00000002,
    J3DMLF_MtxBasicCalc = 0x00000004,
    J3DMLF_04 = 0x00000008,
    J3DMLF_MtxTypeMask = J3DMLF_MtxSoftImageCalc | J3DMLF_MtxMayaCalc | J3DMLF_MtxBasicCalc |
                         J3DMLF_04,  // 0 - 2 (0 = Basic, 1 = SoftImage, 2 = Maya)

    J3DMLF_UseImmediateMtx = 0x00000010,
    J3DMLF_UsePostTexMtx = 0x00000020,
    J3DMLF_07 = 0x00000040,
    J3DMLF_08 = 0x00000080,
    J3DMLF_NoMatrixTransform = 0x00000100,
    J3DMLF_10 = 0x00000200,
    J3DMLF_11 = 0x00000400,
    J3DMLF_12 = 0x00000800,
    J3DMLF_13 = 0x00001000,
    J3DMLF_DoBdlMaterialCalc = 0x00002000,
    J3DMLF_15 = 0x00004000,
    J3DMLF_16 = 0x00008000,
    J3DMLF_TevNumShift = 0x00010000,
    J3DMLF_18 = 0x00020000,
    J3DMLF_UseSingleSharedDL = 0x00040000,
    J3DMLF_20 = 0x00080000,
    J3DMLF_21 = 0x00100000,
    J3DMLF_UseUniqueMaterials = 0x00200000,
    J3DMLF_23 = 0x00400000,
    J3DMLF_24 = 0x00800000,
    J3DMLF_Material_UseIndirect = 0x01000000,
    J3DMLF_26 = 0x02000000,
    J3DMLF_27 = 0x04000000,
    J3DMLF_Material_TexGen_Block4 = 0x08000000,
    J3DMLF_Material_PE_Full = 0x10000000,
    J3DMLF_Material_PE_FogOff = 0x20000000,
    J3DMLF_Material_Color_LightOn = 0x40000000,
    J3DMLF_Material_Color_AmbientOn = 0x80000000
};

static inline u32 getMdlDataFlag_TevStageNum(u32 flags) { return (flags & 0x001f0000) >> 0x10; }
static inline u32 getMdlDataFlag_TexGenFlag(u32 flags) { return flags & 0x0c000000; }
static inline u32 getMdlDataFlag_ColorFlag(u32 flags) { return flags & 0xc0000000; }
static inline u32 getMdlDataFlag_PEFlag(u32 flags) { return flags & 0x30000000; }
static inline u32 getMdlDataFlag_MtxLoadType(u32 flags) { return flags & 0x10; }

/**
 * @ingroup jsystem-j3d
 * 
 */
class J3DModelLoader {
public:
    J3DModelLoader();
    void readInformation(J3DModelInfoBlock const*, u32);
    void readVertex(J3DVertexBlock const*);
    void readEnvelop(J3DEnvelopeBlock const*);
    void readDraw(J3DDrawBlock const*);
    void readJoint(J3DJointBlock const*);
    void readShape(J3DShapeBlock const*, u32);
    void readTexture(J3DTextureBlock const*);
    void readTextureTable(J3DTextureBlock const*);
    void readPatchedMaterial(J3DMaterialBlock const*, u32);
    void readMaterialDL(J3DMaterialDLBlock const*, u32);
    void modifyMaterial(u32);

    u32 calcSizeInformation(J3DModelInfoBlock const*, u32);
    u32 calcSizeJoint(J3DJointBlock const*);
    u32 calcSizeEnvelope(J3DEnvelopeBlock const*);
    u32 calcSizeDraw(J3DDrawBlock const*);
    u32 calcSizeShape(J3DShapeBlock const*, u32);
    u32 calcSizeTexture(J3DTextureBlock const*);
    u32 calcSizeTextureTable(J3DTextureBlock const*);
    u32 calcSizePatchedMaterial(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterialDL(J3DMaterialDLBlock const*, u32);


    virtual J3DModelData* load(void const*, u32);
    virtual J3DMaterialTable* loadMaterialTable(void const*);
    virtual J3DModelData* loadBinaryDisplayList(void const*, u32);
    virtual u32 calcLoadSize(void const*, u32);
    virtual u32 calcLoadMaterialTableSize(void const*);
    virtual u32 calcLoadBinaryDisplayListSize(void const*, u32);
    virtual u16 countMaterialNum(void const*);
    virtual void setupBBoardInfo();
    virtual ~J3DModelLoader() {}
    virtual void readMaterial(J3DMaterialBlock const*, u32) {}
    virtual void readMaterial_v21(J3DMaterialBlock_v21 const*, u32) {}
    virtual void readMaterialTable(J3DMaterialBlock const*, u32) {}
    virtual void readMaterialTable_v21(J3DMaterialBlock_v21 const*, u32) {}
    virtual u32 calcSizeMaterial(J3DMaterialBlock const*, u32) { return false; }
    virtual u32 calcSizeMaterialTable(J3DMaterialBlock const*, u32) { return false; }

    /* 0x04 */ J3DModelData* mpModelData;
    /* 0x08 */ J3DMaterialTable* mpMaterialTable;
    /* 0x0C */ J3DShapeBlock const* mpShapeBlock;
    /* 0x10 */ J3DMaterialBlock const* mpMaterialBlock;
    /* 0x14 */ J3DModelHierarchy* mpModelHierarchy;
    /* 0x18 */ u8 field_0x18;
    /* 0x19 */ u8 field_0x19;
    /* 0x1A */ u16 mEnvelopeSize;
};

/**
 * @ingroup jsystem-j3d
 * 
 */
class J3DModelLoader_v26 : public J3DModelLoader {
public:
    ~J3DModelLoader_v26() {}
    void readMaterial(J3DMaterialBlock const*, u32);
    void readMaterialTable(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterial(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterialTable(J3DMaterialBlock const*, u32);
};

/**
 * @ingroup jsystem-j3d
 *
 */
class J3DModelLoader_v21 : public J3DModelLoader {
public:
    ~J3DModelLoader_v21() {}
    void readMaterial_v21(J3DMaterialBlock_v21 const*, u32);
    void readMaterialTable_v21(J3DMaterialBlock_v21 const*, u32);
};

/**
 * @ingroup jsystem-j3d
 * 
 */
class J3DModelLoaderDataBase {
public:
    static J3DModelData* load(void const* i_data, u32 i_flags);
    static J3DModelData* loadBinaryDisplayList(const void* i_data, u32 flags);
};

/**
 * @ingroup jsystem-j3d
 *
 */
struct J3DMtxCalcJ3DSysInitSoftimage {
    static void init(const Vec& param_0, const Mtx& param_1) {
        J3DSys::mCurrentS = param_0;
        MTXCopy(param_1, J3DSys::mCurrentMtx);
    }
};

#endif /* J3DMODELLOADER_H */

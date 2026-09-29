#pragma once

#include <revolution.h>

class J3DModelData;
class J3DMaterialTable;
class J3DModelHierarchy;

enum J3DModelLoaderFlagTypes {
    J3DMLF_None = 0x00000000,

    J3DMLF_MtxSoftImageCalc = 0x00000001,
    J3DMLF_MtxMayaCalc = 0x00000002,
    J3DMLF_MtxBasicCalc = 0x00000004,
    J3DMLF_04 = 0x00000008,
    J3DMLF_MtxTypeMask =
        J3DMLF_MtxSoftImageCalc | J3DMLF_MtxMayaCalc | J3DMLF_MtxBasicCalc | J3DMLF_04,  // 0 - 2 (0 = Basic, 1 = SoftImage, 2 = Maya)

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

// Offsets inside a model block are stored as 32-bit values relative to the block start.
// The Wii build reads them through void* fields; natively they must stay 32 bits wide.
#ifdef PETARI_NATIVE
typedef u32 J3DBlockOffset;
#else
typedef void* J3DBlockOffset;
#endif

struct J3DModelBlock {
    u32 mBlockType;
    u32 mBlockSize;
};

struct J3DModelFileData {
    u32 mMagic1;
    u32 mMagic2;
    u8 _8[4];
    u32 mBlockNum;
    u8 _10[0x10];
    J3DModelBlock mBlocks[1];
};

struct J3DModelInfoBlock : public J3DModelBlock {
    u16 mFlags;
    u32 mPacketNum;
    u32 mVtxNum;
    J3DBlockOffset mpHierarchy;
};

struct J3DVertexBlock : public J3DModelBlock {
    J3DBlockOffset mpVtxAttrFmtList;
    J3DBlockOffset mpVtxPosArray;
    J3DBlockOffset mpVtxNrmArray;
    J3DBlockOffset mpVtxNBTArray;
    J3DBlockOffset mpVtxColorArray[2];
    J3DBlockOffset mpVtxTexCoordArray[8];
};

struct J3DEnvelopeBlock : public J3DModelBlock {
    u16 mWEvlpMtxNum;
    J3DBlockOffset mpWEvlpMixMtxNum;
    J3DBlockOffset mpWEvlpMixIndex;
    J3DBlockOffset mpWEvlpMixWeight;
    J3DBlockOffset mpInvJointMtx;
};

struct J3DDrawBlock : public J3DModelBlock {
    u16 mMtxNum;
    J3DBlockOffset mpDrawMtxFlag;
    J3DBlockOffset mpDrawMtxIndex;
};

struct J3DJointBlock : public J3DModelBlock {
    /* 0x08 */ u16 mJointNum;
    /* 0x0C */ J3DBlockOffset mpJointInitData;
    /* 0x10 */ J3DBlockOffset mpIndexTable;
    /* 0x14 */ J3DBlockOffset mpNameTable;
};  // size 0x18

struct J3DMaterialBlock : public J3DModelBlock {
    u16 mMaterialNum;
    J3DBlockOffset mpMaterialInitData;
    J3DBlockOffset mpMaterialID;
    J3DBlockOffset mpNameTable;
    J3DBlockOffset mpIndInitData;
    J3DBlockOffset mpCullMode;
    J3DBlockOffset mpMatColor;
    J3DBlockOffset mpColorChanNum;
    J3DBlockOffset mpColorChanInfo;
    J3DBlockOffset mpAmbColor;
    J3DBlockOffset mpLightInfo;
    J3DBlockOffset mpTexGenNum;
    J3DBlockOffset mpTexCoordInfo;
    J3DBlockOffset mpTexCoord2Info;
    J3DBlockOffset mpTexMtxInfo;
    J3DBlockOffset field_0x44;
    J3DBlockOffset mpTexNo;
    J3DBlockOffset mpTevOrderInfo;
    J3DBlockOffset mpTevColor;
    J3DBlockOffset mpTevKColor;
    J3DBlockOffset mpTevStageNum;
    J3DBlockOffset mpTevStageInfo;
    J3DBlockOffset mpTevSwapModeInfo;
    J3DBlockOffset mpTevSwapModeTableInfo;
    J3DBlockOffset mpFogInfo;
    J3DBlockOffset mpAlphaCompInfo;
    J3DBlockOffset mpBlendInfo;
    J3DBlockOffset mpZModeInfo;
    J3DBlockOffset mpZCompLoc;
    J3DBlockOffset mpDither;
    J3DBlockOffset mpNBTScaleInfo;
};

struct J3DMaterialBlock_v21 : public J3DModelBlock {
    u16 mMaterialNum;
    J3DBlockOffset mpMaterialInitData;
    J3DBlockOffset mpMaterialID;
    J3DBlockOffset mpNameTable;
    J3DBlockOffset mpCullMode;
    J3DBlockOffset mpMatColor;
    J3DBlockOffset mpColorChanNum;
    J3DBlockOffset mpColorChanInfo;
    J3DBlockOffset mpTexGenNum;
    J3DBlockOffset mpTexCoordInfo;
    J3DBlockOffset mpTexCoord2Info;
    J3DBlockOffset mpTexMtxInfo;
    J3DBlockOffset field_0x38;
    J3DBlockOffset mpTexNo;
    J3DBlockOffset mpTevOrderInfo;
    J3DBlockOffset mpTevColor;
    J3DBlockOffset mpTevKColor;
    J3DBlockOffset mpTevStageNum;
    J3DBlockOffset mpTevStageInfo;
    J3DBlockOffset mpTevSwapModeInfo;
    J3DBlockOffset mpTevSwapModeTableInfo;
    J3DBlockOffset mpFogInfo;
    J3DBlockOffset mpAlphaCompInfo;
    J3DBlockOffset mpBlendInfo;
    J3DBlockOffset mpZModeInfo;
    J3DBlockOffset mpZCompLoc;
    J3DBlockOffset mpDither;
    J3DBlockOffset mpNBTScaleInfo;
};

struct J3DMaterialDLBlock : public J3DModelBlock {
    u16 mMaterialNum;
    J3DBlockOffset mpDisplayListInit;
    J3DBlockOffset mpPatchingInfo;
    J3DBlockOffset mpCurrentMtxInfo;
    J3DBlockOffset mpMaterialMode;
    J3DBlockOffset _1C;
    J3DBlockOffset mpNameTable;
};

struct J3DShapeBlock : public J3DModelBlock {
    u16 mShapeNum;          // 0x00
    J3DBlockOffset mpShapeInitData;  // 0x04
    J3DBlockOffset mpIndexTable;     // 0x08
    J3DBlockOffset mpNameTable;      // 0x0C
    J3DBlockOffset mpVtxDescList;
    J3DBlockOffset mpMtxTable;
    J3DBlockOffset mpDisplayListData;
    J3DBlockOffset mpMtxInitData;
    J3DBlockOffset mpDrawInitData;
};

struct J3DTextureBlock : public J3DModelBlock {
    u16 mTextureNum;
    J3DBlockOffset mpTextureRes;
    J3DBlockOffset mpNameTable;
};

class J3DModelLoader {
public:
    J3DModelLoader();

    virtual J3DModelData* load(const void*, u32);
    virtual J3DMaterialTable* loadMaterialTable(const void*);
    virtual J3DModelData* loadBinaryDisplayList(const void*, u32);
    virtual u32 calcLoadSize(const void*, u32);
    virtual u32 calcLoadMaterialTableSize(const void*);
    virtual u32 calcLoadBinaryDisplayListSize(void const*, u32);
    virtual u16 countMaterialNum(void const*);
    virtual void setupBBoardInfo();
    virtual ~J3DModelLoader() {
    }

    virtual void readMaterial(J3DMaterialBlock const*, u32) {
    }

    virtual void readMaterial_v21(J3DMaterialBlock_v21 const*, u32) {
    }

    virtual void readMaterialTable(J3DMaterialBlock const*, u32) {
    }

    virtual void readMaterialTable_v21(J3DMaterialBlock_v21 const*, u32) {
    }

    virtual u32 calcSizeMaterial(J3DMaterialBlock const*, u32) {
        return false;
    }

    virtual u32 calcSizeMaterialTable(J3DMaterialBlock const*, u32) {
        return false;
    }

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
    u32 calcSizeDraw(J3DDrawBlock const*) NO_INLINE;
    u32 calcSizeShape(J3DShapeBlock const*, u32);
    u32 calcSizeTexture(J3DTextureBlock const*);
    u32 calcSizeTextureTable(J3DTextureBlock const*);
    u32 calcSizePatchedMaterial(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterialDL(J3DMaterialDLBlock const*, u32);

    J3DModelData* mpModelData;                // 0x04
    J3DMaterialTable* mpMaterialTable;        // 0x08
    const J3DShapeBlock* mpShapeBlock;        // 0x0C
    const J3DMaterialBlock* mpMaterialBlock;  // 0x10
    J3DModelHierarchy* mpModelHierarchy;      // 0x14
    u8 field_0x18;
    u8 field_0x19;
    u16 mEnvelopeSize;  // 0x1A
};

class J3DModelLoader_v26 : public J3DModelLoader {
public:
    J3DModelLoader_v26() {
    }

    ~J3DModelLoader_v26() {
    }

    void readMaterial(J3DMaterialBlock const*, u32);
    void readMaterialTable(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterial(J3DMaterialBlock const*, u32);
    u32 calcSizeMaterialTable(J3DMaterialBlock const*, u32);
};

class J3DModelLoader_v21 : public J3DModelLoader {
public:
    inline J3DModelLoader_v21() {
    }

    ~J3DModelLoader_v21() {
    }

    void readMaterial_v21(J3DMaterialBlock_v21 const*, u32);
    void readMaterialTable_v21(J3DMaterialBlock_v21 const*, u32);
};

class J3DModelLoaderDataBase {
public:
    static J3DModelData* load(void const*, u32);
    static J3DMaterialTable* loadMaterialTable(const void*);
    static J3DModelData* loadBinaryDisplayList(const void*, u32);
};

static inline u32 getMdlDataFlag_TevStageNum(u32 flags) {
    return (flags & 0x001f0000) >> 0x10;
}

static inline u32 getMdlDataFlag_TexGenFlag(u32 flags) {
    return flags & 0x0c000000;
}

static inline u32 getMdlDataFlag_ColorFlag(u32 flags) {
    return flags & 0xc0000000;
}

static inline u32 getMdlDataFlag_PEFlag(u32 flags) {
    return flags & 0x30000000;
}

static inline u32 getMdlDataFlag_MtxLoadType(u32 flags) {
    return flags & 0x10;
}

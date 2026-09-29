#pragma once

#include "JSystem/JAudio2/JAISeqDataMgr.hpp"
#include "JSystem/JSupport/JSUBigEndian.hpp"

struct JAISeqData;
struct JAISeqDataRegion;

// Sequence collection (.bsc) structures overlay big-endian disc data that is not
// always 4-byte aligned; see JSUBigEndian.hpp.
struct JAUSeqCollectionData {
    /* 0x00 */ char magic[2];
    /* 0x02 */ JSU_BE_UNALIGNED(u16) mGroupNum;
    /* 0x04 */ JSU_BE_UNALIGNED(u32) mFileSize;
    /* 0x08 */ JSU_BE_UNALIGNED(u32) mSoundTableOffsets;
};

struct JAUSeqCollectionSoundTable {
    /* 0x00 */ JSU_BE_UNALIGNED(u32) mWaveNum;
    /* 0x04 */ JSU_BE_UNALIGNED(u32) mWaveSeqOffsets[];
};

class JAUSeqCollection {
public:
    JAUSeqCollection();

    void init(void const*);
    bool getSeqData(int, int, JAISeqData*);
    bool getSeqDataRegion(JAISeqDataRegion*);

    bool isValid() const {
        return mCollectionData != nullptr;
    }

    const JAUSeqCollectionData* getCollectionData() const {
        return mCollectionData;
    }

    /* 0x00 */ u16 mGroupNum;
    /* 0x04 */ const JSU_BE_UNALIGNED(u32)* mSoundTableOffsets;
    /* 0x08 */ const JAUSeqCollectionData* mCollectionData;
    /* 0x0C */ u32 mFileSize;
};

class JAUSeqDataMgr_SeqCollection : public JAISeqDataMgr, public JAUSeqCollection {
public:
    JAUSeqDataMgr_SeqCollection();
    bool setSeqDataUser(JAISeqDataUser*);
    int releaseSeqData();
    SeqDataReturnValue getSeqData(JAISoundID, JAISeqData*);
    ~JAUSeqDataMgr_SeqCollection();

    const void* getResource() const {
        return mSoundTableOffsets;
    }
    void init(const void* pData) {
        JAUSeqCollection::init(pData);
    }

    /* 0x14 */ JAISeqDataUser* user_;
};

#pragma once

#include <revolution/types.h>

#ifdef PETARI_NATIVE
#include "Game/Util/BigEndian.hpp"
#endif

// An entry of spktable.bct, in place in the resource (big-endian; decoded on read natively).
struct SpkParameters {
#ifdef PETARI_NATIVE
    /* 0x00 */ BigEndianValue< u16 > mWaveID;
    /* 0x02 */ u8 _2;
    /* 0x03 */ u8 mVolume;
    /* 0x04 */ BigEndianValue< u16 > mReleaseTime;
    /* 0x06 */ BigEndianValue< u16 > _padding;  // (?)
#else
    /* 0x00 */ u16 mWaveID;
    /* 0x02 */ u8 _2;
    /* 0x03 */ u8 mVolume;
    /* 0x04 */ u16 mReleaseTime;
    /* 0x06 */ u16 _padding;  // (?)
#endif
};

struct SpkFile {
    /* 0x0 */ u32 count;
    /* 0x4 */ u32 parametersOffset;
    /* 0x8 */ u32 namesOffset;
    /* 0xC */ u32 initialized;
};

class SpkTable {
public:
    SpkTable();

    void setResource(void*);

    SpkParameters* getParameters(int idx) {
        return &mParameters[idx];
    }

    /* 0x0 */ bool mInitialized;
    /* 0x4 */ s32 mResourceCount;
    /* 0x8 */ SpkParameters* mParameters;
    /* 0xC */ const char** mNames;
};

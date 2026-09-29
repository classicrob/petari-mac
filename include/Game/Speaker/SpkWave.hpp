#pragma once

#include <revolution/types.h>

#ifdef PETARI_NATIVE
#include "Game/Util/BigEndian.hpp"
#endif

// A wave in spkwave.csw, in place in the resource. Natively the header is decoded on read
// and the big-endian s16 samples by SpkMixingBuffer::mix.
struct WaveData {
#ifdef PETARI_NATIVE
    BigEndianValue< u32 > mSize;
    BigEndianValue< u32 > mLoopStartPos;
    BigEndianValue< u32 > mLoopEndPos;
#else
    u32 mSize;
    u32 mLoopStartPos;
    u32 mLoopEndPos;
#endif
    s16 mWave[];
};

class SpkWave {
public:
    SpkWave();

    void setResource(void*);
    s32 getWaveSize(s32) const;
    u32 getLoopStartPos(s32) const;
    u32 getLoopEndPos(s32) const;
    s16* getWave(s32) const;
    WaveData* getWaveData(s32) const;

    /* 0x0 */ void* mResource;  // Raw data of AudioRes/SpkRes/SpkRes.arc
};

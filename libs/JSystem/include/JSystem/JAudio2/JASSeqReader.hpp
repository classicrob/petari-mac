#pragma once

#include <revolution/types.h>
#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#endif

class JASSeqReader {
public:
    JASSeqReader() {
        init();
    }
    void init();
    void init(void*);
    bool call(u32);
    bool loopStart(u32);
    bool loopEnd();
    bool ret();
    u32 readMidiValue();
    void* getStackPtr(u32 idx) const;

    void jump(u32 param_1) {
        mSeqCursor = mSeqBuff + param_1;
    }

    void jump(void* param_1) {
        mSeqCursor = (u8*)param_1;
    }

    u32 get24(u32 param_0) const {
#ifdef PETARI_NATIVE
        return Petari::readU24BE(mSeqBuff + param_0);
#else
        return (*(u32*)(mSeqBuff + param_0 - 1)) & 0xffffff;
#endif
    }

    u32* getBase() {
        return (u32*)mSeqBuff;
    }
    u32 getOffset() {
#ifdef PETARI_NATIVE
        return static_cast<u32>(mSeqCursor - mSeqBuff);
#else
        return (u32)mSeqCursor - (u32)mSeqBuff;
#endif
    }
    u8* getAddr(u32 param_0) {
        return mSeqBuff + param_0;
    }
    u8 getByte(u32 param_0) const {
        return *(mSeqBuff + param_0);
    }
    u16 get16(u32 param_0) const {
#ifdef PETARI_NATIVE
        return Petari::readU16BE(mSeqBuff + param_0);
#else
        return *(u16*)(mSeqBuff + param_0);
#endif
    }
    u32 get32(u32 param_0) const {
#ifdef PETARI_NATIVE
        return Petari::readU32BE(mSeqBuff + param_0);
#else
        return *(u32*)(mSeqBuff + param_0);
#endif
    }
    u8* getCur() {
        return mSeqCursor;
    }
    u32 readByte() {
        return *mSeqCursor++;
    }
    u32 read16() {
#ifdef __MWERKS__
        return *((u16*)mSeqCursor)++;
#elif defined(PETARI_NATIVE)
        const u16 value = Petari::readU16BE(mSeqCursor);
        mSeqCursor += 2;
        return value;
#else
        u16* value = (u16*)mSeqCursor;
        mSeqCursor += 2;
        return *value;
#endif
    }
    u32 read24() {
#ifdef PETARI_NATIVE
        const u32 value = Petari::readU24BE(mSeqCursor);
        mSeqCursor += 3;
        return value;
#else
        mSeqCursor--;
#ifdef __MWERKS__
        return (*((u32*)mSeqCursor)++) & 0x00ffffff;
#else
        u32* value = (u32*)mSeqCursor;
        mSeqCursor += 4;
        return *value & 0x00ffffff;
#endif
#endif
    }
    u16 getLoopCount() const {
        if (mNumStacks == 0) {
            return 0;
        }
        return mLoopCounts[mNumStacks - 1];
    }

    /* 0x00 */ u8* mSeqBuff;
    /* 0x04 */ u8* mSeqCursor;
    /* 0x08 */ u32 mNumStacks;
    /* 0x0C */ u8* mStackPtrs[8];
    /* 0x2C */ u16 mLoopCounts[8];
};

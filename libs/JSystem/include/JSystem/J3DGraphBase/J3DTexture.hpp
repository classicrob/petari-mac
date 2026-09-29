#pragma once

#include "JSystem/J3DGraphBase/J3DTevs.hpp"
#include "JSystem/JUtility/JUTTexture.hpp"
#include <stdint.h>

class J3DTexture {
private:
    /* 0x0 */ u16 mNum;
    /* 0x2 */ u16 unk_0x2;
    /* 0x4 */ ResTIMG* mpRes;

public:
    J3DTexture(u16 num, ResTIMG* res) : mNum(num), unk_0x2(0), mpRes(res) {
    }

    void loadGX(u16, GXTexMapID) const;
    void entryNum(u16);
    void addResTIMG(u16, ResTIMG const*);
    virtual ~J3DTexture() {
    }

    u16 getNum() const {
        return mNum;
    }

    ResTIMG* getResTIMG(u16 index) const {
        return &mpRes[index];
    }

    void setResTIMG(u16 index, const ResTIMG& timg) {
#ifdef PETARI_NATIVE
        // Rebased offsets must stay within the signed 32-bit range read back
        // through JUT_RESTIMG_OFFSET.
        intptr_t delta = reinterpret_cast< intptr_t >(&timg) - reinterpret_cast< intptr_t >(mpRes + index);
        if (delta + static_cast< intptr_t >(timg.mImageDataOffset) != static_cast< s32 >(delta + timg.mImageDataOffset) ||
            delta + static_cast< intptr_t >(timg.mPaletteDataOffset) != static_cast< s32 >(delta + timg.mPaletteDataOffset)) {
            OSPanic(__FILE__, __LINE__, "J3DTexture::setResTIMG: texture data is more than 2 GiB from its header");
        }
#endif
        mpRes[index] = timg;
        mpRes[index].mImageDataOffset = ((mpRes[index].mImageDataOffset + (uintptr_t)&timg - (uintptr_t)(mpRes + index)));
        mpRes[index].mPaletteDataOffset = ((mpRes[index].mPaletteDataOffset + (uintptr_t)&timg - (uintptr_t)(mpRes + index)));
    }
};

extern J3DTexMtxInfo const j3dDefaultTexMtxInfo;

class J3DTexMtx {
public:
    J3DTexMtx() {
        mTexMtxInfo = j3dDefaultTexMtxInfo;
    }

    J3DTexMtx(const J3DTexMtxInfo& info) {
        mTexMtxInfo = info;
    }

    void load(u32) const;
    void calc(const Mtx);
    void calcTexMtx(const Mtx);
    void calcPostTexMtx(const Mtx);
    inline void loadTexMtx(u32) const;
    inline void loadPostTexMtx(u32) const;

    J3DTexMtxInfo& getTexMtxInfo() {
        return mTexMtxInfo;
    }
    Mtx& getMtx() {
        return mMtx;
    }
    void setEffectMtx(Mtx effectMtx) {
        mTexMtxInfo.setEffectMtx(effectMtx);
    }

    /* 0x00 */ J3DTexMtxInfo mTexMtxInfo;

private:
    /* 0x64 */ Mtx mMtx;
};  // Size: 0x94

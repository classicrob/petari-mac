#include "Game/Speaker/SpkMixingBuffer.hpp"
#include "Game/Speaker/SpkSound.hpp"
#include <JSystem/JAudio2/JASCalc.hpp>
#include <JSystem/JAudio2/JASGlobal.hpp>
#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#endif

SpkMixingBuffer::SpkMixingBuffer(JKRHeap* pHeap) {
    for (int i = 0; i < 4; i++) {
        mSamples[i] = new (pHeap, 0) s16[40];
        JASCalc::bzero((void*)mSamples[i], sizeof(s16) * 40);
    }
}

void SpkMixingBuffer::mix(s32 padChannel, s16* pWaveData, s32 readSize, f32 volume, s32 offset) {
    for (s32 i = 0; i < readSize; i++) {
        s32 sample = mSamples[padChannel][i + offset];
#ifdef PETARI_NATIVE
        // Wave data points into spkwave.csw, whose samples are big-endian.
        s16 wav = static_cast< s16 >(PetariNative::readU16BE(&pWaveData[i]));
#else
        s16 wav = pWaveData[i];
#endif

        sample += (s32)(wav * volume);

        mSamples[padChannel][i + offset] = JASCalc::clamp< s16, s32 >(sample);
    }
}

const s16* SpkMixingBuffer::getSamples(s32 idx) const {
    return mSamples[idx];
}

bool SpkMixingBuffer::update(s32 idx) {
    JASCalc::bzero((void*)mSamples[idx], 0x50);
    return JASGlobalInstance< SpkSoundHolder >::sInstance->update(idx);
}

#include "Game/Screen/THPSimplePlayerWrapper.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include <JSystem/JAudio2/JASAiCtrl.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <cstring>
#ifdef PETARI_NATIVE
#include <cstdlib>
#include <petari/endian.hpp>

// petari/platform/os_host.hpp (native/platform): run host work without holding the OS CPU.
extern "C" void petari_os_begin_host_blocking(void);
extern "C" void petari_os_end_host_blocking(void);

namespace PetariNative::Platform::OS {
    // native/platform/os/os_internal.hpp: the OS thread bound to the calling host thread, or
    // null for host threads and threads already doing host-blocking work.
    OSThread* boundThread();
}  // namespace PetariNative::Platform::OS
#endif

static u16 VolumeTable[] = {0,     2,     8,     18,    32,    50,    73,    99,    130,   164,   203,   245,   292,   343,   398,   457,
                            520,   587,   658,   733,   812,   895,   983,   1074,  1170,  1269,  1373,  1481,  1592,  1708,  1828,  1952,
                            2080,  2212,  2348,  2488,  2632,  2781,  2933,  3090,  3250,  3415,  3583,  3756,  3933,  4114,  4298,  4487,
                            4680,  4877,  5079,  5284,  5493,  5706,  5924,  6145,  6371,  6600,  6834,  7072,  7313,  7559,  7809,  8063,
                            8321,  8583,  8849,  9119,  9394,  9672,  9954,  10241, 10531, 10826, 11125, 11427, 11734, 12045, 12360, 12679,
                            13002, 13329, 13660, 13995, 14335, 14678, 15025, 15377, 15732, 16092, 16456, 16823, 17195, 17571, 17951, 18335,
                            18723, 19115, 19511, 19911, 20316, 20724, 21136, 21553, 21974, 22398, 22827, 23260, 23696, 24137, 24582, 25031,
                            25484, 25941, 26402, 26868, 27337, 27810, 28288, 28769, 29255, 29744, 30238, 30736, 31238, 31744, 32254, 32768};

static s32 WorkBuffer[16] __attribute__((aligned(32)));

// Bytes per interleaved stereo sample in the mix buffers.
#define THP_STEREO_SAMPLE_BYTES (2 * sizeof(s16))

THPSimplePlayerStaticAudio THPSimplePlayerWrapper::mStaticAudioPlayer;
THPSimplePlayerWrapper* THPSimplePlayerStaticAudio::mPlayer;

namespace NrvTHPSimplePlayerWrapper {
    NEW_NERVE(HostTypeWait, THPSimplePlayerWrapper, Wait);
    NEW_NERVE(HostTypeReadHeader, THPSimplePlayerWrapper, ReadHeader);
    NEW_NERVE(HostTypeReadFrameComp, THPSimplePlayerWrapper, ReadFrameComp);
    NEW_NERVE(HostTypeReadVideoComp, THPSimplePlayerWrapper, ReadVideoComp);
    NEW_NERVE(HostTypeReadAudioComp, THPSimplePlayerWrapper, ReadAudioComp);
    NEW_NERVE(HostTypeReadPreLoad, THPSimplePlayerWrapper, ReadPreLoad);
};  // namespace NrvTHPSimplePlayerWrapper

namespace {
#ifdef PETARI_NATIVE
    // Frame validation and video decoding are pure host work on the frame and texture
    // buffers the decoding thread owns (no allocation, no OS calls; a few ms per frame).
    // Natively an OS thread holds the CPU until an interrupt-state change, so a decode
    // would delay interrupt-readied threads (audio) for its whole length, and longer when
    // the host deschedules it; the Wii preempts it at any instruction. The OS thread gives
    // up the CPU for the work instead. Only the OS thread that holds the CPU, with
    // interrupts enabled, does so; other callers (host threads, standalone tools without
    // OSInit) run the work in place as before.
    class HostDecodeScope {
    public:
        HostDecodeScope() : mReleased(false) {
            const BOOL enabled = OSDisableInterrupts();
            OSRestoreInterrupts(enabled);
            OSThread* bound = PetariNative::Platform::OS::boundThread();
            if (enabled && bound != nullptr && bound == OSGetCurrentThread()) {
                petari_os_begin_host_blocking();
                mReleased = true;
            }
        }

        ~HostDecodeScope() {
            if (mReleased) {
                petari_os_end_host_blocking();
            }
        }

    private:
        bool mReleased;
    };

    PetariNative::Movie::ThpHeader toMovieHeader(const THPHeader& rHeader) {
        PetariNative::Movie::ThpHeader header;
        memcpy(header.magic, rHeader.magic, sizeof(header.magic));
        header.version = rHeader.version;
        header.bufSize = rHeader.bufSize;
        header.audioMaxSamples = rHeader.audioMaxSamples;
        header.frameRate = rHeader.frameRate;
        header.numFrames = rHeader.numFrames;
        header.firstFrameSize = rHeader.firstFrameSize;
        header.movieDataSize = rHeader.movieDataSize;
        header.compInfoDataOffsets = rHeader.compInfoDataOffsets;
        header.offsetDataOffsets = rHeader.offsetDataOffsets;
        header.movieDataOffsets = rHeader.movieDataOffsets;
        header.finalFrameDataOffsets = rHeader.finalFrameDataOffsets;
        return header;
    }
#endif

    void dvdCallBackFunc(s32 a1, DVDFileInfo* pFileInfo) {
        THPSimplePlayerWrapper* player = (THPSimplePlayerWrapper*)pFileInfo->cb.userData;
        player->dvdCallBack(a1);
    }
};  // namespace

THPSimplePlayerWrapper::THPSimplePlayerWrapper(const char* pName) : NerveExecutor(pName) {
    _8 = 0;
    _9 = 0;
    _C = 0;
    _10 = 0;
    mTHPWork = nullptr;
    mOpen = 0;
    mPreFetchState = 0;
    mAudioState = 0;
    mLoop = 0;
    mAudioExist = 0;
    mCurOffset = 0;
    mDvdError = 0;
    mReadProgress = 0;
    mNextDecodeIndex = 0;
    mReadIndex = 0;
    mReadSize = 0;
    mTotalReadFrame = 0;
    mCurrentVolume = 0.0f;
    mTargetVolume = 0.0f;
    mDeltaVolume = 0.0f;
    mRampCount = 0;
    mAudioDecodeIndex = 0;
    mAudioOutputIndex = 0;
    _2F0 = 0;
    _2F4 = 1.0f;
    _2F8 = 0.0f;
    _2FC = 0;
    mSoundBufferIndex = 0;
    _30C = 0;
    _310 = 0;
    mSoundBuffer[0] = new (32) s32[0x230];
    mSoundBuffer[1] = new (32) s32[0x230];
    MR::zeroMemory(mSoundBuffer[0], 0x8C0);
    MR::zeroMemory(mSoundBuffer[1], 0x8C0);
    DCFlushRange(mSoundBuffer[0], 0x8C0);
    DCFlushRange(mSoundBuffer[1], 0x8C0);
    MR::zeroMemory(&mFileInfo, sizeof(mFileInfo));
    MR::zeroMemory(&mHeader, sizeof(mHeader));
    MR::zeroMemory(&mFrameComp, sizeof(mFrameComp));
    MR::zeroMemory(&mVideoInfo, sizeof(mVideoInfo));
    MR::zeroMemory(&mAudioInfo, sizeof(mAudioInfo));
    MR::zeroMemory(mReadBuffer, sizeof(mReadBuffer));
    MR::zeroMemory(&mTextureSet[0], sizeof(mTextureSet[0]));
    MR::zeroMemory(&mTextureSet[1], sizeof(mTextureSet[1]));
    MR::zeroMemory(mAudioBuffer, sizeof(mAudioBuffer));
#ifdef PETARI_NATIVE
    mNativeName = "";
    MR::zeroMemory(&mNativeComponents, sizeof(mNativeComponents));
    MR::zeroMemory(mNativeReadSize, sizeof(mNativeReadSize));
    mNativeCompletion = 0;
    mNativeCompletionPending = false;
    mNativeReadIssued = false;
    nativeResetAv();
    mNativeAvActive = false;
#endif
    initNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeWait));
}

bool THPSimplePlayerWrapper::init(s32 audio) {
    LCEnable();
    if (!THPInit()) {
        return false;
    }

    s32 old = OSDisableInterrupts();
    mSoundBufferIndex = 0;
    initAudio();
    OSRestoreInterrupts(old);
    MR::zeroMemory(mSoundBuffer[0], 0x8C0);
    MR::zeroMemory(mSoundBuffer[1], 0x8C0);
    DCFlushRange(mSoundBuffer[0], 0x8C0);
    DCFlushRange(mSoundBuffer[1], 0x8C0);
    _2F0 = 0;
    _2F4 = 1.0f;
    _2F8 = 0.0f;
    _2FC = 0;
    _8 = 1;
    return true;
}

void THPSimplePlayerWrapper::quit() {
    LCDisable();
    JASDriver::registerMixCallback(nullptr, (JASMixMode)3);
    THPSimplePlayerStaticAudio::mPlayer = nullptr;
    _8 = 0;
}

bool THPSimplePlayerWrapper::open(const char* pName) {
    if (!tryDvdOpen(pName)) {
        return false;
    }

    setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadHeader));
    return true;
}

bool THPSimplePlayerWrapper::close() {
    if (mOpen) {
        if (mPreFetchState == 0) {
            if (mAudioExist) {
                if (mAudioState == 1) {
                    return false;
                }
            } else {
                mAudioState = 0;
            }

            if (!mReadProgress) {
#ifdef PETARI_NATIVE
                nativeReportAv(true);
#endif
                mOpen = 0;
                DVDClose(&mFileInfo);
                return true;
            }
        }
    }

    return false;
}

s32 THPSimplePlayerWrapper::getUseTextureCount() const {
    return 2;
}

u32 THPSimplePlayerWrapper::calcNeedMemory() {
    if (!mOpen) {
        return 0;
    }

    u32 size = OSRoundUp32B(mHeader.bufSize) * 20;
    size += OSRoundUp32B(mVideoInfo.xSize * mVideoInfo.ySize * 2);
    size += OSRoundUp32B(mVideoInfo.xSize * mVideoInfo.ySize >> 1 & 0x7FFFFFFE);
    size += OSRoundUp32B(mVideoInfo.xSize * mVideoInfo.ySize >> 1 & 0x7FFFFFFE);

    if (mAudioExist) {
        size += (OSRoundUp32B(mHeader.audioMaxSamples * 4) * 20);
    }

    size += 0x1000;
    return size;
}

bool THPSimplePlayerWrapper::setBuffer(u8* pBuffer) {
    if (mOpen && !mPreFetchState) {
        if (mAudioState == 1) {
            return false;
        }

        u32 ysize = OSRoundUp32B(mVideoInfo.xSize * mVideoInfo.ySize);
        u32 uvsize = OSRoundUp32B(mVideoInfo.xSize * mVideoInfo.ySize / 4);
        mTextureSet[0].ytexture = pBuffer;
        DCInvalidateRange(pBuffer, ysize);
        pBuffer += ysize;
        mTextureSet[0].utexture = pBuffer;
        DCInvalidateRange(pBuffer, uvsize);
        pBuffer += uvsize;
        mTextureSet[0].vtexture = pBuffer;
        DCInvalidateRange(pBuffer, uvsize);
        pBuffer += uvsize;
        mTextureSet[1].ytexture = pBuffer;
        DCInvalidateRange(pBuffer, ysize);
        pBuffer += ysize;
        mTextureSet[1].utexture = pBuffer;
        DCInvalidateRange(pBuffer, uvsize);
        pBuffer += uvsize;
        mTextureSet[1].vtexture = pBuffer;
        DCInvalidateRange(pBuffer, uvsize);
        pBuffer += uvsize;

        for (s32 i = 0; i < 20; i++) {
            mReadBuffer[i].ptr = pBuffer;
            pBuffer += OSRoundUp32B(mHeader.bufSize);
            mReadBuffer[i].isValid = 0;
        }

        if (mAudioExist) {
            for (s32 i = 0; i < 20; i++) {
                mAudioBuffer[i].buffer = (s16*)pBuffer;
                mAudioBuffer[i].curPtr = (s16*)pBuffer;
                mAudioBuffer[i].validSample = 0;
                pBuffer += OSRoundUp32B(mHeader.audioMaxSamples * 4);
            }
        }

        mTHPWork = (void*)pBuffer;
    }

    return true;
}

bool THPSimplePlayerWrapper::preLoad(s32 loop) {
    if (!mOpen || mPreFetchState) {
        return false;
    }

    mLoop = loop;
#ifdef PETARI_NATIVE
    nativeResetAv();
#endif
    _314 = 20;
    if (!mLoop) {
        if (mHeader.numFrames < 0x14) {
            _314 = mHeader.numFrames;
        }
    }

    setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadPreLoad));
    return true;
}

bool THPSimplePlayerWrapper::loadStop() {
    if (mOpen && !mAudioState) {
#ifdef PETARI_NATIVE
        // DVD callbacks run on the drive thread. Stop further prefetch and read the
        // in-flight state under the interrupt lock, then cancel any read still
        // targeting the movie buffers (including a preload read) before the caller
        // frees them.
        BOOL level = OSDisableInterrupts();
        mPreFetchState = 0;
        bool inFlight = mReadProgress || mNativeReadIssued;
        OSRestoreInterrupts(level);

        if (inFlight) {
            DVDCancel(&mFileInfo.cb);
        }

        level = OSDisableInterrupts();
        mReadProgress = 0;
        mNativeReadIssued = false;
        mNativeCompletionPending = false;
        OSRestoreInterrupts(level);

        if (isPreLoading()) {
            setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeWait));
        }
#else
        mPreFetchState = 0;

        if (mReadProgress) {
            DVDCancel(&mFileInfo.cb);
            mReadProgress = 0;
        }
#endif

        for (s32 i = 0; i < 0x14; i++) {
            mReadBuffer[i].isValid = 0;
        }

        for (s32 i = 0; i < 0x14; i++) {
            mAudioBuffer[i].validSample = 0;
        }

        mTextureSet[0].frameNumber = -1;
        mTextureSet[1].frameNumber = -1;
        mCurOffset = mHeader.movieDataOffsets;
        mReadSize = mHeader.firstFrameSize;
        mReadIndex = 0;
        mTotalReadFrame = 0;
        mDvdError = 0;
        mNextDecodeIndex = 0;
        mAudioDecodeIndex = 0;
        mAudioOutputIndex = 0;
        mCurrentVolume = mTargetVolume;
        mRampCount = 0;
        _310 = 0;
        return true;
    }

    return false;
}

s32 THPSimplePlayerWrapper::decode(s32 audio) {
#ifdef PETARI_NATIVE
    // The drive thread publishes read buffers under the interrupt lock.
    BOOL level = OSDisableInterrupts();
    bool isValid = mReadBuffer[mNextDecodeIndex].isValid == true;
    OSRestoreInterrupts(level);
#else
    bool isValid = mReadBuffer[mNextDecodeIndex].isValid == true;
#endif
    if (isValid) {
#ifdef PETARI_NATIVE
        // Same early returns as below, taken before validation so a frame waiting
        // for audio buffer space is not re-validated on every call.
        if (mAudioExist) {
            if (audio < 0 || audio >= mAudioInfo.sndNumTracks) {
                return 4;
            }

            if (mAudioBuffer[mAudioDecodeIndex].validSample != 0) {
                return 3;
            }
        }

        u32 compSizes[PetariNative::Movie::kThpMaxComponents];
        nativeValidateFrame(mReadBuffer[mNextDecodeIndex].ptr, audio, compSizes);
        u32* compSize = compSizes;
#else
        u32* compSize = (u32*)mReadBuffer[mNextDecodeIndex].ptr + 2;
#endif
        u8* ptr = mReadBuffer[mNextDecodeIndex].ptr + mFrameComp.numComponents * 4 + 8;

        if (mAudioExist) {
            if (audio < 0 || audio >= mAudioInfo.sndNumTracks) {
                return 4;
            }

            if (mAudioBuffer[mAudioDecodeIndex].validSample == 0) {
                for (s32 i = 0; i < mFrameComp.numComponents; i++) {
                    switch (mFrameComp.frameComp[i]) {
                    case 0:
                        if (!videoDecode(ptr)) {
                            return 1;
                        }
                        break;
                    case 1:
                        u32 sample = THPAudioDecode(mAudioBuffer[mAudioDecodeIndex].buffer, ptr + (*compSize) * audio, 0);
                        s32 interrupt = OSDisableInterrupts();
                        mAudioBuffer[mAudioDecodeIndex].validSample = sample;
                        mAudioBuffer[mAudioDecodeIndex].curPtr = mAudioBuffer[mAudioDecodeIndex].buffer;
                        OSRestoreInterrupts(interrupt);
                        mAudioDecodeIndex++;

                        if (mAudioDecodeIndex >= 20) {
                            mAudioDecodeIndex = 0;
                        }

                        break;
                    }

                    ptr += *compSize;
                    compSize++;
                }
            } else {
                return 3;
            }
        } else {
            for (s32 i = 0; i < mFrameComp.numComponents; i++) {
                switch (mFrameComp.frameComp[i]) {
                case 0:
                    if (!videoDecode(ptr)) {
                        return 1;
                    }
                    break;
                }

                ptr += *compSize;
                compSize++;
            }
        }

        mReadBuffer[mNextDecodeIndex].isValid = 0;
        mNextDecodeIndex = getNextBuffer(mNextDecodeIndex);
        checkPrefetch();
        return 0;
    }

    return 2;
}

s32 THPSimplePlayerWrapper::drawCurrentFrame(_GXRenderModeObj* rmode, u32 x, u32 y, u32 polyW, u32 polyH) {
    if (mTextureSet[_310].frameNumber < 0) {
        return -1;
    }

    THPGXYuv2RgbSetup(rmode);
    THPGXYuv2RgbDraw(mTextureSet[_310].ytexture, mTextureSet[_310].utexture, mTextureSet[_310].vtexture, x, y, mVideoInfo.xSize, mVideoInfo.ySize,
                     polyW, polyH);
    THPGXRestore();
    s32 ret = mTextureSet[_310].frameNumber;
    _310 = _310 == 0;
    return ret;
}

bool THPSimplePlayerWrapper::getVideoInfo(THPVideoInfo* pInfo) const {
    if (!mOpen) {
        return false;
    }

    memcpy(pInfo, &mVideoInfo, sizeof(mVideoInfo));
    return true;
}

f32 THPSimplePlayerWrapper::getFrameRate() const {
    if (mOpen) {
        return mHeader.frameRate;
    }

    return 0.0f;
}

s32 THPSimplePlayerWrapper::getTotalFrame() const {
    if (mOpen) {
        return mHeader.numFrames;
    }

    return 0;
}

bool THPSimplePlayerWrapper::videoDecode(u8* pFile) {
#ifdef PETARI_NATIVE
    s32 result;
    {
        HostDecodeScope scope;
        result = THPVideoDecode(pFile, mTextureSet[_310].ytexture, mTextureSet[_310].utexture, mTextureSet[_310].vtexture, mTHPWork);
    }
    if (result) {
        return false;
    }
#else
    if (THPVideoDecode(pFile, mTextureSet[_310].ytexture, mTextureSet[_310].utexture, mTextureSet[_310].vtexture, mTHPWork)) {
        return false;
    }
#endif

    mTextureSet[_310].frameNumber = mReadBuffer[mNextDecodeIndex].frameNumber;
    return true;
}

void THPSimplePlayerWrapper::readFrameAsync() {
    if (!mDvdError && mPreFetchState != 0) {
        if (mTotalReadFrame > mHeader.numFrames - 1) {
            if (mLoop == 1) {
                mTotalReadFrame = 0;
                mCurOffset = mHeader.movieDataOffsets;
                mReadSize = mHeader.firstFrameSize;
            } else {
                return;
            }
        }

#ifdef PETARI_NATIVE
        nativeCheckFrameRead(mCurOffset, mReadSize);
        mNativeReadSize[mReadIndex] = mReadSize;
#endif
        mReadProgress = 1;
        mFileInfo.cb.userData = this;

        if (DVDReadAsyncPrio(&mFileInfo, mReadBuffer[mReadIndex].ptr, mReadSize, mCurOffset, ::dvdCallBackFunc, 2) != 1) {
            mReadProgress = 0;
            mDvdError = 1;
        }
    }

    return;
}

void THPSimplePlayerWrapper::checkPrefetch() {
    BOOL in = OSDisableInterrupts();
    bool isValid = mReadBuffer[mReadIndex].isValid == true;
    if (!isValid && !mReadProgress) {
        readFrameAsync();
    }

    OSRestoreInterrupts(in);
}

void THPSimplePlayerWrapper::dvdCallBack(s32 result) {
    if (result == -1) {
        mDvdError = 1;
        return;
    } else if (result == -3) {
        return;
    }

    mReadProgress = 0;
    mReadBuffer[mReadIndex].frameNumber = mTotalReadFrame;
    mTotalReadFrame++;
    mReadBuffer[mReadIndex].isValid = 1;
    mCurOffset += mReadSize;
#ifdef PETARI_NATIVE
    mReadSize = PetariNative::readU32BE(mReadBuffer[mReadIndex].ptr);
#else
    mReadSize = *(u32*)mReadBuffer[mReadIndex].ptr;
#endif
    int index = mReadIndex;
    index = getNextBuffer(index);
    mReadIndex = index;

    bool valid = mReadBuffer[index].isValid == TRUE;

    if (!valid) {
        readFrameAsync();
    }
}

void THPSimplePlayerWrapper::readAsyncCallBack(s32 a1) {
#ifdef PETARI_NATIVE
    if (a1 < 0) {
        // The original closes the file and leaves the nerve waiting forever.
        nativeFail(a1 == -3 ? "a header or preload read was canceled" : "a header or preload read failed");
    }
#endif
    if (a1 < 0) {
        if (!isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadHeader)) && !isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadFrameComp)) &&
            !isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadVideoComp)) &&
            !isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadAudioComp))) {
            isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadPreLoad));
        }

        DVDClose(&mFileInfo);
        return;
    }

    if (isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadHeader))) {
        endReadHeader();
        setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadFrameComp));
        return;
    }

    if (isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadFrameComp))) {
        _10 = 0;
        endReadFrameComp();
        checkComponentsInFrame(_10);
        return;
    }

    if (isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadVideoComp))) {
        endReadVideoComp();
        if (tryFinishDvdOpen()) {
            return;
        }

        checkComponentsInFrame(_10);
        return;
    }

    if (isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadAudioComp))) {
        endReadAudioComp();
        if (tryFinishDvdOpen()) {
            return;
        }

        checkComponentsInFrame(_10);
        return;
    }

    if (isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadPreLoad))) {
        endReadPreLoadOne();
        setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadPreLoad));
    }
}

s32 THPSimplePlayerWrapper::getNextBuffer(u32 idx) const {
    return ((idx) + 1 >= 0x14 ? 0 : (idx) + 1);
}

bool THPSimplePlayerWrapper::tryDvdOpen(const char* pFileName) {
    if (!_8) {
        return false;
    }

    if (mOpen) {
        return false;
    }

    MR::zeroMemory(&mVideoInfo, sizeof(THPVideoInfo));
    MR::zeroMemory(&mAudioInfo, sizeof(THPAudioInfo));

    if (!DVDOpen(pFileName, &mFileInfo)) {
        return false;
    }

#ifdef PETARI_NATIVE
    mNativeName = pFileName;
    MR::zeroMemory(&mNativeComponents, sizeof(mNativeComponents));
    mNativeCompletionPending = false;
    mNativeReadIssued = false;
#endif

    _C = 0;
    _9 = 0;
    return true;
}

void THPSimplePlayerWrapper::setupParams() {
    mCurOffset = mHeader.movieDataOffsets;
    mReadSize = mHeader.firstFrameSize;
    mReadIndex = 0;
    mTotalReadFrame = 0;
    mDvdError = 0;
    mTextureSet[0].frameNumber = -1;
    mTextureSet[1].frameNumber = -1;
    mNextDecodeIndex = 0;
    mPreFetchState = 0;
    mAudioState = 0;
    mLoop = 0;
    mOpen = 1;
    _310 = 0;
    resetAudioParams();
}

namespace {
    void readAsyncCallBackFunc(s32 a1, DVDFileInfo* pInfo) {
        THPSimplePlayerWrapper* player = (THPSimplePlayerWrapper*)pInfo->cb.userData;
#ifdef PETARI_NATIVE
        // Runs on the drive thread with interrupts disabled. The game thread
        // applies the result in nativePollReadCompletion().
        player->mNativeCompletion = a1;
        player->mNativeCompletionPending = true;
        player->mNativeReadIssued = false;
#else
        player->readAsyncCallBack(a1);
#endif
    }
};  // namespace

void THPSimplePlayerWrapper::exeWait() {
}

void THPSimplePlayerWrapper::exeReadHeader() {
    if (MR::isFirstStep(this)) {
        mFileInfo.cb.userData = this;
#ifdef PETARI_NATIVE
        mNativeReadIssued = true;
#endif
        DVDReadAsyncPrio(&mFileInfo, WorkBuffer, 64, _C, ::readAsyncCallBackFunc, 2);
    }
#ifdef PETARI_NATIVE
    nativePollReadCompletion();
#endif
}

void THPSimplePlayerWrapper::exeReadFrameComp() {
    if (MR::isFirstStep(this)) {
        mFileInfo.cb.userData = this;
        _C = mHeader.compInfoDataOffsets;
#ifdef PETARI_NATIVE
        mNativeReadIssued = true;
#endif
        DVDReadAsyncPrio(&mFileInfo, WorkBuffer, 32, _C, ::readAsyncCallBackFunc, 2);
    }
#ifdef PETARI_NATIVE
    nativePollReadCompletion();
#endif
}

void THPSimplePlayerWrapper::exeReadVideoComp() {
    if (MR::isFirstStep(this)) {
        mFileInfo.cb.userData = this;
#ifdef PETARI_NATIVE
        mNativeReadIssued = true;
#endif
        DVDReadAsyncPrio(&mFileInfo, WorkBuffer, 32, _C, ::readAsyncCallBackFunc, 2);
    }
#ifdef PETARI_NATIVE
    nativePollReadCompletion();
#endif
}

void THPSimplePlayerWrapper::exeReadAudioComp() {
    if (MR::isFirstStep(this)) {
        mFileInfo.cb.userData = this;
#ifdef PETARI_NATIVE
        mNativeReadIssued = true;
#endif
        DVDReadAsyncPrio(&mFileInfo, WorkBuffer, 32, _C, ::readAsyncCallBackFunc, 2);
    }
#ifdef PETARI_NATIVE
    nativePollReadCompletion();
#endif
}

void THPSimplePlayerWrapper::endReadHeader() {
#ifdef PETARI_NATIVE
    PetariNative::Movie::ThpHeader header;
    if (const char* pError = PetariNative::Movie::parseThpHeader(WorkBuffer, 64, &header)) {
        nativeFail(pError);
    }
    memcpy(mHeader.magic, header.magic, sizeof(mHeader.magic));
    mHeader.version = header.version;
    mHeader.bufSize = header.bufSize;
    mHeader.audioMaxSamples = header.audioMaxSamples;
    mHeader.frameRate = header.frameRate;
    mHeader.numFrames = header.numFrames;
    mHeader.firstFrameSize = header.firstFrameSize;
    mHeader.movieDataSize = header.movieDataSize;
    mHeader.compInfoDataOffsets = header.compInfoDataOffsets;
    mHeader.offsetDataOffsets = header.offsetDataOffsets;
    mHeader.movieDataOffsets = header.movieDataOffsets;
    mHeader.finalFrameDataOffsets = header.finalFrameDataOffsets;
#else
    memcpy(&mHeader, WorkBuffer, sizeof(mHeader));
#endif
    if (strcmp(mHeader.magic, "THP")) {
        DVDClose(&mFileInfo);
        return;
    }

    if (mHeader.version != 0x11000) {
        DVDClose(&mFileInfo);
    }
}

void THPSimplePlayerWrapper::endReadFrameComp() {
#ifdef PETARI_NATIVE
    PetariNative::Movie::ThpComponents& components = mNativeComponents;
    if (const char* pError =
            PetariNative::Movie::parseThpFrameCompInfo(WorkBuffer, 32, &components.numComponents, components.kinds)) {
        nativeFail(pError);
    }
    components.byteSize = PetariNative::Movie::kThpFrameCompInfoSize;
    mFrameComp.numComponents = components.numComponents;
    memcpy(mFrameComp.frameComp, components.kinds, sizeof(mFrameComp.frameComp));
#else
    memcpy(&mFrameComp, WorkBuffer, sizeof(mFrameComp));
#endif
    mAudioExist = 0;
    _C += 0x14;
}

void THPSimplePlayerWrapper::endReadVideoComp() {
#ifdef PETARI_NATIVE
    PetariNative::Movie::ThpComponents& components = mNativeComponents;
    if (components.hasVideo) {
        nativeFail("THP has more than one video component");
    }
    if (const char* pError = PetariNative::Movie::parseThpVideoInfo(WorkBuffer, 32, &components.video)) {
        nativeFail(pError);
    }
    components.hasVideo = true;
    components.byteSize += PetariNative::Movie::kThpVideoInfoSize;
    mVideoInfo.xSize = components.video.xSize;
    mVideoInfo.ySize = components.video.ySize;
    mVideoInfo.videoType = components.video.videoType;
#else
    memcpy(&mVideoInfo, WorkBuffer, sizeof(mVideoInfo));
#endif
    _C += 12;
    _10++;
}

void THPSimplePlayerWrapper::endReadAudioComp() {
#ifdef PETARI_NATIVE
    PetariNative::Movie::ThpComponents& components = mNativeComponents;
    if (components.hasAudio) {
        nativeFail("THP has more than one audio component");
    }
    if (const char* pError = PetariNative::Movie::parseThpAudioInfo(WorkBuffer, 32, &components.audio)) {
        nativeFail(pError);
    }
    components.hasAudio = true;
    components.byteSize += PetariNative::Movie::kThpAudioInfoSize;
    mAudioInfo.sndChannels = components.audio.sndChannels;
    mAudioInfo.sndFrequency = components.audio.sndFrequency;
    mAudioInfo.sndNumSamples = components.audio.sndNumSamples;
    mAudioInfo.sndNumTracks = components.audio.sndNumTracks;
#else
    memcpy(&mAudioInfo, WorkBuffer, sizeof(mAudioInfo));
#endif
    mAudioExist = 1;
    _C += 0x10;
    _10++;
}

void THPSimplePlayerWrapper::exeReadPreLoad() {
#ifdef PETARI_NATIVE
    // The original also issues a read on the step after the last preload
    // completes; it lands in read buffer 0 (already holding frame 0) while
    // playback may decode it. Natively only the counted preload reads are issued.
    if (MR::isFirstStep(this) && _314 > 0) {
        nativeCheckFrameRead(mCurOffset, mReadSize);
        mNativeReadSize[mReadIndex] = mReadSize;
        mFileInfo.cb.userData = this;
        mNativeReadIssued = true;
        DVDReadAsyncPrio(&mFileInfo, mReadBuffer[mReadIndex].ptr, mReadSize, mCurOffset, ::readAsyncCallBackFunc, 2);
    }

    nativePollReadCompletion();
#else
    if (MR::isFirstStep(this)) {
        DVDReadAsyncPrio(&mFileInfo, mReadBuffer[mReadIndex].ptr, mReadSize, mCurOffset, ::readAsyncCallBackFunc, 2);
    }
#endif

    if (!_314) {
        mPreFetchState = 1;
        setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeWait));
    }
}

void THPSimplePlayerWrapper::endReadPreLoadOne() {
    mCurOffset += mReadSize;
#ifdef PETARI_NATIVE
    mReadSize = PetariNative::readU32BE(mReadBuffer[mReadIndex].ptr);
#else
    mReadSize = *(s32*)mReadBuffer[mReadIndex].ptr;
#endif
    mReadBuffer[mReadIndex].isValid = 1;
    mReadBuffer[mReadIndex].frameNumber = mTotalReadFrame;
    mReadIndex = getNextBuffer(mReadIndex);
    mTotalReadFrame++;

    if (mTotalReadFrame > mHeader.numFrames - 1 && mLoop == 1) {
        mTotalReadFrame = 0;
        mCurOffset = mHeader.movieDataOffsets;
        mReadSize = mHeader.firstFrameSize;
    }

    _314--;
}

bool THPSimplePlayerWrapper::checkComponentsInFrame(s32 comp) {
    if (comp < 0 || mFrameComp.numComponents <= comp) {
        return false;
    }

    switch (mFrameComp.frameComp[comp]) {
    case 0:
        setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadVideoComp));
        return true;
    case 1:
        setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadAudioComp));
        return true;
    }

    return false;
}

bool THPSimplePlayerWrapper::tryFinishDvdOpen() {
    if (_10 < mFrameComp.numComponents) {
        return false;
    }

#ifdef PETARI_NATIVE
    const PetariNative::Movie::ThpHeader header = toMovieHeader(mHeader);
    if (const char* pError = PetariNative::Movie::validateThpStream(header, mNativeComponents, mFileInfo.length)) {
        nativeFail(pError);
    }
    // mixAudio() plays decoded samples at the 32 kHz DAC rate without resampling.
    if (mNativeComponents.hasAudio && mNativeComponents.audio.sndFrequency != 32000) {
        nativeFail("THP audio is not 32000 Hz; the simple player does not resample");
    }
#endif

    setupParams();
    _9 = 1;
    setNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeWait));
    return true;
}

void THPSimplePlayerWrapper::initAudio() {
    if (!SCGetSoundMode()) {
        _30C = 1;
    } else {
        _30C = 0;
    }

    THPSimplePlayerStaticAudio::mPlayer = this;
    JASDriver::registerMixCallback(THPSimplePlayerStaticAudio::audioCallback, (JASMixMode)3);
}

bool THPSimplePlayerWrapper::isAudioProcessValid() {
    if (mOpen && mAudioExist) {
        return true;
    }

    return false;
}

s16* THPSimplePlayerWrapper::audioCallback(s32 sample) {
    if (!isAudioProcessValid()) {
        return nullptr;
    }

    s32 en = OSEnableInterrupts();
    mSoundBufferIndex ^= 1;
    mixAudio((s16*)mSoundBuffer[mSoundBufferIndex], sample);
    OSRestoreInterrupts(en);
    return (s16*)mSoundBuffer[mSoundBufferIndex];
}

void THPSimplePlayerWrapper::mixAudio(s16* pDest, u32 sample) {
    u32 sampleNum, requestSample;
    s32 mix;
    s16 *libsrc, *thpsrc;
    u16 attenuation;

    if (isAudioProcessValid()) {
        if (_2F0) {
            _2F0 = 0;
            _2F4 = 0.0f;
            _2F8 = 0.001f;
            _2FC = 4800;

            s32 idx1 = (mAudioOutputIndex + 1) % 0x14;
            s32 idx2 = (mAudioOutputIndex + 2) % 0x14;
            if (!mAudioBuffer[idx1].validSample || !mAudioBuffer[idx2].validSample) {
                MR::zeroMemory(pDest, sample * THP_STEREO_SAMPLE_BYTES);
                return;
            }
        } else if (!mAudioBuffer[(mAudioOutputIndex + 1) % 0x14].validSample) {
#ifdef PETARI_NATIVE
            mNativeAvSilence += sample;
#endif
            MR::zeroMemory(pDest, sample * THP_STEREO_SAMPLE_BYTES);
            return;
        }

        do {
            if (mAudioBuffer[mAudioOutputIndex].validSample) {
                if (mAudioBuffer[mAudioOutputIndex].validSample >= sample) {
                    sampleNum = sample;
                } else {
                    sampleNum = mAudioBuffer[mAudioOutputIndex].validSample;
                }

                thpsrc = mAudioBuffer[mAudioOutputIndex].curPtr;

                for (u32 i = 0; i < sampleNum; i++) {
                    if (mRampCount) {
                        mRampCount--;
                        mCurrentVolume += mDeltaVolume;
                    } else {
                        mCurrentVolume = mTargetVolume;
                    }

                    f32 vol = mCurrentVolume;

                    if (_2FC > 0) {
                        s32 v19 = _2FC - 1;
                        vol = 0.0f;
                        _2FC = v19;
                        if (v19 < 0) {
                            _2FC = 0;
                        }
                    } else if (_2F4 < 1.0f) {
                        vol = mCurrentVolume * _2F4;
                        f32 v20 = _2F4 + _2F8;
                        _2F4 += _2F8;

                        if (v20 >= 1.0f) {
                            _2F4 = 1.0f;
                            _2F8 = 0.0f;
                        }
                    }

                    attenuation = VolumeTable[(s32)vol];
                    mix = ((attenuation * (*thpsrc)) >> 15);

                    if (mix < -32768) {
                        mix = -32768;
                    }

                    if (mix > 32767) {
                        mix = 32767;
                    }

                    *pDest = (s16)mix;
                    *thpsrc = 0;
                    pDest++;
                    thpsrc++;

                    mix = ((attenuation * (*thpsrc)) >> 15);

                    if (mix < -32768) {
                        mix = -32768;
                    }
                    if (mix > 32767) {
                        mix = 32767;
                    }

                    *pDest = (s16)mix;
                    *thpsrc = 0;
                    pDest++;
                    thpsrc++;

                    if (_30C) {
                        s32 diff = *(pDest - 2) + (*(pDest - 1));
                        f32 v16 = (diff / 2);
                        *(pDest - 1) = *(pDest - 2) = 0.707f * v16;
                    }
                }

#ifdef PETARI_NATIVE
                mNativeAvAudioSamples += sampleNum;
#endif
                sample -= sampleNum;
                mAudioBuffer[mAudioOutputIndex].validSample -= sampleNum;
                mAudioBuffer[mAudioOutputIndex].curPtr = thpsrc;

                if (mAudioBuffer[mAudioOutputIndex].validSample == 0) {
                    mAudioOutputIndex++;
                    if (mAudioOutputIndex >= 0x14) {
                        mAudioOutputIndex = 0;
                    }
                }
                if (sample == 0)
                    break;
            } else {
#ifdef PETARI_NATIVE
                mNativeAvSilence += sample;
#endif
                MR::zeroMemory(pDest, sample * THP_STEREO_SAMPLE_BYTES);
                return;
            }
        } while (true);

    } else {
        MR::zeroMemory(pDest, sample * THP_STEREO_SAMPLE_BYTES);
    }
}

void THPSimplePlayerWrapper::resetAudioParams() {
    mAudioDecodeIndex = 0;
    mAudioOutputIndex = 0;
    MR::zeroMemory(mAudioBuffer, sizeof(mAudioBuffer));

    for (s32 i = 0; i < 20; i++) {
        mAudioBuffer[i].validSample = 0;
    }

    mRampCount = 0;
    mCurrentVolume = 127.0f;
    mTargetVolume = 127.0f;
}

bool THPSimplePlayerWrapper::setVolume(s32 volume, s32 time) {
    if (!isAudioProcessValid()) {
        return false;
    }

    if (volume > 127) {
        volume = 127;
    }

    if (volume < 0) {
        volume = 0;
    }

    if (time > 60000) {
        time = 60000;
    }

    if (time < 0) {
        time = 0;
    }

    s32 en = OSEnableInterrupts();
    mTargetVolume = (f32)volume;

    if (time > 0) {
        mRampCount = 32 * time;
        mDeltaVolume = (mTargetVolume - mCurrentVolume) / (f32)mRampCount;
    } else {
        mRampCount = 0;
        mCurrentVolume = mTargetVolume;
    }

    OSRestoreInterrupts(en);
    return true;
}

bool THPSimplePlayerWrapper::isPreLoading() const {
    return isNerve(GET_NERVE(THPSimplePlayerWrapper, HostTypeReadPreLoad));
}

void THPSimplePlayerWrapper::setUnpauseFrameFlag() {
    s32 en = OSDisableInterrupts();
    _2F0 = 1;
    OSRestoreInterrupts(en);
}

#ifdef PETARI_NATIVE
void THPSimplePlayerWrapper::nativeFail(const char* pReason, s32 frame) const {
    // A damaged or unsupported movie must not play as garbage or be skipped.
    if (frame >= 0) {
        OSPanic(__FILE__, __LINE__, "THP movie %s, frame %d: %s", mNativeName, frame, pReason);
    } else {
        OSPanic(__FILE__, __LINE__, "THP movie %s: %s", mNativeName, pReason);
    }
    std::abort();
}

void THPSimplePlayerWrapper::nativePollReadCompletion() {
    BOOL level = OSDisableInterrupts();
    bool pending = mNativeCompletionPending;
    s32 result = mNativeCompletion;
    mNativeCompletionPending = false;
    OSRestoreInterrupts(level);

    if (pending) {
        readAsyncCallBack(result);
    }
}

void THPSimplePlayerWrapper::nativeCheckFrameRead(u32 offset, s32 size) const {
    if (size < 0) {
        nativeFail("THP frame size is negative", mTotalReadFrame);
    }
    if (const char* pError = PetariNative::Movie::checkThpFrameRead(toMovieHeader(mHeader), mNativeComponents,
                                                                    mFileInfo.length, offset, size)) {
        nativeFail(pError, mTotalReadFrame);
    }
}

void THPSimplePlayerWrapper::nativeValidateFrame(const u8* pFrame, s32 audio, u32* pCompSizes) {
    const PetariNative::Movie::ThpComponents& components = mNativeComponents;
    const s32 frame = mReadBuffer[mNextDecodeIndex].frameNumber;
    PetariNative::Movie::ThpFrame layout;
    if (const char* pError = PetariNative::Movie::parseThpFrame(pFrame, mNativeReadSize[mNextDecodeIndex], components,
                                                                &layout)) {
        nativeFail(pError, frame);
    }

    for (u32 i = 0; i < components.numComponents; i++) {
        const u8* pComp = pFrame + layout.componentOffsets[i];
        const u32 size = layout.componentSizes[i];
        const char* pError = nullptr;
        pCompSizes[i] = size;

        if (components.kinds[i] == PetariNative::Movie::kThpComponentVideo) {
            HostDecodeScope scope;
            pError = PetariNative::Movie::validateThpVideoComponent(pComp, size, components.video.xSize,
                                                                    components.video.ySize);
        } else if (audio >= 0 && static_cast< u32 >(audio) < components.audio.sndNumTracks) {
            u32 samples;
            pError = PetariNative::Movie::validateThpAudioComponent(pComp + size * audio, size, components.audio,
                                                                    mHeader.audioMaxSamples, &samples);
        }

        if (pError != nullptr) {
            nativeFail(pError, frame);
        }
    }
}
#endif

s16* THPSimplePlayerStaticAudio::audioCallback(s32 audio) {
    THPSimplePlayerWrapper* player = THPSimplePlayerStaticAudio::mPlayer;
    if (player == nullptr) {
        return nullptr;
    }

    return player->audioCallback(audio);
}

THPSimplePlayerStaticAudio::THPSimplePlayerStaticAudio() {
}

#ifdef PETARI_NATIVE
namespace {
    bool isAudioDiag() {
        static const bool enabled = [] {
            const char* pValue = std::getenv("PETARI_AUDIO_DIAG");
            return pValue != nullptr && pValue[0] != '\0' && pValue[0] != '0';
        }();
        return enabled;
    }
}  // namespace

void THPSimplePlayerWrapper::nativeResetAv() {
    mNativeAvVideoFrames = mNativeAvAudioWaits = mNativeAvAudioSamples = mNativeAvSilence = 0;
    mNativeAvLastVideo = mNativeAvLastAudio = mNativeAvLastSilence = mNativeAvLastWaits = 0;
    mNativeAvStart = mNativeAvLast = 0;
    mNativeAvActive = true;
}

void THPSimplePlayerWrapper::nativeNoteDecode(s32 result) {
    if (!isAudioDiag() || !mNativeAvActive) {
        return;
    }
    if (result == 0) {
        mNativeAvVideoFrames++;
    } else if (result == 3) {
        mNativeAvAudioWaits++;
    }
    const OSTime now = OSGetTime();
    if (mNativeAvStart == 0) {
        mNativeAvStart = mNativeAvLast = now;
    } else if (OSTicksToMilliseconds(now - mNativeAvLast) >= 1000) {
        nativeReportAv(false);
    }
}

void THPSimplePlayerWrapper::nativeReportAv(bool final) {
    if (!isAudioDiag() || !mNativeAvActive || mNativeAvStart == 0) {
        return;
    }
    const OSTime now = OSGetTime();
    const f32 frameRate = getFrameRate();
    const f64 videoSeconds = frameRate > 0.0f ? mNativeAvVideoFrames / static_cast< f64 >(frameRate) : 0.0;
    const u32 rate = mAudioExist ? mAudioInfo.sndFrequency : 0;
    const f64 audioSeconds = rate != 0 ? mNativeAvAudioSamples / static_cast< f64 >(rate) : 0.0;
    s32 queued = 0;
    for (s32 i = 0; i < 20; i++) {
        queued += mAudioBuffer[i].validSample != 0;
    }
    OSReport("[movie-av] %s %s at %.2f s: video %u frames (+%u) = %.3f s; audio %u samples (+%u) = %.3f s; drift %+.1f ms; "
             "queued audio %d frames; decode waits for audio +%u; silence +%u samples\n",
             mNativeName != nullptr ? mNativeName : "?", final ? "end" : "play", OSTicksToMilliseconds(now - mNativeAvStart) / 1000.0,
             mNativeAvVideoFrames, mNativeAvVideoFrames - mNativeAvLastVideo, videoSeconds, mNativeAvAudioSamples,
             mNativeAvAudioSamples - mNativeAvLastAudio, audioSeconds, rate != 0 ? (videoSeconds - audioSeconds) * 1000.0 : 0.0, queued,
             mNativeAvAudioWaits - mNativeAvLastWaits, mNativeAvSilence - mNativeAvLastSilence);
    mNativeAvLast = now;
    mNativeAvLastVideo = mNativeAvVideoFrames;
    mNativeAvLastAudio = mNativeAvAudioSamples;
    mNativeAvLastSilence = mNativeAvSilence;
    mNativeAvLastWaits = mNativeAvAudioWaits;
    if (final) {
        mNativeAvActive = false;
    }
}
#endif

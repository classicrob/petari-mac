#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRAramPiece.hpp>
#include <JSystem/JKernel/JKRAramHeap.hpp>
namespace { s32 sAramThreadStackSize = 0xC000; s32 sAramThreadMsgSize = 0x20; }

void JKRAramPiece::startDMA(JKRAMCommand* pCommand) {
#ifdef PETARI_NATIVE
    // The Wii screening below tests Wii address ranges and completes every
    // ARAM-to-main (direction 1) request without copying. Natively main memory is
    // host pointers; ARStartDMA validates direction, pointers, and ARAM bounds, and
    // aborts on invalid transfers instead of reporting them done.
    ARStartDMA(pCommand->mTransferDirection, pCommand->mSrc, pCommand->mDst, pCommand->mDataLength);
    doneDMA(reinterpret_cast< uintptr_t >(pCommand));
    return;
#endif
    if (pCommand->mSrc < 0x80000000) {
        doneDMA(reinterpret_cast< uintptr_t >(pCommand));
        return;
    }

    if (pCommand->mDst >= 0x04000000) {
        doneDMA(reinterpret_cast< uintptr_t >(pCommand));
        return;
    }

    if (pCommand->mDataLength > 0x00E00000) {
        doneDMA(reinterpret_cast< uintptr_t >(pCommand));
        return;
    }

    ARStartDMA(pCommand->mTransferDirection, pCommand->mSrc, pCommand->mDst, pCommand->mDataLength);
    doneDMA(reinterpret_cast< uintptr_t >(pCommand));
}

JKRAram::JKRAram(u32 audioSize, u32 graphSize, s32 priority) : JKRThread(::sAramThreadStackSize, ::sAramThreadMsgSize, priority) {
    u32 reserved = ARInit(mStackArray, 3);
    ARQInit();
    u32 total = ARGetSize();
    mAudioMemorySize = audioSize;
    if (graphSize == 0xFFFFFFFF) {
        mGraphMemorySize = total - audioSize - reserved;
        mAramMemorySize = 0;
    } else {
        mGraphMemorySize = graphSize;
        mAramMemorySize = total - (audioSize + graphSize) - reserved;
    }

    mAudioMemoryPtr = ARAlloc(mAudioMemorySize);
    mGraphMemoryPtr = ARAlloc(mGraphMemorySize);
    if (mAramMemorySize != 0) {
        mAramMemoryPtr = ARAlloc(mAramMemorySize);
    } else {
        mAramMemoryPtr = 0;
    }

    mAramHeap = new (JKRGetSystemHeap(), 0) JKRAramHeap(mGraphMemoryPtr, mGraphMemorySize);
}

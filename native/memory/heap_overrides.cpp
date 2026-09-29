#include <JSystem/JKernel/JKRExpHeap.hpp>

s32 JKRExpHeap::adjustSize() {
    CMemBlock* pNext;
    JKRHeap* pParent = mChildTree.getParent()->getObject();
    if (pParent == nullptr) {
        return -1;
    }

    lock();
    u8* pEnd = mStart;
    for (CMemBlock* pBlock = mHeadUsedList; pBlock != nullptr; pBlock = pBlock->mNext) {
        u8* pBlockEnd = reinterpret_cast< u8* >(pBlock + 1) + pBlock->mSize;
        if (pBlockEnd > pEnd) {
            pEnd = pBlockEnd;
        }
    }

    if (pEnd == mEnd) {
        unlock();
        return -1;
    }

    if (pParent->getHeapType() != 'EXPH') {
        unlock();
        return -1;
    }

    for (CMemBlock* pBlock = mHeadFreeList; pBlock != nullptr; pBlock = pNext) {
        pNext = pBlock->mNext;
        if (reinterpret_cast< u8* >(pBlock) >= pEnd) {
            if (pNext != nullptr) {
                pNext = pNext->mNext;
            }

            removeFreeBlock(pBlock);
        }
    }

    if (mHeadFreeList == nullptr) {
        CMemBlock* pBlock = reinterpret_cast< CMemBlock* >(pEnd);
        pBlock->initiate(nullptr, nullptr, 0, 0, 0);
        mHeadFreeList = pBlock;
        mTailFreeList = pBlock;
        pEnd += sizeof(CMemBlock);
    }

    u32 size = pEnd - reinterpret_cast< u8* >(this);
    pParent->resize(this, size);
    mEnd = pEnd;
    mSize = pEnd - mStart;
    unlock();
    return size;
}

u32 JKRHeap::getMaxAllocatableSize(int alignment) {
#ifdef PETARI_NATIVE
    u32 address = reinterpret_cast< uintptr_t >(getMaxFreeBlock()) & 0xF;
#else
    u32 address = reinterpret_cast< u32 >(getMaxFreeBlock());
#endif
    return ~(alignment - 1) & (getFreeSize() - ((alignment - 1) & (alignment - (address & 0xF))));
}

#include <JSystem/JKernel/JKRUnitHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
namespace { const u8 sUnitMask[] = {0x80,0x40,0x20,0x10,8,4,2,1}; }

void* JKRUnitHeap::do_alloc(u32 size, int alignment) {
    u32 bit;
    u8* pBat;
    s32 index;
    lock();
    void* pResult = nullptr;

    if (size <= mUnitSize) {
        index = find1FreeBlock(alignment);
        if (index >= 0) {
            setUnitUsed(index);
            mTotalFreeSize -= mUnitSize;
            pResult = indexToAddress(index);
        }
    } else {
        u32 count = (size + mUnitSize - 1) / mUnitSize;
        index = findFreeBlock(alignment, count);
        if (index >= 0) {
            pBat = mBat + index / 8;
            for (s32 i = index; i < index + count; i++) {
                bit = i & 7;
                if (bit == 0) {
                    pBat = mBat + i / 8;
                }

                *pBat |= ::sUnitMask[bit];
            }

            mTotalFreeSize -= mUnitSize * count;
            pResult = indexToAddress(index);
        }
    }

    unlock();
    return pResult;
}

void JKRUnitHeap::do_free(void* pMemory) {
    lock();
    s32 index = addressToIndex(pMemory);
    if (index >= 0) {
        u32 value = mBat[index / 8];
        u32 cleared = value & (::sUnitMask[index & 7] ^ 0xFF);
        mBat[index / 8] = cleared;
        if (value != cleared) {
            mTotalFreeSize += mUnitSize;
        }
    }

    unlock();
}

void JKRSolidHeap::do_free(void* pMemory) {
}

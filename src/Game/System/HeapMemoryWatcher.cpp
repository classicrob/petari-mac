#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <revolution/os.h>
#include <revolution/wpad.h>

JKRExpHeap* HeapMemoryWatcher::sRootHeapGDDR3;

namespace {
    JKRExpHeap* createExpHeap(u32 size, JKRHeap* pHeap, bool a3) NO_INLINE {
        JKRExpHeap* heap;

        if (a3) {
            void* data = new (pHeap, -4) u8[size];
            heap = JKRExpHeap::create(data, size, pHeap, true);
        } else {
            heap = JKRExpHeap::create(size, pHeap, true);
        }

        if (MR::isEqualCurrentHeap(heap)) {
            JKRHeap::sRootHeap->becomeCurrentHeap();
        }

        return heap;
    }

    JKRSolidHeap* createSolidHeap(u32 size, JKRHeap* pHeap) {
        JKRSolidHeap* heap = JKRSolidHeap::create(size, pHeap, true);

        if (MR::isEqualCurrentHeap(heap)) {
            JKRHeap::sRootHeap->becomeCurrentHeap();
        }

        return heap;
    }

    void destroyHeapAndSetNULL(JKRHeap** pHeap) {
        if (*pHeap != nullptr) {
            JKRHeap::destroy(*pHeap);
            *pHeap = nullptr;
        }
    }
};  // namespace

JKRHeap* HeapMemoryWatcher::getHeapNapa(const JKRHeap* pHeap) {
    if (pHeap == mStationedHeapNapa || pHeap == mStationedHeapGDDR) {
        return mStationedHeapNapa;
    }

    if (pHeap == mSceneHeapNapa || pHeap == mSceneHeapGDDR) {
        return mSceneHeapNapa;
    }

    return nullptr;
}

JKRHeap* HeapMemoryWatcher::getHeapGDDR3(const JKRHeap* pHeap) {
    if (pHeap == mStationedHeapNapa || pHeap == mStationedHeapGDDR) {
        return mStationedHeapGDDR;
    }

    if (pHeap == mSceneHeapNapa || pHeap == mSceneHeapGDDR) {
        return mSceneHeapGDDR;
    }

    return nullptr;
}

void HeapMemoryWatcher::createFileCacheHeapOnGameHeap(u32 size) {
    mFileCacheHeap = ::createSolidHeap(size, mGameHeapGDDR);
}

void HeapMemoryWatcher::createSceneHeapOnGameHeap() {
    mSceneHeapNapa = ::createSolidHeap(-1, mGameHeapNapa);
    mSceneHeapGDDR = ::createSolidHeap(-1, mGameHeapGDDR);
}

void HeapMemoryWatcher::adjustStationedHeaps() {
    MR::adjustHeapSize(mStationedHeapNapa, 0);
    MR::adjustHeapSize(mStationedHeapGDDR, 0);
}

void HeapMemoryWatcher::setCurrentHeapToStationedHeap() {
    MR::becomeCurrentHeap(mStationedHeapNapa);
}

void HeapMemoryWatcher::setCurrentHeapToGameHeap() {
    MR::becomeCurrentHeap(JKRHeap::sSystemHeap);
}

void HeapMemoryWatcher::setCurrentHeapToSceneHeap() {
    MR::becomeCurrentHeap(mSceneHeapNapa);
}

void HeapMemoryWatcher::destroySceneHeap() {
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapNapa));
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapGDDR));
}

void HeapMemoryWatcher::destroyGameHeap() {
    if (mSceneHeapNapa != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapNapa));
    }

    if (mSceneHeapGDDR != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mSceneHeapGDDR));
    }

    if (mFileCacheHeap != nullptr) {
        ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mFileCacheHeap));
    }

    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mGameHeapNapa));
    ::destroyHeapAndSetNULL(reinterpret_cast< JKRHeap** >(&mGameHeapGDDR));
    createGameHeap();
}

void HeapMemoryWatcher::createRootHeap() {
    JKRExpHeap* pHeap;
    void* newHi;
#ifdef PETARI_NATIVE
    uintptr_t arenaHi, arenaLo;

    JKRExpHeap::createRoot(1, true);
    arenaLo = reinterpret_cast< uintptr_t >(OSGetMEM2ArenaLo());
    arenaHi = reinterpret_cast< uintptr_t >(OSGetMEM2ArenaHi());
    // The start of MEM2 becomes ARAM (RVL "alternate ARAM"). The audio ARAM heap
    // occupies offsets ARGetBaseAddress() (0x4000) .. 0x4000 + 0xE00000, so reserve
    // that whole range. The Wii reserved only 0xE00000 bytes, which let the top
    // 16 KiB of audio ARAM overlap the GDDR3 root heap and made JKRAram's
    // graph-memory size wrap to 0xFFFFC000; natively it is exactly 0.
    newHi = reinterpret_cast< void* >(arenaLo + 0x4000 + 0xE00000);
    OSSetMEM2ArenaHi(newHi);
    JKRHeap::setAltAramStartAdr(arenaLo);
    pHeap = JKRExpHeap::create(newHi, arenaHi - reinterpret_cast< uintptr_t >(newHi), JKRHeap::sRootHeap, true);
#else
    u32 arenaHi, arenaLo;

    JKRExpHeap::createRoot(1, true);
    arenaLo = reinterpret_cast< u32 >(OSGetMEM2ArenaLo());
    arenaHi = reinterpret_cast< u32 >(OSGetMEM2ArenaHi());
    newHi = reinterpret_cast< void* >(arenaLo + 0xE00000);
    OSSetMEM2ArenaHi(newHi);
    JKRHeap::setAltAramStartAdr(arenaLo);
    pHeap = JKRExpHeap::create(newHi, arenaHi - reinterpret_cast< u32 >(newHi), JKRHeap::sRootHeap, true);
#endif

    if (MR::isEqualCurrentHeap(pHeap)) {
        JKRHeap::sRootHeap->becomeCurrentHeap();
    }

    HeapMemoryWatcher::sRootHeapGDDR3 = pHeap;
}

void HeapMemoryWatcher::createHeaps() {
    MR::CurrentHeapRestorer heapRestorer = MR::CurrentHeapRestorer(JKRHeap::sRootHeap);
    ::createExpHeap(0x40000, JKRHeap::sRootHeap, false)->becomeSystemHeap();
#ifdef PETARI_NATIVE
    // Measured by the AudioLib audit: native audio objects need about 280 KB more than
    // the Wii's 0x1E0000, which left about 250 KB free.
    mAudSystemHeap = ::createSolidHeap(0x300000, JKRHeap::sRootHeap);
#else
    mAudSystemHeap = ::createSolidHeap(0x1E0000, JKRHeap::sRootHeap);
#endif
#ifdef PETARI_NATIVE
    // Stationed resources get native-only memory on top of the Wii 0x900000: same-size host
    // images of every J3D file and of the particle container, plus larger native objects.
    // Measured on the disc with the native loaders: player J3D loading 3.23 MiB, other NAPA
    // J3D about 0.2 MiB, particles (created here by initAfterStationedResourceLoaded)
    // 4.36 MiB; at most 7.8 MiB more than the Wii, of which at least 5.2 MiB host images.
    // adjustStationedHeaps() trims the unused tail once loading ends.
    mStationedHeapNapa = ::createExpHeap(0x900000 + 0x900000, JKRHeap::sRootHeap, false);
#else
    mStationedHeapNapa = ::createExpHeap(0x900000, JKRHeap::sRootHeap, false);
#endif
    JKRHeap* pRootHeapGDDR = HeapMemoryWatcher::sRootHeapGDDR3;
#ifdef PETARI_NATIVE
    // Wii 0xD0 is the 0x90 heap object, one 0x10 block header and 0x30 usable bytes.
    // The native heap object is larger; keep the same usable bytes.
    u32 wpadHeapSize = OSRoundUp32B(WPADGetWorkMemorySize()) + ALIGN_NEXT(sizeof(JKRExpHeap), 0x10) + sizeof(JKRExpHeap::CMemBlock) + 0x30;
#else
    u32 wpadHeapSize = OSRoundUp32B(WPADGetWorkMemorySize()) + 0xD0;
#endif
    mWPadHeap = ::createExpHeap(wpadHeapSize, pRootHeapGDDR, false);
    mHomeButtonLayoutHeap = ::createExpHeap(0x80000, HeapMemoryWatcher::sRootHeapGDDR3, false);
#ifdef PETARI_NATIVE
    // Measured on the disc: native J3D loading of the GDDR stationed archives uses 7.55 MiB,
    // of which 6.49 MiB are host images the Wii never allocates; the rest bounds native
    // object growth. The extra 0xA00000 covers that plus margin for unmeasured native growth
    // (layouts, tables). adjustStationedHeaps() trims the unused tail once loading ends.
    mStationedHeapGDDR = ::createExpHeap(0x1400000 + 0xA00000, HeapMemoryWatcher::sRootHeapGDDR3, false);
#else
    mStationedHeapGDDR = ::createExpHeap(0x1400000, HeapMemoryWatcher::sRootHeapGDDR3, false);
#endif
    createGameHeap();
}

void HeapMemoryWatcher::createGameHeap() {
    mGameHeapNapa = ::createExpHeap(-1, JKRHeap::sRootHeap, false);
    mGameHeapGDDR = ::createExpHeap(-1, HeapMemoryWatcher::sRootHeapGDDR3, false);
}

HeapMemoryWatcher::HeapMemoryWatcher()
    : mStationedHeapNapa(nullptr), mStationedHeapGDDR(nullptr), mGameHeapNapa(nullptr), mGameHeapGDDR(nullptr), mFileCacheHeap(nullptr),
      mSceneHeapNapa(nullptr), mSceneHeapGDDR(nullptr), mWPadHeap(nullptr), mHomeButtonLayoutHeap(nullptr), mAudSystemHeap(nullptr) {
    JKRHeap::setErrorHandler(HeapMemoryWatcher::memoryErrorCallback);
    createHeaps();
}

#ifdef PETARI_NATIVE
void HeapMemoryWatcher::memoryErrorCallback(void* heap, u32 bytes, int alignment) {
    JKRHeap* allocator = static_cast<JKRHeap*>(heap);
    OSPanic(__FILE__, __LINE__, "Native heap allocation failed: heap=%p, requested=%u, alignment=%d, size=%u, free=%d, max=%d",
            heap, bytes, alignment,
            static_cast< u32 >(static_cast< u8* >(allocator->getEndAddr()) - static_cast< u8* >(allocator->getStartAddr())), allocator->getTotalFreeSize(), allocator->getFreeSize());
}
#else
void HeapMemoryWatcher::memoryErrorCallback(void*, u32, int) {
    OSPanic(__FILE__, 537, "");
}
#endif

void HeapMemoryWatcher::checkRestMemory() {
}

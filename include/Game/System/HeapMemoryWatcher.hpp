#pragma once

#include "Inline.hpp"
#include <revolution/types.h>

class JKRExpHeap;
class JKRHeap;
class JKRSolidHeap;

class HeapMemoryWatcher {
public:
    HeapMemoryWatcher();

    JKRSolidHeap* getAudSystemHeap() const {
        return mAudSystemHeap;
    }

    JKRHeap* getHeapNapa(const JKRHeap*);
    JKRHeap* getHeapGDDR3(const JKRHeap*);
    void createFileCacheHeapOnGameHeap(u32);
    void createSceneHeapOnGameHeap();
    void adjustStationedHeaps();
    void setCurrentHeapToStationedHeap();
    void setCurrentHeapToGameHeap();
    void setCurrentHeapToSceneHeap();
    void destroySceneHeap();
    void destroyGameHeap();
    static void createRootHeap();
    void createHeaps();
    void createGameHeap();
    static void memoryErrorCallback(void*, u32, int);
    void checkRestMemory();

    // used in GameSystemObjHolder::initAudio()
    JKRSolidHeap* getAudSystemHeap() {
        return mAudSystemHeap;
    }

    /* 0x00 */ JKRExpHeap* mStationedHeapNapa;
    /* 0x04 */ JKRExpHeap* mStationedHeapGDDR;
    /* 0x08 */ JKRExpHeap* mGameHeapNapa;
    /* 0x0C */ JKRExpHeap* mGameHeapGDDR;
    /* 0x10 */ JKRHeap* mFileCacheHeap;
    /* 0x14 */ JKRSolidHeap* mSceneHeapNapa;
    /* 0x18 */ JKRSolidHeap* mSceneHeapGDDR;
    /* 0x1C */ JKRExpHeap* mWPadHeap;
    /* 0x20 */ JKRExpHeap* mHomeButtonLayoutHeap;
    /* 0x24 */ JKRSolidHeap* mAudSystemHeap;
#ifdef PETARI_NATIVE
    // Host-layout copies of big-endian J3D files whose archive is in mFileCacheHeap (see
    // HeapMemoryWatcher.cpp). Created and destroyed with mFileCacheHeap.
    JKRSolidHeap* mFileCacheHostImageHeap;

    // Heap headroom diagnostics (PETARI_TRACE_BOOT): FileLoaderThread reports each mounted
    // archive; the archive bytes resident in the file cache since it was created separate the
    // Wii's share of its use (the same archive bytes) from native growth.
    void noteArchiveMounted(JKRHeap* pHeap, const char* pName, const void* pData);
    u32 mFileCacheArchiveBytes;
    u32 mFileCacheArchiveCount;
#endif
    static JKRExpHeap* sRootHeapGDDR3;
};

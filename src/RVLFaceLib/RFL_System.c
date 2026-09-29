#include "RVLFaceLibInternal.h"
#ifdef PETARI_NATIVE
#include "rfl_native.h"
#endif
#if defined(PETARI_NATIVE)
#include <stdio.h>
#else
#include <cstdio>
#endif
#include <revolution/mem.h>
#include <revolution/types.h>

#ifdef PETARI_NATIVE
// The Wii's system heap fits its nine fixed allocations (the database,
// controller buffers, the hidden middle database, two NAND safe buffers) with
// 368 bytes to spare. Native MEM block headers hold host pointers and blocks
// are 32-byte aligned here, so the heap gets room for that overhead. The work
// buffer grows by the same amount, leaving the temporary heap its Wii size.
#define RFL_NATIVE_HEAP_OVERHEAD 0x400
#else
#define RFL_NATIVE_HEAP_OVERHEAD 0
#endif
#define RFL_SYSTEM_HEAP_SIZE (0x24800 + RFL_NATIVE_HEAP_OVERHEAD)
#define RFL_WORK_SIZE (0x4B000 + RFL_NATIVE_HEAP_OVERHEAD)
#define RFL_DELUXE_WORK_SIZE (0x64000 + RFL_NATIVE_HEAP_OVERHEAD)

/**
 * These functions are inlined, but with their conditions inverted(?) from the
 * actual functions. To avoid manually inlining them, these defines will hide
 * the fake inlines.
 */
#define RFLiGetLoader_() (RFLAvailable() ? &RFLiGetManager()->loader : NULL)
#define RFLiGetLastReason_() (RFLAvailable() ? RFLiGetManager()->lastReason : 0)
#define RFLGetLastReason_() (RFLAvailable() ? RFLiGetLastReason_() : sRFLLastReason)

static char* __RFLVersion = "<< RVL_SDK - RFL \trelease build: Mar  6 2008 17:40:04 (0x4199_60831) >>";

const RFLiCoordinateData scCoordinate = {1, 2, 0, FALSE, FALSE, FALSE};

static RFLiManager* sRFLManager = NULL;
static RFLErrcode sRFLLastErrCode = RFLErrcode_NotAvailable;
static u8 sRFLiFileBrokenType;
static s32 sRFLLastReason;

u32 RFLGetWorkSize(BOOL deluxeTex) {
    return deluxeTex ? RFL_DELUXE_WORK_SIZE + sizeof(RFLiManager) : RFL_WORK_SIZE + sizeof(RFLiManager);
}

u32 MEMGetAllocatableSizeForExpHeap(MEMiHeapHead* heap) {
    return MEMGetAllocatableSizeForExpHeapEx(heap, 4);
}

RFLErrcode RFLInitResAsync(void* workBuffer, void* resBuffer, u32 resSize, BOOL useDeluxTex) {
    RFLErrcode errcode;
    u32 wholesize;

    if (resBuffer == NULL) {
        return RFLErrcode_Fatal;
    }

#ifdef PETARI_NATIVE
    // The Wii trusts the disc image. Natively a damaged or mismatched
    // RFL_Res.dat is refused here instead of being read out of bounds.
    if (RFLiGetManager() == NULL) {
        char error[128];
        if (!RFLiNativeValidateResource(resBuffer, resSize, error, sizeof(error))) {
            OSReport("RFL: RFL_Res.dat rejected: %s\n", error);
            return RFLErrcode_Fatal;
        }
    }
#endif

    if (RFLiGetManager() == NULL) {
        OSRegisterVersion(__RFLVersion);

        wholesize = useDeluxTex ? RFL_DELUXE_WORK_SIZE : RFL_WORK_SIZE;
        memset(workBuffer, 0, wholesize);

        sRFLManager = (RFLiManager*)workBuffer;
        sRFLLastErrCode = RFLErrcode_NotAvailable;
        sRFLLastReason = NAND_RESULT_OK;
        sRFLiFileBrokenType = RFLiFileBrokenType_DBNotFound;
        sRFLManager->workBuffer = (u8*)workBuffer + sizeof(RFLiManager);

        {
            u32 size = useDeluxTex ? RFL_DELUXE_WORK_SIZE - sizeof(RFLiManager) : RFL_WORK_SIZE - sizeof(RFLiManager);
            RFLiGetManager()->rootHeap = MEMCreateExpHeapEx(RFLiGetManager()->workBuffer, size, 1);
        }
        {
            u32 size = RFL_SYSTEM_HEAP_SIZE;
            void* buffer = MEMAllocFromExpHeapEx(RFLiGetManager()->rootHeap, size, 32);
            RFLiGetManager()->systemHeap = MEMCreateExpHeapEx(buffer, size, 1);
        }
        {
            u32 size = MEMGetAllocatableSizeForExpHeap(RFLiGetManager()->rootHeap);
            void* buffer = MEMAllocFromExpHeapEx(RFLiGetManager()->rootHeap, size, 8);
            RFLiGetManager()->tmpHeap = MEMCreateExpHeapEx(buffer, size, 1);
        }

        RFLiGetManager()->lastErrCode = RFLErrcode_Success;
        RFLiGetManager()->beforeCloseErr = RFLErrcode_Success;
        RFLiGetManager()->lastReason = 0;
        RFLiGetManager()->beforeCloseReason = 0;
        RFLiGetManager()->deluxeTex = useDeluxTex;
        RFLiGetManager()->brokenType = RFLiFileBrokenType_DBNotFound;

        RFLSetIconDrawDoneCallback(NULL);
        RFLSetModelDrawDoneCallback(NULL);
        RFLiSetWorking(FALSE);
        RFLiInitDatabase(RFLiGetManager()->systemHeap);
        RFLiInitLoader();
        RFLiInitCtrlBuf(RFLiGetManager()->systemHeap);
        RFLiInitHiddenDatabase();
        RFLiInitAccessInfo(RFLiGetManager()->systemHeap);
        RFLiSetCoordinateData(&scCoordinate);

        if (resBuffer != NULL) {
            RFLiLoader* loader = RFLiGetLoader_();
            loader->cached = TRUE;
            loader->cacheSize = resSize;
            loader->cache = resBuffer;
        }

        errcode = RFLiBootLoadAsync();
        if (errcode != RFLErrcode_Busy && errcode != RFLErrcode_Success) {
            RFLExit();
        }
    } else {
        errcode = RFLErrcode_Success;
    }

    return errcode;
}

RFL_HEADER_STATIC RFLErrcode RFLInitRes(void* workBuffer, void* resBuffer, u32 resSize, BOOL deluxeTex) {
    RFLInitResAsync(workBuffer, resBuffer, resSize, deluxeTex);
    return RFLWaitAsync();
}

void RFLExit(void) {
    if (RFLiGetManager() == NULL) {
        return;
    }

    RFLWaitAsync();

    sRFLLastErrCode = RFLGetAsyncStatus();
    sRFLLastReason = RFLGetLastReason_();
    sRFLiFileBrokenType = RFLiGetManager()->brokenType;

    if (RFLIsResourceCached()) {
        RFLFreeCachedResource();
    }

    RFLiExitAccessInfo();

    MEMDestroyExpHeap(RFLiGetManager()->tmpHeap);
    MEMDestroyExpHeap(RFLiGetManager()->systemHeap);
    MEMDestroyExpHeap(RFLiGetManager()->rootHeap);

    sRFLManager = NULL;
}

static void bootloadDB2Res_(void) {
    RFLiLoadResourceHeaderAsync();

    switch (RFLWaitAsync()) {
    case RFLErrcode_Loadfail:
    case RFLErrcode_Success:
    case RFLErrcode_Busy:
        break;
    default:
        RFLExit();
    }
}

RFL_HEADER_STATIC RFLErrcode RFLiBootLoadAsync(void) {
    return RFLiBootLoadDatabaseAsync(bootloadDB2Res_);
}

BOOL RFLAvailable(void) {
    return sRFLManager != NULL;
}

static void* allocal_(u32 size, s32 align) {
    return MEMAllocFromExpHeapEx(RFLiGetManager()->tmpHeap, size, align);
}

RFL_HEADER_STATIC void* RFLiAlloc(u32 size) {
    return allocal_(size, 8);
}

void* RFLiAlloc32(u32 size) {
    return allocal_(size, 32);
}

void RFLiFree(void* block) {
    MEMFreeToExpHeap(RFLiGetManager()->tmpHeap, block);
}

RFLiDBManager* RFLiGetDBManager(void) {
    return !RFLAvailable() ? NULL : &RFLiGetManager()->dbMgr;
}

RFLiHDBManager* RFLiGetHDBManager(void) {
    return !RFLAvailable() ? NULL : &RFLiGetManager()->hdbMgr;
}

RFLiLoader* RFLiGetLoader(void) {
    return !RFLAvailable() ? NULL : &RFLiGetManager()->loader;
}

BOOL RFLiGetWorking(void) {
    return !RFLAvailable() ? FALSE : RFLiGetManager()->working;
}

void RFLiSetWorking(BOOL working) {
    RFLiGetManager()->working = working;
}

RFLiManager* RFLiGetManager(void) {
    return sRFLManager;
}

#ifdef PETARI_NATIVE
// On the Wii, NAND and alarm callbacks interrupt the one CPU, so a status
// poll never overlaps a callback's writes. Natively the callbacks run on
// platform host threads while holding the interrupt lock, in parallel with
// the game thread. Polling under the same lock orders everything a callback
// wrote before the game thread acts on the status it reports.
static RFLErrcode getAsyncStatusCore_(void);

RFLErrcode RFLGetAsyncStatus(void) {
    const BOOL enabled = OSDisableInterrupts();
    const RFLErrcode status = getAsyncStatusCore_();
    OSRestoreInterrupts(enabled);
    return status;
}

s32 RFLGetLastReason(void) {
    const BOOL enabled = OSDisableInterrupts();
    const s32 reason = !RFLAvailable() ? sRFLLastReason : RFLiGetLastReason_();
    OSRestoreInterrupts(enabled);
    return reason;
}

static RFLErrcode getAsyncStatusCore_(void) {
#else
RFLErrcode RFLGetAsyncStatus(void) {
#endif
    if (!RFLAvailable()) {
        return sRFLLastErrCode;
    }

    if (RFLiIsWorking()) {
        return RFLErrcode_Busy;
    }

    if (RFLiCriticalError()) {
        return RFLErrcode_Fatal;
    }

    return RFLiGetManager()->lastErrCode;
}

#ifndef PETARI_NATIVE
s32 RFLGetLastReason(void) {
    return !RFLAvailable() ? sRFLLastReason : RFLiGetLastReason_();
}
#endif

RFL_HEADER_STATIC RFLErrcode RFLWaitAsync(void) {
    volatile RFLErrcode status;

    do {
        status = RFLGetAsyncStatus();
    } while (status == RFLErrcode_Busy);

    return status;
}

RFLiAccessInfo* RFLiGetAccInfo(RFLiFileType type) {
    return !RFLAvailable() ? NULL : &RFLiGetManager()->info[type];
}

RFLiCtrlBufManager* RFLiGetCtrlBufManager(void) {
    return !RFLAvailable() ? NULL : &RFLiGetManager()->ctrlMgr;
}

BOOL RFLiGetUseDeluxTex(void) {
    return !RFLAvailable() ? FALSE : RFLiGetManager()->deluxeTex;
}

s32 RFLiGetLastReason(void) {
    return !RFLAvailable() ? FALSE : RFLiGetManager()->lastReason;
}

void RFLiSetFileBroken(RFLiFileBrokenType type) {
    if (RFLAvailable()) {
        RFLiGetManager()->brokenType |= (1 << type) & 0xFF;
    }
}

BOOL RFLiNotFoundError(void) {
    u8* broken = &sRFLiFileBrokenType;

    if (RFLAvailable()) {
        broken = &RFLiGetManager()->brokenType;
    }

    return *broken >> RFLiFileBrokenType_DBNotFound & 1;
}

BOOL RFLiNeedRepairError(void) {
    u8* broken = &sRFLiFileBrokenType;

    if (RFLAvailable()) {
        broken = &RFLiGetManager()->brokenType;
    }

    return *broken >> RFLiFileBrokenType_DBBroken & 1;
}

RFL_HEADER_STATIC BOOL RFLiCriticalError(void) {
    u8* broken = &sRFLiFileBrokenType;

    if (RFLAvailable()) {
        broken = &RFLiGetManager()->brokenType;
    }

    return *broken & 1 << RFLiFileBrokenType_ResBroken || *broken & 1 << RFLiFileBrokenType_Corrupt;
}

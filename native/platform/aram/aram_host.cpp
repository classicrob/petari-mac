// Native ARAM: port of src/RVL_SDK/aralt/aralt.c, the RVL SDK's "dummy ARAM"
// that keeps GameCube-style ARAM in MEM2.
//
// As in aralt.c:
// - ARInit() adopts the MEM2 arena [OSGetMEM2ArenaLo, OSGetMEM2ArenaHi) as
//   ARAM. ARAM offset N is host address base + N; offsets below
//   ARGetBaseAddress() (0x4000) are reserved. The game reserves this MEM2
//   range in HeapMemoryWatcher::createRootHeap and hands the same base to the
//   DSP through JKRHeap::setAltAramStartAdr.
// - ARAlloc() is a bump allocator returning ARAM offsets.
// - ARStartDMA(type, source, destination, length) copies synchronously with
//   the scheduler disabled, then runs the AR callback (the ARQ service routine
//   after ARQInit). Type 0 copies host memory (source) to an ARAM offset
//   (destination); type 1 copies an ARAM offset (source) to host memory
//   (destination).
// Native differences, all explicit failures instead of silent corruption:
// - main-memory addresses are host pointers (uintptr_t);
// - ARStartDMA checks direction, non-null host pointers, and that the ARAM
//   range lies inside ARAM, and aborts via OSPanic otherwise (aralt.c ignores
//   unknown types and copies out of range);
// - ARAlloc aborts when an allocation would pass the end of ARAM (aralt.c
//   lets the 32-bit top wrap);
// - no address alignment is required: RVL ARAM is MEM2 accessed by memcpy,
//   unlike GameCube ARAM DMA.

#include <revolution/aralt.h>
#include <revolution/os.h>

#include <cstring>

#include "os_internal.hpp"
#include "petari/platform/aram.hpp"

extern "C" {
void* OSGetMEM2ArenaLo(void);
void* OSGetMEM2ArenaHi(void);
}

namespace {

using PetariNative::Platform::OS::InterruptGuard;

constexpr u32 kReserved = 0x4000;

ARCallback __AR_Callback;
u32 __AR_Size;
uintptr_t __ARH_BaseAdr;
u32 __ARH_MemoryTop;  // bump allocator end, as an ARAM offset
u32 __AR_init_flag;

ARQRequest* __ARQRequestQueueHi;
ARQRequest* __ARQRequestQueueLo;
ARQRequest* __ARQRequestPendingHi;
ARQRequest* __ARQRequestPendingLo;
ARQCallback __ARQCallbackHi;
ARQCallback __ARQCallbackLo;
u32 __ARQChunkSize;
s32 __ARQ_init_flag;

bool aramRangeValid(uintptr_t offset, u32 length) {
    return offset <= 0xFFFFFFFFu && static_cast<u64>(offset) + length <= __AR_Size;
}

// ---- ARQ, unchanged in behaviour from aralt.c. The RVL SDK has no
// ARQPostRequest, so these queues are only ever empty; the service routine is
// still installed as the AR callback exactly as ARQInit does on the console.

void __ARQPopTaskQueueHi() {
    if (__ARQRequestQueueHi) {
        const u32 type = __ARQRequestQueueHi->type;
        if (type == 0) {
            ARStartDMA(type, __ARQRequestQueueHi->source, __ARQRequestQueueHi->dest, __ARQRequestQueueHi->length);
        } else {
            ARStartDMA(type, __ARQRequestQueueHi->dest, __ARQRequestQueueHi->source, __ARQRequestQueueHi->length);
        }
        __ARQCallbackHi = __ARQRequestQueueHi->callback;
        __ARQRequestPendingHi = __ARQRequestQueueHi;
        __ARQRequestQueueHi = __ARQRequestQueueHi->next;
    }
}

void __ARQServiceQueueLo() {
    if (!__ARQRequestPendingLo && __ARQRequestQueueLo) {
        __ARQRequestPendingLo = __ARQRequestQueueLo;
        __ARQRequestQueueLo = __ARQRequestQueueLo->next;
    }
    if (__ARQRequestPendingLo) {
        ARQRequest* r = __ARQRequestPendingLo;
        const u32 length = r->length <= __ARQChunkSize ? r->length : __ARQChunkSize;
        if (r->type == 0) {
            ARStartDMA(r->type, r->source, r->dest, length);
        } else {
            ARStartDMA(r->type, r->dest, r->source, length);
        }
        if (r->length <= __ARQChunkSize) {
            __ARQCallbackLo = r->callback;
        }
        r->length -= __ARQChunkSize;
        r->source += __ARQChunkSize;
        r->dest += __ARQChunkSize;
    }
}

void __ARQInterruptServiceRoutine() {
    if (__ARQCallbackHi) {
        __ARQCallbackHi(reinterpret_cast<uintptr_t>(__ARQRequestPendingHi));
        __ARQRequestPendingHi = nullptr;
        __ARQCallbackHi = nullptr;
    } else if (__ARQCallbackLo) {
        __ARQCallbackLo(reinterpret_cast<uintptr_t>(__ARQRequestPendingLo));
        __ARQRequestPendingLo = nullptr;
        __ARQCallbackLo = nullptr;
    }
    __ARQPopTaskQueueHi();
    if (!__ARQRequestPendingHi) {
        __ARQServiceQueueLo();
    }
}

}  // namespace

namespace PetariNative::Platform::ARAM {

bool isInitialized() {
    InterruptGuard guard;
    return __AR_init_flag != 0;
}

std::uintptr_t base() {
    InterruptGuard guard;
    return __AR_init_flag ? __ARH_BaseAdr : 0;
}

std::uint32_t size() {
    InterruptGuard guard;
    return __AR_Size;
}

std::uint32_t allocated() {
    InterruptGuard guard;
    return __ARH_MemoryTop;
}

void* translate(std::uint32_t offset, std::uint32_t length) {
    InterruptGuard guard;
    if (!__AR_init_flag || !aramRangeValid(offset, length)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(__ARH_BaseAdr + offset);
}

void reset() {
    InterruptGuard guard;
    __AR_Callback = nullptr;
    __AR_Size = 0;
    __ARH_BaseAdr = 0;
    __ARH_MemoryTop = 0;
    __AR_init_flag = 0;
    __ARQRequestQueueHi = __ARQRequestQueueLo = nullptr;
    __ARQRequestPendingHi = __ARQRequestPendingLo = nullptr;
    __ARQCallbackHi = __ARQCallbackLo = nullptr;
    __ARQChunkSize = 0;
    __ARQ_init_flag = 0;
}

}  // namespace PetariNative::Platform::ARAM

extern "C" {

void ARStartDMA(u32 type, uintptr_t source, uintptr_t destination, u32 length) {
    ARCallback callback;
    {
        InterruptGuard guard;
        if (!__AR_init_flag) {
            OSPanic(__FILE__, __LINE__, "ARStartDMA(): ARAM is not initialized (ARInit)");
        }
        uintptr_t hostSource;
        uintptr_t hostDestination;
        if (type == 0) {
            if (source == 0 || !aramRangeValid(destination, length)) {
                OSPanic(__FILE__, __LINE__, "ARStartDMA(): main 0x%lx -> ARAM 0x%lx, length 0x%x is outside ARAM (size 0x%x)",
                        static_cast<unsigned long>(source), static_cast<unsigned long>(destination), length, __AR_Size);
            }
            hostSource = source;
            hostDestination = __ARH_BaseAdr + destination;
        } else if (type == 1) {
            if (destination == 0 || !aramRangeValid(source, length)) {
                OSPanic(__FILE__, __LINE__, "ARStartDMA(): ARAM 0x%lx -> main 0x%lx, length 0x%x is outside ARAM (size 0x%x)",
                        static_cast<unsigned long>(source), static_cast<unsigned long>(destination), length, __AR_Size);
            }
            hostSource = __ARH_BaseAdr + source;
            hostDestination = destination;
        } else {
            OSPanic(__FILE__, __LINE__, "ARStartDMA(): unknown transfer type %u", type);
        }
        callback = __AR_Callback;

        OSDisableScheduler();
        if (length != 0) {
            std::memmove(reinterpret_cast<void*>(hostDestination), reinterpret_cast<const void*>(hostSource), length);
        }
        OSEnableScheduler();
    }
    if (callback) {
        callback();
    }
}

u32 ARAlloc(u32 amount) {
    InterruptGuard guard;
    if (!__AR_init_flag) {
        OSPanic(__FILE__, __LINE__, "ARAlloc(): ARAM is not initialized (ARInit)");
    }
    if (static_cast<u64>(__ARH_MemoryTop) + amount > __AR_Size) {
        OSPanic(__FILE__, __LINE__, "ARAlloc(): 0x%x bytes do not fit: 0x%x of 0x%x bytes of ARAM already allocated", amount,
                __ARH_MemoryTop, __AR_Size);
    }
    const u32 top = __ARH_MemoryTop;
    __ARH_MemoryTop += amount;
    return top;
}

u32 ARInit(u32*, u32) {
    InterruptGuard guard;
    if (__AR_init_flag != 0) {
        return kReserved;
    }
    const uintptr_t memLo = reinterpret_cast<uintptr_t>(OSGetMEM2ArenaLo());
    const uintptr_t memHi = reinterpret_cast<uintptr_t>(OSGetMEM2ArenaHi());
    if (memLo == 0 || memHi < memLo + kReserved || memHi - memLo > 0xFFFFFFFFu) {
        OSPanic(__FILE__, __LINE__, "ARInit(): MEM2 arena %p -> %p cannot hold ARAM", reinterpret_cast<void*>(memLo),
                reinterpret_cast<void*>(memHi));
    }
    const u32 diff = static_cast<u32>(memHi - memLo);
    __AR_Callback = nullptr;
    __AR_init_flag = 1;
    __ARH_BaseAdr = memLo;
    __ARH_MemoryTop = ARGetBaseAddress();
    __AR_Size = diff;
    OSReport("ARInit : Dummy ARAM enabled (RVL), area %p -> %p (size 0x%x)\n", reinterpret_cast<void*>(memLo),
             reinterpret_cast<void*>(memHi), diff);
    return kReserved;
}

u32 ARGetBaseAddress(void) {
    return kReserved;
}

u32 ARGetSize(void) {
    InterruptGuard guard;
    return __AR_Size;
}

void ARQInit(void) {
    InterruptGuard guard;
    if (__ARQ_init_flag != 1) {
        __ARQRequestQueueLo = nullptr;
        __ARQRequestQueueHi = nullptr;
        __ARQChunkSize = 0x1000;
        __AR_Callback = __ARQInterruptServiceRoutine;
        __ARQRequestPendingHi = nullptr;
        __ARQRequestPendingLo = nullptr;
        __ARQCallbackHi = nullptr;
        __ARQCallbackLo = nullptr;
        __ARQ_init_flag = 1;
    }
}

}  // extern "C"

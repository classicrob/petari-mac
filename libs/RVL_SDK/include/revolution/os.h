#ifndef OS_H
#define OS_H

#if defined(PETARI_NATIVE) && !defined(__cplusplus)
#include <stdarg.h>
#else
#include <cstdarg>
#endif
#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OSHalt(msg) OSPanic(__FILE__, __LINE__, msg)

#ifdef __MWERKS__
u32 __OSBusClock : (0x8000 << 16 | 0x00F8);
u32 __MEM2End : (0x8000 << 16 | 0x3128);
#elif defined(PETARI_NATIVE)
// Values of the Wii low-memory globals, defined once by the native OS platform.
extern u32 __OSBusClock;
extern uintptr_t __MEM2End;
#else
u32 __OSBusClock = 0x800000F8;
u32 __MEM2End = 0x80003128;
#endif

#define OS_BUS_CLOCK __OSBusClock
#ifndef OS_CORE_CLOCK
#define OS_CORE_CLOCK 729000000u
#endif
#ifndef OS_BUS_CLOCK
#define OS_BUS_CLOCK 243000000u
#endif
#ifndef OS_TIMER_CLOCK
#define OS_TIMER_CLOCK (OS_BUS_CLOCK / 4)
#endif

#define OSNanosecondsToTicks(nsec) (((nsec) * (OS_TIMER_CLOCK / 125000)) / 8000)

#define OSTicksToSeconds(ticks) ((ticks) / OS_TIMER_CLOCK)
#define OSTicksToMilliseconds(ticks) ((ticks) / (OS_TIMER_CLOCK / 1000))
#define OSTicksToMicroseconds(ticks) (((ticks)*8) / (OS_TIMER_CLOCK / 125000))
#define OSSecondsToTicks(sec) ((sec)*OS_TIMER_CLOCK)
#define OSMillisecondsToTicks(msec) ((msec) * (OS_TIMER_CLOCK / 1000))
#define OSMicrosecondsToTicks(usec) (((usec) * (OS_TIMER_CLOCK / 125000)) / 8)

void* OSPhysicalToUncached(u32);

#define OS_CACHED_REGION_PREFIX 0x8000
#define OS_UNCACHED_REGION_PREFIX 0xC000

#define OS_BASE_CACHED (OS_CACHED_REGION_PREFIX << 16)
#define OS_BASE_UNCACHED (OS_UNCACHED_REGION_PREFIX << 16)

#ifdef PETARI_NATIVE
void* PetariNativePhysicalToHost(uintptr_t);
uintptr_t PetariNativeHostToPhysical(const void*);
BOOL PetariNativeIsMemoryRegion(const void*, int);
#define OSPhysicalToCached(paddr) PetariNativePhysicalToHost((uintptr_t)(paddr))
#define OSCachedToPhysical(caddr) PetariNativeHostToPhysical((const void*)(caddr))
#define OSUncachedToPhysical(ucaddr) PetariNativeHostToPhysical((const void*)(ucaddr))
#define OSPhysicalToUncached(paddr) PetariNativePhysicalToHost((uintptr_t)(paddr))
#define OSIsMEM1Region(addr) PetariNativeIsMemoryRegion((const void*)(addr), 1)
#define OSIsMEM2Region(addr) PetariNativeIsMemoryRegion((const void*)(addr), 2)
#else
#define OSPhysicalToCached(paddr) ((void*)((u32)(paddr) + OS_BASE_CACHED))
#define OSCachedToPhysical(caddr) ((u32)((u8*)(caddr)-OS_BASE_CACHED))
#define OSUncachedToPhysical(ucaddr) ((u32)((u32)(ucaddr) & (~OS_BASE_UNCACHED)))
#define OSPhysicalToUncached(paddr) ((void*)((u32)(paddr) + OS_BASE_UNCACHED))

#define OSIsMEM1Region(addr) (((u32)(addr)&0x30000000) == 0x00000000)
#define OSIsMEM2Region(addr) (((u32)(addr)&0x30000000) == 0x10000000)
#endif

#ifdef PETARI_NATIVE
// Accepts pointers or sizes; pointer inputs keep their full host width.
#define OSRoundUp32B(x) (((uintptr_t)(x) + 32 - 1) & ~(uintptr_t)(32 - 1))
#define OSRoundDown32B(x) (((uintptr_t)(x)) & ~(uintptr_t)(32 - 1))
#else
#define OSRoundUp32B(x) (((u32)(x) + 32 - 1) & ~(32 - 1))
#define OSRoundDown32B(x) (((u32)(x)) & ~(32 - 1))
#endif
#define OSDiffTick(tick1, tick0) ((s32)(tick1) - (s32)(tick0))

#define OS_TICKS_DELTA(x, y) ((s32)x - (s32)y)

u32 OSGetSoundMode(void);
void OSSetSoundMode(u32 mode);

u32 OSGetConsoleType(void);

void OSRegisterVersion(const char*);

void OSReport(const char*, ...);
void OSVReport(const char*, va_list);
void OSPanic(const char*, int, const char*, ...);

void* OSGetArenaHi(void);
void* OSGetArenaLo(void);

void OSSetArenaHi(void*);
void OSSetArenaLo(void*);

void OSSetMEM2ArenaHi(void*);

void* OSGetMEM1ArenaLo(void);
void* OSGetMEM1ArenaHi(void);
void* OSGetMEM2ArenaLo(void);
void* OSGetMEM2ArenaHi(void);

void* OSAllocFromMEM1ArenaLo(u32, u32);

u32 OSGetPhysicalMem2Size(void);

void OSProtectRange(u32, void*, u32, u32);

void* OSAllocFromHeap(int, u32);

typedef struct OSIOSRev {
    u8 reserved;
    u8 major;
    u8 minor;
    u8 micro;
    u8 month;
    u8 date;
    u16 year;
} OSIOSRev;

const u8 OSGetAppType(void);

char* OSUTF8to32(const char*, u32*);
u16* OSUTF16to32(const u16*, u32*);
u8 OSUTF32toANSI(u32);
u16 OSUTF32toSJIS(u32);

extern void __RAS_OSDisableInterrupts_begin(void);
extern void __RAS_OSDisableInterrupts_end(void);

extern s64 __OSStartTime;

const char* OSGetAppGamename(void);

void* OSAllocFromArenaLo(u32 size, u32 align);
void* OSAllocFromArenaHi(u32 size, u32 align);

#define OSAssert_Line(line_, exp_)

#ifndef ASSERT
#define ASSERT(exp) ((void)0)
#endif

#ifndef ASSERTMSG
#define ASSERTMSG(exp, msg) ((void)0)
#endif

inline s16 __OSf32tos16(__REGISTER f32 inF) {
#ifdef PETARI_NATIVE
    // PSQ stores saturate to the destination range and truncate toward zero.
    if (inF != inF) return 0;
    return inF <= -32768.0f ? -32768 : inF >= 32767.0f ? 32767 : (s16)inF;
#else
    __REGISTER s16 out;
    u32 tmp;
    __REGISTER u32* tmpPtr = &tmp;
    // clang-format off
#ifdef __MWERKS__
    asm {
        psq_st inF, 0(tmpPtr), 0x1, 5
        lha out, 0(tmpPtr)
    }
#endif
    // clang-format on

    return out;
#endif
}

inline void OSf32tos16(f32* f, s16* out) {
    *out = __OSf32tos16(*f);
}

inline u8 __OSf32tou8(__REGISTER f32 inF) {
#ifdef PETARI_NATIVE
    // PSQ stores saturate to the destination range and truncate toward zero.
    return !(inF > 0.0f) ? 0 : inF >= 255.0f ? 255 : (u8)inF;
#else
    __REGISTER u8 out;
    u32 tmp;
    __REGISTER u32* tmpPtr = &tmp;
    // clang-format off
#ifdef __MWERKS__
    asm {
        psq_st inF, 0(tmpPtr), 0x1, 2
        lbz out, 0(tmpPtr)
    }
#endif
    // clang-format on

    return out;
#endif
}

#ifdef OS_F32_TO_U8_DEFER_INLINE
static void OSf32tou8(f32* f, u8* out);
#else
inline void OSf32tou8(f32* f, u8* out) {
    *out = __OSf32tou8(*f);
}
#endif

#include "revolution/base/PPCArch.h"
#include "revolution/gx.h"
#include "revolution/os/OSAlarm.h"
#include "revolution/os/OSAlloc.h"
#include "revolution/os/OSCache.h"
#include "revolution/os/OSContext.h"
#include "revolution/os/OSException.h"
#include "revolution/os/OSExecParams.h"
#include "revolution/os/OSFastCast.h"
#include "revolution/os/OSFont.h"
#include "revolution/os/OSInterrupt.h"
#include "revolution/os/OSMessage.h"
#include "revolution/os/OSMutex.h"
#include "revolution/os/OSPlayRecord.h"
#include "revolution/os/OSPlayTime.h"
#include "revolution/os/OSReset.h"
#include "revolution/os/OSResetSW.h"
#include "revolution/os/OSRtc.h"
#include "revolution/os/OSStateFlags.h"
#include "revolution/os/OSStateTM.h"
#include "revolution/os/OSThread.h"
#include "revolution/os/OSTime.h"

/* PRIVATE OS FUNCTIONS */
void __OSInitSystemCall(void);
void __OSUnhandledException(__OSException, OSContext*, u32, u32);
void __OSReschedule(void);
void __OSInitAlarm(void);
void __OSModuleInit(void);
void __OSInterruptInit(void);
void __OSContextInit(void);
void __OSInitAudioSystem(void);
void __OSInitMemoryProtection(void);
void __OSGetIOSRev(OSIOSRev*);
int __OSInitSTM(void);
void __OSInitNet(void);
void __OSPromoteThread(OSThread*, OSPriority);

void OSFatal(GXColor, GXColor, const char*);

#ifdef __cplusplus
}
#endif

#endif  // OS_H

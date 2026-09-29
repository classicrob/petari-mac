#ifndef AI_H
#define AI_H

#include "revolution/types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*AIDCallback)(void);

#define SET_FLAG(reg, value, mask, shift) reg = reg & ~mask | value << shift;

#define GET_FLAG(reg, mask, shift) ((reg & mask) >> shift)

AIDCallback AIRegisterDMACallback(AIDCallback);
#ifdef PETARI_NATIVE
// DMA buffers are host pointers.
void AIInitDMA(uintptr_t, u32);
#else
void AIInitDMA(u32, u32);
#endif
void AIStartDMA(void);
void AIStopDMA(void);
#ifdef PETARI_NATIVE
uintptr_t AIGetDMAStartAddr(void);
#else
u32 AIGetDMAStartAddr(void);
#endif
u32 AIGetDMALength(void);
void AISetDSPSampleRate(u32);
u32 AIGetDSPSampleRate(void);
void AIInit(u8*);

#ifdef __cplusplus
}
#endif

#endif  // AI_H

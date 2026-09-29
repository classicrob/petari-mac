#ifndef ARALT_H
#define ARALT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "revolution/types.h"
#include <stdint.h>

typedef void (*ARCallback)(void);
// Main-memory addresses and request pointers use pointer width on native hosts.
// ARAM addresses remain 32-bit offsets.
typedef void (*ARQCallback)(uintptr_t);

typedef struct ARQRequest {
    struct ARQRequest* next;
    u32 owner;
    u32 type;
    u32 priority;
    uintptr_t source;
    uintptr_t dest;
    u32 length;
    ARQCallback callback;
} ARQRequest;

void ARStartDMA(u32, uintptr_t, uintptr_t, u32) NO_INLINE;
u32 ARAlloc(u32);
u32 ARInit(u32*, u32);
u32 ARGetBaseAddress(void);
u32 ARGetSize(void);

void ARQInit(void);

#ifdef __cplusplus
}
#endif

#endif  // ARALT_H

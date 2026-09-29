// SDK-internal declarations that src/RVL_SDK/os/*.c rely on without a header.
// Force-included when those files are compiled natively.
#pragma once

#include <revolution/os.h>

#ifdef __cplusplus
extern "C" {
#endif

OSPriority __OSGetEffectivePriority(OSThread* thread);
void __OSUnlockAllMutex(OSThread* thread);
extern OSThreadQueue __OSActiveThreadQueue;

#ifdef __cplusplus
}
#endif

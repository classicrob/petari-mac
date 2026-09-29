#pragma once

#include <revolution/types.h>
#include <stdint.h>

// Addresses sent to the audio DSP: 32-bit physical addresses on Wii, host
// pointers for the native DSP.
#ifdef PETARI_NATIVE
typedef uintptr_t JASDspAddr;
#else
typedef u32 JASDspAddr;
#endif

void DSPReleaseHalt2(u32 msg);
void DsetupTable(u32 param_0, JASDspAddr param_1, JASDspAddr param_2, JASDspAddr param_3, JASDspAddr param_4);
void DsetMixerLevel(f32 level);
void DsyncFrame2ch(u32 param_0, JASDspAddr param_1, JASDspAddr param_2);
void DsyncFrame4ch(u32 param_0, JASDspAddr param_1, JASDspAddr param_2, JASDspAddr param_3, JASDspAddr param_4);
void DsetVARAM(JASDspAddr param_0);

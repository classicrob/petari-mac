#pragma once
// Host view of the emulated ARAM (Wii "alternate ARAM" in MEM2).
//
// ARInit() takes the MEM2 arena as ARAM, as the RVL SDK's aralt does: ARAM
// offset N is host address base() + N. The first ARGetBaseAddress() (0x4000)
// bytes are reserved. The native audio mixer reads sample data here, where
// the Wii DSP reads from the address given to DsetVARAM.

#include <cstddef>
#include <cstdint>

namespace PetariNative::Platform::ARAM {

bool isInitialized();
std::uintptr_t base();      // host address of ARAM offset 0; 0 before ARInit
std::uint32_t size();       // bytes of ARAM, including the reserved 0x4000
std::uint32_t allocated();  // end offset of the ARAlloc bump allocator

// Host pointer for [offset, offset + length) inside ARAM, or null if the
// range is outside ARAM or ARAM is not initialised.
void* translate(std::uint32_t offset, std::uint32_t length);

// Forgets the ARAM state so ARInit() can run again. For tests.
void reset();

}  // namespace PetariNative::Platform::ARAM

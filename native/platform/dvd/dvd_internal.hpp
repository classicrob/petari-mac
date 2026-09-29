#pragma once
// Test hooks for the native DVD drive. Not for game code.

#include <cstdint>
#include <functional>

#include <revolution/dvd.h>

namespace PetariNative::Platform::DVD::Testing {

// Called on the drive thread after each transfer chunk of a read, with the
// drive lock released. Lets tests hold a read in the busy state
// deterministically. Pass an empty function to remove.
void setChunkHook(std::function<void(DVDCommandBlock*)> hook);

// Bytes the drive transfers per step. The SDK uses 0x80000; tests shrink it to
// exercise multi-chunk reads and mid-transfer cancellation.
void setChunkSize(std::uint32_t bytes);

}  // namespace PetariNative::Platform::DVD::Testing

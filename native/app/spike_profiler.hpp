#pragma once
// Opt-in spike profiler (PETARI_SPIKE_PROFILE=<path>): a sampler thread records
// the stacks of the game (main) thread and Aurora's GX FIFO processor every
// PETARI_SPIKE_PROFILE_US microseconds (default 1000) into a preallocated
// ring. When the frame seam sees a long frame, it keeps that frame's samples;
// at exit they are symbolized and written, aggregated per spike and thread.
// A sample suspends the target thread only to read its registers and walk its
// frame-pointer chain: no allocation or locks while it is suspended.
// macOS only; a no-op elsewhere. Standard headers only.

#include <cstdint>

namespace PetariNative::App::SpikeProfiler {

// Main thread, before the game starts. Returns false when disabled.
bool startFromEnvironment();
bool enabled();
// Frame seam, main thread: keeps the samples in [startNs, endNs] (steady clock).
void keepSpike(std::uint64_t frameIndex, double intervalMs, std::uint64_t startNs, std::uint64_t endNs);
// At exit: writes the report (once).
void writeReport();

}  // namespace PetariNative::App::SpikeProfiler

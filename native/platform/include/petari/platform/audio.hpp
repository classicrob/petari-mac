#pragma once
// Host side of the native audio interface (AI).
//
// The game streams stereo 16-bit samples through AI DMA: AIInitDMA registers
// a block, AIStartDMA starts playback, and the DMA interrupt (the callback
// from AIRegisterDMACallback) fires as each block starts playing, when the
// game registers the block after it. Natively a host audio backend
// (CoreAudio, SDL, ...), owned by the application, pulls samples:
//
//   PetariNative::Platform::Audio::pull(buffer, frames)
//
// from its realtime callback. pull() never blocks or takes the OS lock: DMA
// interrupts are handed to an AI interrupt thread, which runs the game
// callback with interrupts disabled. The platform library links no audio
// framework.

#include <cstddef>
#include <cstdint>

namespace PetariNative::Platform::Audio {

// Output sample rate selected by AISetDSPSampleRate: 32000 or 48000 Hz. (Wii
// hardware runs about 0.09% fast, e.g. 32028.5 Hz; the backend plays the
// nominal rate.)
std::uint32_t outputRate();

// Fills frames of interleaved stereo int16 (L, R), host byte order. The
// game's DMA blocks are interleaved R, L as on the hardware (JAudio2's DAC
// buffer, THP audio); pull() swaps them into L, R. While DMA is stopped, or
// if no block is registered, the output is silence, as from the hardware.
// Realtime-safe. Returns the number of frames written (always frames).
std::size_t pull(std::int16_t* interleaved, std::size_t frames);

// Puts the calling host thread under Mach time-constraint (real-time)
// scheduling suited to audio work: wakes within about a millisecond, runs
// briefly. For the AI interrupt thread and an audio backend's producer.
// Returns false if the kernel refused (the thread keeps its old policy).
bool setRealtimeAudioThread();

// Backend notification: called when the game first starts AI DMA (so the
// device can open at outputRate()) and when AI is shut down. Both run on the
// game thread; neither may call pull().
struct Sink {
    void (*start)(std::uint32_t sampleRate, void* user) = nullptr;
    void (*stop)(void* user) = nullptr;
    void* user = nullptr;
};
void setSink(const Sink& sink);

// Diagnostic: block starts at which no new block had been registered since
// the previous start, so the hardware replayed a block. JAudio2 registers a
// fresh buffer on every DMA interrupt, so a nonzero count there means its
// audio thread ran late (audible as a repeated ~17 ms fragment).
std::uint64_t replayedBlocks();

// Diagnostic: the longest time from a DMA interrupt being raised (block
// start) to its delivery to the game, since the previous call.
std::uint32_t takeWorstInterruptLatencyMicroseconds();

// DMA interrupts raised by pull() and not yet delivered to the game.
std::uint32_t pendingInterrupts();

// Blocks until every DMA interrupt raised so far has been delivered. For
// deterministic tests; not for realtime threads.
void drainInterrupts();

// Stops DMA, the interrupt thread, and the sink. For tests and exit.
void shutdown();

}  // namespace PetariNative::Platform::Audio

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

// Opt-in audio-cycle timing (off by default; see audio/audio_timing.hpp). Worst
// values since the previous takeTimingStats(), in microseconds:
struct TimingStats {
    std::int64_t dmaDeliverUs = 0;               // DMA interrupt raised (block start) -> game callback
    // Registration (AIInitDMA of the next block), per audio/audio_timing.hpp:
    std::int64_t latestRaiseToRegisterUs = 0;    // from the latest delivered DMA interrupt's raise
    std::int64_t latestDeliverToRegisterUs = 0;  // from its delivery
    std::int64_t oldestRaiseToRegisterUs = 0;    // from the oldest RETAINED one delivered since the previous registration
    std::int64_t generationsPerRegistration = 0; // DMA interrupts since the previous registration (normally 1)
    std::int64_t dspInterruptsBeforeRegister = 0;  // DSP interrupts delivered since that oldest one
    std::int64_t dspDeliverUs = 0;               // DSP interrupt raised -> handler
    std::int64_t subframeRenderUs = 0;           // DSP voices released -> subframe rendered
    std::int64_t frameUs = 0;                    // DSP frame requested -> frame end
    std::uint64_t registrationsMissingBlocks = 0;  // registrations answering 2+ interrupts (a block went unanswered)
    std::uint64_t registrationsTruncated = 0;      // answering more than the 64-record ring retains: "oldest" is retained-only
};
void setTimingDiagnostics(bool enabled);
TimingStats takeTimingStats();

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

#pragma once
// Opt-in timing diagnostics for the AI/DSP/JAudio2 audio cycle (see
// Audio::setTimingDiagnostics in petari/platform/audio.hpp). Internal to
// petari_platform_audio (ai_host.cpp, dsp_device.cpp).
//
// JAudio2's audio thread takes DMA and DSP interrupts from one message queue.
// A DMA interrupt (block start) leads to updateDac, whose first act is
// AIInitDMA of the next block; that must happen before the current block ends
// or the block is replayed. The timings below separate where the time goes:
//   AI raise -> DMA callback delivered   (host wake of the AI interrupt thread)
//   delivered -> AIInitDMA               (see the mapping below: queued events
//                                         make "latest" and "oldest" differ)
//   DSP interrupts delivered in between  (updateDSP/finishDSPFrame ahead of it)
//   DSP raise -> DSP handler             (host wake of the DSP interrupt thread)
//   voices released -> subframe rendered (DSP render work on the host)
//   frame requested -> frame end         (whole DSP frame, 7 audio-thread round trips)
//
// Disabled cost: one relaxed atomic load per event. Enabled: a clock read and
// a few relaxed atomics per event. Worst values are taken and reset by
// takeTimingStats().

#include <atomic>
#include <chrono>
#include <cstdint>

namespace PetariNative::Platform::Audio::Timing {

inline std::atomic<bool> enabled{false};

inline std::int64_t now() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

inline std::int64_t toMicroseconds(std::int64_t ticks) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::duration(ticks)).count();
}

inline void raiseMax(std::atomic<std::int64_t>& worst, std::int64_t value) {
    std::int64_t current = worst.load(std::memory_order_relaxed);
    while (value > current && !worst.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

// DMA interrupt -> registration (AIInitDMA) of the next block.
// Every delivered DMA interrupt (generation g, counted from 1) records its
// raise time, delivery time and the DSP-delivery count in a ring. JAudio2
// answers DMA interrupts in order, one AIInitDMA each (none for the very
// first one after AIStartDMA), so a registration is answering the OLDEST
// interrupt delivered since the previous registration, or a later one.
// Each registration therefore reports:
//   - generations since the previous registration (normally 1; 2 or more
//     means at least one block went by unanswered and was replayed);
//   - time since the latest delivered interrupt's raise and delivery;
//   - time since the oldest unanswered interrupt's raise that is still in the
//     ring (at startup it includes the unanswered first interrupt, i.e. one
//     extra block). If more than kDmaRecords - 1 interrupts went unanswered,
//     the true oldest is no longer recorded: the value is only the oldest
//     RETAINED one and the registration is counted as truncated;
//   - DSP interrupts delivered since that oldest one (work queued ahead of
//     the DMA message on the audio thread).
constexpr std::uint64_t kDmaRecords = 64;
struct DmaRecord {
    std::atomic<std::int64_t> raisedAt{0};
    std::atomic<std::int64_t> deliveredAt{0};
    std::atomic<std::uint64_t> dspAtDelivery{0};
};
inline DmaRecord dmaRecords[kDmaRecords];
inline std::atomic<std::uint64_t> dmaGeneration{0};       // DMA callbacks delivered (while enabled)
inline std::atomic<std::uint64_t> generationAtRegistration{0};  // dmaGeneration at the previous registration

// Monotonic count of DSP interrupts delivered while enabled (dsp_device.cpp).
inline std::atomic<std::uint64_t> dspDelivered{0};

// Worst values since the last take (steady_clock ticks, counts).
inline std::atomic<std::int64_t> worstDmaDeliver{0};
inline std::atomic<std::int64_t> worstLatestRaiseToRegister{0};
inline std::atomic<std::int64_t> worstLatestDeliverToRegister{0};
inline std::atomic<std::int64_t> worstOldestRaiseToRegister{0};
inline std::atomic<std::int64_t> worstGenerationsPerRegistration{0};
inline std::atomic<std::int64_t> worstDspBetween{0};
inline std::atomic<std::int64_t> worstDspDeliver{0};
inline std::atomic<std::int64_t> worstSubframeRender{0};
inline std::atomic<std::int64_t> worstFrame{0};
inline std::atomic<std::uint64_t> registrationsMissingBlocks{0};  // registrations answering >= 2 generations
inline std::atomic<std::uint64_t> registrationsTruncated{0};      // ... more than the ring retains (oldest is retained-only)

}  // namespace PetariNative::Platform::Audio::Timing

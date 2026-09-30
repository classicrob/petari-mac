#pragma once
// Frame-time attribution counters. Any thread may add to them (render worker,
// GX processor, VI and GP interrupt threads, game threads); the frame seam
// (native/app/frame_seam.cpp) samples them once per game frame. Adding is a
// few relaxed atomic operations: nothing here blocks, allocates or logs, so it
// is safe on real-time and interrupt threads.
//
// Each counter is time spent in one kind of wait, measured on the thread that
// waited. They overlap each other and the game thread's own timings, so they
// are attribution hints for a frame, not parts that sum to its interval.
//
// Header-only (C++17 inline variables): every static library that includes it
// shares one copy in the executable, without a link dependency.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace PetariNative::FrameTelemetry {

enum Counter : unsigned {
    // GX processor thread: blocking pipeline resolves (first-use compiles).
    PipelineWait,
    // GX processor thread: EFB capture segment, waiting for staging memory.
    EfbStagingWait,
    // GX processor thread: EFB capture segment, waiting for the render worker
    // to encode and submit it (includes the worker's queue ahead of it).
    EfbSubmitWait,
    // An EFB capture from its request (GX processor thread, at a draw-sync
    // token) until its copy was mapped back and handed to GX sync: the GPU
    // round trip. That token, and the draw-done interrupts behind it, are
    // held back that long.
    EfbCaptureLatency,
    // Render worker: surface GetCurrentTexture (drawable acquisition).
    DrawableAcquire,
    // Render worker: surface Present() call. Submission side, not scan-out.
    PresentCall,
    // Render worker: queue Submit of the frame's command buffer.
    FrameSubmit,
    // VI thread: retrace timer woke after its deadline (overshoot).
    ViTimerLate,
    // VI thread: after the timer woke, waiting for the OS interrupt lock
    // (a game thread with interrupts disabled delays the retrace interrupt).
    ViInterruptLockWait,
    // GX processor thread: hashing texture sources on a texture-object cache miss.
    TextureHash,
    // GX processor thread: creating, converting and uploading a static texture.
    TextureUpload,
    // Game thread: waiting at the frame boundary for the previous frame's
    // draw-sync tokens (EFB readbacks) to be delivered.
    TokenBarrierWait,
    // Game thread: GameSystemSceneController::startScene waiting for the stage's
    // shader preparation (petari_gx_pipeline_stage_wait) before the first frame.
    StagePrepWait,
    CounterCount
};

struct Slot {
    std::atomic<std::uint64_t> ns{0};      // total, monotonic
    std::atomic<std::uint64_t> events{0};  // total, monotonic
    std::atomic<std::uint64_t> maxNs{0};   // largest single event since the last takeMax
};

inline Slot slots[CounterCount];

// Steady clock of the latest VI retrace interrupt, in ns (0 before the first).
inline std::atomic<std::uint64_t> lastRetraceNs{0};

inline std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void add(Counter counter, std::uint64_t ns) {
    Slot& slot = slots[counter];
    slot.ns.fetch_add(ns, std::memory_order_relaxed);
    slot.events.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t seen = slot.maxNs.load(std::memory_order_relaxed);
    while (ns > seen && !slot.maxNs.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
}

// Times one wait: construction to destruction.
class Scope {
public:
    explicit Scope(Counter counter) : mCounter(counter), mStart(nowNs()) {}
    ~Scope() { add(mCounter, nowNs() - mStart); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Counter mCounter;
    std::uint64_t mStart;
};

}  // namespace PetariNative::FrameTelemetry

// Native audio interface (AI). Replaces src/RVL_SDK/ai/ai.c.
//
// Hardware model: the DMA engine plays the registered block; when a block
// starts playing it raises the AI DMA interrupt, and the registers written by
// AIInitDMA during that block are latched when it ends. Natively the backend's
// pull() is the DMA engine. Block registration uses a small spin lock (a few
// instructions), so the realtime thread never waits on OS locks. Interrupts
// are counted and signalled with a dispatch semaphore to an AI interrupt
// thread, which calls the game's DMA callback with interrupts disabled.

#include <revolution/ai.h>
#include <revolution/os.h>

#include <dispatch/dispatch.h>
#include <pthread.h>

#include <atomic>
#include <cstring>

#include "os_internal.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/audio.hpp"

namespace OS = PetariNative::Platform::OS;
namespace PAudio = PetariNative::Platform::Audio;

namespace {

class SpinLock {
public:
    void lock() {
        while (mFlag.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() { mFlag.clear(std::memory_order_release); }

private:
    std::atomic_flag mFlag = ATOMIC_FLAG_INIT;
};

struct Block {
    const std::int16_t* samples = nullptr;  // interleaved stereo
    std::uint32_t frames = 0;
};

SpinLock gRegisterLock;
Block gRegistered;       // AIInitDMA, latched at the next block start
Block gPlaying;          // DMA engine state (pull thread only)
std::uint32_t gPosition; // frames played in gPlaying (pull thread only)
std::atomic<bool> gRunning{false};
std::atomic<std::uint32_t> gRate{32000};

std::atomic<AIDCallback> gCallback{nullptr};
std::atomic<std::uint32_t> gRaised{0};
std::atomic<std::uint32_t> gDelivered{0};
dispatch_semaphore_t gInterruptSignal;
std::atomic<bool> gInitialized{false};
std::atomic<bool> gStopThread{false};
pthread_t gInterruptThread;

PAudio::Sink gSink;
bool gSinkStarted;

// Delivery is driven by the raised/delivered counts, not by semaphore
// signals: a signal only wakes the thread, so extra or stale wake-ups deliver
// nothing and every raised interrupt is delivered exactly once.
void* interruptThreadMain(void*) {
    while (true) {
        dispatch_semaphore_wait(gInterruptSignal, DISPATCH_TIME_FOREVER);
        while (!gStopThread.load(std::memory_order_acquire) &&
               gDelivered.load(std::memory_order_acquire) != gRaised.load(std::memory_order_acquire)) {
            // One AI DMA interrupt, delivered as the hardware would.
            BOOL enabled = OSDisableInterrupts();
            AIDCallback callback = gCallback.load(std::memory_order_acquire);
            if (callback) {
                callback();
            }
            gDelivered.fetch_add(1, std::memory_order_release);
            OSRestoreInterrupts(enabled);
        }
        if (gStopThread.load(std::memory_order_acquire)) {
            return nullptr;
        }
    }
}

void raiseInterrupt() {
    gRaised.fetch_add(1, std::memory_order_acq_rel);
    dispatch_semaphore_signal(gInterruptSignal);
}

}  // namespace

namespace PetariNative::Platform::Audio {

std::uint32_t outputRate() {
    return gRate.load(std::memory_order_acquire);
}

std::size_t pull(std::int16_t* out, std::size_t frames) {
    std::size_t written = 0;
    while (written < frames) {
        if (!gRunning.load(std::memory_order_acquire)) {
            std::memset(out + written * 2, 0, (frames - written) * 2 * sizeof(std::int16_t));
            gPlaying = Block{};
            gPosition = 0;
            return frames;
        }
        if (gPlaying.samples == nullptr || gPosition >= gPlaying.frames) {
            gRegisterLock.lock();
            gPlaying = gRegistered;
            gRegisterLock.unlock();
            gPosition = 0;
            if (gPlaying.samples == nullptr || gPlaying.frames == 0) {
                std::memset(out + written * 2, 0, (frames - written) * 2 * sizeof(std::int16_t));
                return frames;
            }
            raiseInterrupt();  // block start
        }
        const std::size_t n = std::min<std::size_t>(frames - written, gPlaying.frames - gPosition);
        // AI DMA blocks are interleaved R, L (the SDK/hardware order:
        // JASDriver::readDspBuffer and mixExtraTrack put right first; Dolphin
        // AudioCommon/Mixer.cpp, revision
        // 5102a0339c2177575378107b76541e47cc52122d, reads DMA as "RL-ordered").
        // pull() returns L, R.
        const std::int16_t* src = gPlaying.samples + gPosition * 2;
        std::int16_t* dst = out + written * 2;
        for (std::size_t i = 0; i < n; ++i) {
            dst[i * 2] = src[i * 2 + 1];
            dst[i * 2 + 1] = src[i * 2];
        }
        written += n;
        gPosition += static_cast<std::uint32_t>(n);
    }
    return frames;
}

void setSink(const Sink& sink) {
    OS::InterruptGuard guard;
    gSink = sink;
}

std::uint32_t pendingInterrupts() {
    return gRaised.load(std::memory_order_acquire) - gDelivered.load(std::memory_order_acquire);
}

void drainInterrupts() {
    if (OS::interruptsDisabled()) {
        OS::fatal("drainInterrupts() with interrupts disabled would deadlock the AI interrupt thread");
    }
    const std::uint32_t target = gRaised.load(std::memory_order_acquire);
    while (static_cast<std::int32_t>(gDelivered.load(std::memory_order_acquire) - target) < 0) {
        // Let an OS thread holding the CPU be preempted by the delivered interrupt.
        BOOL enabled = OSDisableInterrupts();
        OSRestoreInterrupts(enabled);
        sched_yield();
    }
}

// Order matters: DMA stops, then the sink (after sink.stop returns the
// backend no longer calls pull()), then the interrupt thread; only then is
// state reset. Interrupts raised but not yet delivered when shutdown starts
// are dropped, as when the hardware is reset. Leftover semaphore counts are
// drained so a later AIInit() starts clean.
void shutdown() {
    gRunning.store(false, std::memory_order_release);
    Sink sink;
    bool started;
    {
        OS::InterruptGuard guard;
        sink = gSink;
        started = gSinkStarted;
        gSinkStarted = false;
        gSink = Sink{};
    }
    if (started && sink.stop) {
        sink.stop(sink.user);
    }
    if (gInitialized.exchange(false)) {
        gStopThread.store(true, std::memory_order_release);
        dispatch_semaphore_signal(gInterruptSignal);
        pthread_join(gInterruptThread, nullptr);
        gStopThread.store(false, std::memory_order_release);
    }
    if (gInterruptSignal != nullptr) {
        while (dispatch_semaphore_wait(gInterruptSignal, DISPATCH_TIME_NOW) == 0) {
        }
    }
    gRegisterLock.lock();
    gRegistered = Block{};
    gRegisterLock.unlock();
    gPlaying = Block{};
    gPosition = 0;
    gCallback.store(nullptr);
    gRaised.store(0);
    gDelivered.store(0);
    gRate.store(32000);
}

}  // namespace PetariNative::Platform::Audio

extern "C" {

void AIInit(u8*) {
    if (gInitialized.exchange(true)) {
        return;
    }
    PetariNative::HostAllocationScope hostAllocations;
    if (gInterruptSignal == nullptr) {
        gInterruptSignal = dispatch_semaphore_create(0);
    }
    if (pthread_create(&gInterruptThread, nullptr, interruptThreadMain, nullptr) != 0) {
        OS::fatal("cannot start the AI interrupt thread");
    }
}

AIDCallback AIRegisterDMACallback(AIDCallback cb) {
    return gCallback.exchange(cb, std::memory_order_acq_rel);
}

void AIInitDMA(uintptr_t start, u32 length) {
    if (start == 0 || length == 0 || (length & 3) != 0) {
        OSPanic(__FILE__, __LINE__, "AIInitDMA(): invalid block %p, %u bytes (stereo 16-bit frames required)", reinterpret_cast<void*>(start),
                length);
    }
    gRegisterLock.lock();
    gRegistered.samples = reinterpret_cast<const std::int16_t*>(start);
    gRegistered.frames = length / 4;
    gRegisterLock.unlock();
}

uintptr_t AIGetDMAStartAddr(void) {
    gRegisterLock.lock();
    const uintptr_t start = reinterpret_cast<uintptr_t>(gRegistered.samples);
    gRegisterLock.unlock();
    return start;
}

u32 AIGetDMALength(void) {
    gRegisterLock.lock();
    const u32 length = gRegistered.frames * 4;
    gRegisterLock.unlock();
    return length;
}

void AISetDSPSampleRate(u32 rate) {
    gRate.store(rate == 0 ? 32000 : 48000, std::memory_order_release);
}

u32 AIGetDSPSampleRate(void) {
    return gRate.load(std::memory_order_acquire) == 32000 ? 0 : 1;
}

void AIStartDMA(void) {
    if (!gInitialized.load()) {
        OSPanic(__FILE__, __LINE__, "AIStartDMA() before AIInit()");
    }
    PAudio::Sink sink;
    bool start = false;
    {
        OS::InterruptGuard guard;
        if (!gSinkStarted && gSink.start) {
            gSinkStarted = true;
            start = true;
            sink = gSink;
        }
    }
    gRunning.store(true, std::memory_order_release);
    if (start) {
        sink.start(PAudio::outputRate(), sink.user);
    }
}

void AIStopDMA(void) {
    gRunning.store(false, std::memory_order_release);
}

}  // extern "C"

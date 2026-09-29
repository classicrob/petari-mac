// Native audio interface (AI). Replaces src/RVL_SDK/ai/ai.c.
//
// Hardware model: the DMA engine plays the registered block; when a block
// starts playing it raises the AI DMA interrupt, and the registers written by
// AIInitDMA during that block are latched when it ends. Natively the backend's
// pull() is the DMA engine. Block registration uses a small spin lock (a few
// instructions), so the realtime thread never waits on OS locks. Interrupts
// are counted and signalled with a dispatch semaphore to an AI interrupt
// thread, which calls the game's DMA callback with interrupts disabled.
// Unlike the hardware, a block the game is late to follow is waited for
// (bounded) rather than replayed; see pull() in petari/platform/audio.hpp.

#include <revolution/ai.h>
#include <revolution/os.h>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include "os_internal.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/audio.hpp"
#include "audio_timing.hpp"

namespace OS = PetariNative::Platform::OS;
namespace PAudio = PetariNative::Platform::Audio;
namespace Timing = PetariNative::Platform::Audio::Timing;

namespace {

// Block registration lock. The realtime pull() path takes it, as do game
// threads (AIInitDMA). Unlike the previous busy spin, os_unfair_lock records
// its owner, which the system uses to attempt to resolve priority inversions
// (os/lock.h): a waiting realtime thread need not spin on a preempted owner.
// No latency guarantee is implied. Critical sections are a few loads and
// stores; there is no trylock retry loop.
class RegisterLock {
public:
    void lock() { os_unfair_lock_lock(&mLock); }
    void unlock() { os_unfair_lock_unlock(&mLock); }

private:
    os_unfair_lock mLock = OS_UNFAIR_LOCK_INIT;
};

struct Block {
    const std::int16_t* samples = nullptr;  // interleaved stereo
    std::uint32_t frames = 0;
    std::uint64_t generation = 0;           // AIInitDMA count when registered
    bool answer = false;                    // registered after a DMA interrupt of this DMA session
};

RegisterLock gRegisterLock;
Block gRegistered;       // AIInitDMA, latched at the next block start
Block gPlaying;          // DMA engine state (pull thread only)
std::uint32_t gPosition; // frames played in gPlaying (pull thread only)
std::atomic<bool> gRunning{false};
std::atomic<std::uint32_t> gRate{32000};
std::uint64_t gGeneration = 0;                 // under gRegisterLock
std::atomic<std::uint64_t> gReplayed{0};       // block starts without a new AIInitDMA

// Elastic DMA (see pull() in petari/platform/audio.hpp). Pull thread only,
// except the limit and the counters.
std::atomic<std::int64_t> gWaitLimit{std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                         std::chrono::microseconds(PAudio::kDefaultRegistrationWaitUs))
                                         .count()};  // steady_clock ticks
bool gWaitArmed = false;                  // gPlaying may be waited for at its end
std::int64_t gWaitStart = 0;              // when the current wait began, 0 if not waiting
std::int64_t gBlockStart = 0;             // when gPlaying started
bool gBlockEndNoted = false;              // its wall time was recorded
std::atomic<std::uint64_t> gWaits{0}, gWaitTimeouts{0}, gDspHolds{0};
std::atomic<std::int64_t> gWorstWait{0};
std::atomic<std::int64_t> gMinBlock{0};   // 0: none since the last take
constexpr std::uint32_t kMinTimedBlockFrames = 256;

std::int64_t steadyNow() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

std::int64_t ticksFromMicroseconds(std::uint32_t us) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::microseconds(us)).count();
}

std::int64_t microsecondsFromTicks(std::int64_t ticks) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::duration(ticks)).count();
}

void lowerMin(std::atomic<std::int64_t>& least, std::int64_t value) {
    std::int64_t current = least.load(std::memory_order_relaxed);
    while ((current == 0 || value < current) && !least.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void raiseMaxTicks(std::atomic<std::int64_t>& worst, std::int64_t value) {
    std::int64_t current = worst.load(std::memory_order_relaxed);
    while (value > current && !worst.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void resetElastic() {
    gWaitArmed = false;
    gWaitStart = 0;
    gBlockStart = 0;
    gBlockEndNoted = false;
}
// Raise times of recent interrupts (steady_clock ticks), for the delivery
// latency diagnostic. Indexed by raise count.
constexpr std::uint32_t kRaiseStamps = 64;
std::atomic<std::int64_t> gRaisedAt[kRaiseStamps];
std::atomic<std::int64_t> gWorstLatency{0};

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
    // Audio deadline: the game must see each DMA interrupt well within a
    // block. Ordinary (even user-interactive) threads were measured waking
    // 2-14 ms after the signal; Mach time-constraint (real-time) scheduling,
    // as audio threads use, wakes promptly.
    PAudio::setRealtimeAudioThread();
    while (true) {
        dispatch_semaphore_wait(gInterruptSignal, DISPATCH_TIME_FOREVER);
        while (!gStopThread.load(std::memory_order_acquire) &&
               gDelivered.load(std::memory_order_acquire) != gRaised.load(std::memory_order_acquire)) {
            // One AI DMA interrupt, delivered as the hardware would.
            BOOL enabled = OSDisableInterrupts();
            {
                const std::uint32_t n = gDelivered.load(std::memory_order_relaxed);
                const std::int64_t lag = std::chrono::steady_clock::now().time_since_epoch().count() -
                                         gRaisedAt[n % kRaiseStamps].load(std::memory_order_relaxed);
                std::int64_t worst = gWorstLatency.load(std::memory_order_relaxed);
                while (lag > worst && !gWorstLatency.compare_exchange_weak(worst, lag, std::memory_order_relaxed)) {
                }
            }
            if (Timing::enabled.load(std::memory_order_relaxed)) {
                // Record this DMA interrupt (its block's start) under the
                // next generation; AIInitDMA measures against the records.
                const std::uint32_t n = gDelivered.load(std::memory_order_relaxed);
                const std::int64_t raised = gRaisedAt[n % kRaiseStamps].load(std::memory_order_relaxed);
                const std::int64_t delivered = Timing::now();
                Timing::raiseMax(Timing::worstDmaDeliver, delivered - raised);
                const std::uint64_t generation = Timing::dmaGeneration.load(std::memory_order_relaxed) + 1;
                Timing::DmaRecord& record = Timing::dmaRecords[generation % Timing::kDmaRecords];
                record.raisedAt.store(raised, std::memory_order_relaxed);
                record.deliveredAt.store(delivered, std::memory_order_relaxed);
                record.dspAtDelivery.store(Timing::dspDelivered.load(std::memory_order_relaxed), std::memory_order_relaxed);
                Timing::dmaGeneration.store(generation, std::memory_order_release);
            }
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
    const std::uint32_t n = gRaised.load(std::memory_order_relaxed);
    gRaisedAt[n % kRaiseStamps].store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
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
            resetElastic();
            return frames;
        }
        if (gPlaying.samples == nullptr || gPosition >= gPlaying.frames) {
            if (gPlaying.samples != nullptr && !gBlockEndNoted) {
                // The block's last frame has been pulled: its wall time is
                // the time the game had to register the next one.
                gBlockEndNoted = true;
                if (gBlockStart != 0 && gPlaying.frames >= kMinTimedBlockFrames) {
                    lowerMin(gMinBlock, std::max<std::int64_t>(1, steadyNow() - gBlockStart));
                }
            }
            gRegisterLock.lock();
            const Block next = gRegistered;
            gRegisterLock.unlock();
            const std::uint64_t previous = gPlaying.samples != nullptr ? gPlaying.generation : 0;
            const bool replay = previous != 0 && next.generation == previous;
            if (replay && gWaitArmed && gCallback.load(std::memory_order_acquire) != nullptr) {
                const std::int64_t now = steadyNow();
                if (gWaitStart == 0) {
                    gWaitStart = now;
                }
                if (now - gWaitStart < gWaitLimit.load(std::memory_order_relaxed)) {
                    return written;  // the engine waits for the game's registration
                }
                gWaitTimeouts.fetch_add(1, std::memory_order_relaxed);
                gWaitStart = 0;
            } else if (!replay && gWaitStart != 0) {
                gWaits.fetch_add(1, std::memory_order_relaxed);
                raiseMaxTicks(gWorstWait, steadyNow() - gWaitStart);
                gWaitStart = 0;
            }
            gPlaying = next;
            if (replay) {
                // The hardware replays the block: the game did not register
                // the next one in time (for JAudio2, a late audio thread).
                gReplayed.fetch_add(1, std::memory_order_relaxed);
            }
            // Only a block the game registered in answer to an interrupt is
            // waited for; a replayed one (already timed out) is not.
            gWaitArmed = !replay && gPlaying.answer;
            gPosition = 0;
            gBlockStart = steadyNow();
            gBlockEndNoted = false;
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

void setRegistrationWaitLimit(std::uint32_t microseconds) {
    gWaitLimit.store(ticksFromMicroseconds(microseconds), std::memory_order_relaxed);
}

DmaStats takeDmaStats() {
    DmaStats st;
    st.waits = gWaits.exchange(0, std::memory_order_relaxed);
    st.worstWaitUs = microsecondsFromTicks(gWorstWait.exchange(0, std::memory_order_relaxed));
    st.waitTimeouts = gWaitTimeouts.exchange(0, std::memory_order_relaxed);
    st.minBlockUs = microsecondsFromTicks(gMinBlock.exchange(0, std::memory_order_relaxed));
    st.dspHolds = gDspHolds.exchange(0, std::memory_order_relaxed);
    return st;
}

void setTimingDiagnostics(bool enabled) {
    Timing::enabled.store(enabled, std::memory_order_relaxed);
}

TimingStats takeTimingStats() {
    const auto us = [](std::atomic<std::int64_t>& worst) { return Timing::toMicroseconds(worst.exchange(0, std::memory_order_relaxed)); };
    TimingStats st;
    st.dmaDeliverUs = us(Timing::worstDmaDeliver);
    st.latestRaiseToRegisterUs = us(Timing::worstLatestRaiseToRegister);
    st.latestDeliverToRegisterUs = us(Timing::worstLatestDeliverToRegister);
    st.oldestRaiseToRegisterUs = us(Timing::worstOldestRaiseToRegister);
    st.generationsPerRegistration = Timing::worstGenerationsPerRegistration.exchange(0, std::memory_order_relaxed);
    st.dspInterruptsBeforeRegister = Timing::worstDspBetween.exchange(0, std::memory_order_relaxed);
    st.dspDeliverUs = us(Timing::worstDspDeliver);
    st.subframeRenderUs = us(Timing::worstSubframeRender);
    st.frameUs = us(Timing::worstFrame);
    st.registrationsMissingBlocks = Timing::registrationsMissingBlocks.exchange(0, std::memory_order_relaxed);
    st.registrationsTruncated = Timing::registrationsTruncated.exchange(0, std::memory_order_relaxed);
    return st;
}

bool setRealtimeAudioThread() {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    const auto ticks = [&](double ms) { return static_cast<std::uint32_t>(ms * 1e6 * timebase.denom / timebase.numer); };
    thread_time_constraint_policy_data_t policy;
    policy.period = 0;                 // event driven
    policy.computation = ticks(0.5);   // per wake-up
    policy.constraint = ticks(2.0);    // done within 2 ms of waking
    policy.preemptible = TRUE;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

std::uint32_t takeWorstInterruptLatencyMicroseconds() {
    const std::int64_t ticks = gWorstLatency.exchange(0, std::memory_order_relaxed);
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::duration(ticks)).count());
}

std::uint64_t replayedBlocks() {
    return gReplayed.load(std::memory_order_relaxed);
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
    gReplayed.store(0);
    gPlaying = Block{};
    gPosition = 0;
    resetElastic();
    gCallback.store(nullptr);
    gRaised.store(0);
    gDelivered.store(0);
    gRate.store(32000);
}

}  // namespace PetariNative::Platform::Audio

extern "C" {

// JASDriver::readDspBuffer (PETARI_NATIVE): the DSP frame was not ready.
void petari_audio_note_dsp_hold(void) {
    gDspHolds.fetch_add(1, std::memory_order_relaxed);
}

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
    const bool answer = gRaised.load(std::memory_order_acquire) != 0;
    gRegisterLock.lock();
    gRegistered.samples = reinterpret_cast<const std::int16_t*>(start);
    gRegistered.frames = length / 4;
    gRegistered.generation = ++gGeneration;
    gRegistered.answer = answer;
    gRegisterLock.unlock();
    if (Timing::enabled.load(std::memory_order_relaxed)) {
        // See audio_timing.hpp for how a registration maps to interrupts.
        const std::uint64_t latest = Timing::dmaGeneration.load(std::memory_order_acquire);
        const std::uint64_t previous = Timing::generationAtRegistration.exchange(latest, std::memory_order_relaxed);
        if (latest > previous) {  // at least one DMA interrupt since the previous registration
            const std::uint64_t oldest = std::max(previous + 1, latest > Timing::kDmaRecords - 1 ? latest - (Timing::kDmaRecords - 1) : 1);
            const Timing::DmaRecord& newest = Timing::dmaRecords[latest % Timing::kDmaRecords];
            const Timing::DmaRecord& first = Timing::dmaRecords[oldest % Timing::kDmaRecords];
            const std::int64_t t = Timing::now();
            Timing::raiseMax(Timing::worstLatestRaiseToRegister, t - newest.raisedAt.load(std::memory_order_relaxed));
            Timing::raiseMax(Timing::worstLatestDeliverToRegister, t - newest.deliveredAt.load(std::memory_order_relaxed));
            Timing::raiseMax(Timing::worstOldestRaiseToRegister, t - first.raisedAt.load(std::memory_order_relaxed));
            Timing::raiseMax(Timing::worstGenerationsPerRegistration, static_cast<std::int64_t>(latest - previous));
            Timing::raiseMax(Timing::worstDspBetween, static_cast<std::int64_t>(Timing::dspDelivered.load(std::memory_order_relaxed) -
                                                                                first.dspAtDelivery.load(std::memory_order_relaxed)));
            if (latest - previous >= 2) {
                Timing::registrationsMissingBlocks.fetch_add(1, std::memory_order_relaxed);
            }
            if (latest - previous > Timing::kDmaRecords - 1) {
                Timing::registrationsTruncated.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
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

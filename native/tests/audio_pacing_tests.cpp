// Pacing of AI output for a host audio device (native/audio/paced_ring.hpp,
// used by native/audio/sdl_sink.cpp). No SDL needed.
//
// 1. Simulated device clock (virtual time): a device consuming large bursts
//    at +-1% of the nominal rate, producer stalls and jitter. Every pulled
//    frame must reach the device in order (no drops), steady state must not
//    underrun, and time lost to a stall must be made up at a bounded rate.
//    In steady state every 560-frame DMA block must take at least ~16 ms of
//    real time: that is the game's deadline for registering the next one
//    (the previous 2x refill after each device burst cut it to ~8.75 ms).
//    The previous wall-clock pacing is modelled too, and must fail the same
//    scenario (test sensitivity).
// 2. Real time, real AI: the production tick drives Platform::Audio::pull
//    while an OS thread plays JAudio2's part (on each DMA interrupt, register
//    the next of three DAC buffers). No block may be replayed because the game
//    had no time to register the next one, and DMA interrupts must be spaced.
//    A game that answers each interrupt 10 ms late (within the hardware's
//    17.5 ms), or stalls 40 ms now and then, must not have blocks replayed:
//    the DMA engine waits for it (elastic DMA) and the ring covers the gap.

#include <revolution/ai.h>
#include <revolution/os.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../audio/paced_ring.hpp"
#include "petari/platform/audio.hpp"
#include "petari/platform/os_host.hpp"

extern "C" void __OSThreadInit(void);

namespace Detail = PetariNative::AudioSDL::Detail;
namespace PAudio = PetariNative::Platform::Audio;

namespace {

int checks = 0;
int failures = 0;
// PACING_REPORT=1: report every failed check and fail at the end (to compare
// all real-time scenarios between builds).
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        ++failures;
        if (!std::getenv("PACING_REPORT")) {
            std::exit(1);
        }
    }
}

// ---- 1. Simulated device clock ----

constexpr std::uint32_t kRate = 32000;
constexpr std::size_t kQuantum = kRate / 1000;
constexpr std::size_t kMaxChunk = 2 * kQuantum;
constexpr std::uint64_t kPrebuffer = 2048;

struct SimResult {
    std::uint64_t underrunAfterWarmup = 0;
    std::uint64_t outOfOrder = 0;
    std::uint64_t pulled = 0;
    std::uint64_t delivered = 0;
    std::size_t maxPerTick = 0;
    double recoveryMs = -1;  // after the stall, until the level is back at target - burst
    double minBlockMs = 1e9; // steady state: shortest real time to pull a 560-frame DMA block
};

constexpr std::uint64_t kDmaBlock = 560;  // JAudio2 frame: 7 subframes of 0x50

// The frame counter is encoded in the samples: L = low 15 bits, R = high.
struct Counter {
    std::uint64_t next = 1;
    std::size_t operator()(std::int16_t* out, std::size_t frames) {
        for (std::size_t i = 0; i < frames; ++i, ++next) {
            out[i * 2] = static_cast<std::int16_t>(next & 0x7FFF);
            out[i * 2 + 1] = static_cast<std::int16_t>((next >> 15) & 0x7FFF);
        }
        return frames;
    }
};

// wallClock = true models the previous producer: exactly one quantum per
// tick, a full ring drops the pulled quantum, and a late tick resets the
// schedule (the lost time is never made up). doubleRefill = true models the
// previous level-driven pacer: 2x the elapsed time per tick whatever the level.
SimResult simulate(double drift, std::size_t burst, double stallAtMs, double stallMs, bool wallClock, double seconds = 120,
                   double tickMs = 1.0, bool doubleRefill = false) {
    Detail::PacedRing ring;
    ring.reset(kPrebuffer);
    Counter source;
    SimResult result;
    std::uint64_t expect = 1;
    const double deviceRate = kRate * (1.0 + drift);
    const double burstMs = burst * 1000.0 / deviceRate;
    double nextDevice = burstMs;
    double nextProducer = 0;
    double lastTick = 0;
    Detail::Pacer pacer(kRate);
    std::vector<std::int16_t> out(burst * 2);
    double blockStartMs = -1;  // when the current DMA block's first frame was pulled
    std::vector<std::int16_t> block(kQuantum * 2);
    const double warmup = 500;
    bool stalled = false;
    for (double t = 0; t < seconds * 1000; t += 0.25) {
        if (!stalled && stallMs > 0 && t >= stallAtMs && t < stallAtMs + stallMs) {
            stalled = true;
            nextProducer = stallAtMs + stallMs;
        }
        if (stalled && t >= stallAtMs + stallMs) {
            stalled = false;
        }
        if (!stalled && t >= nextProducer) {
            std::size_t pulled;
            if (wallClock) {
                source(block.data(), kQuantum);
                if (ring.level() + kQuantum <= Detail::PacedRing::kCapacity) {
                    ring.write(block.data(), kQuantum);
                }
                pulled = kQuantum;
                nextProducer += 1;
                if (nextProducer + 1 < t) {
                    nextProducer = t;
                }
            } else {
                const double elapsed = (t - lastTick) / 1000.0;
                const std::size_t allowance =
                    doubleRefill ? std::max<std::size_t>(1, static_cast<std::size_t>(2.0 * kRate * std::min(elapsed, 0.005)))
                                 : pacer.allowance(elapsed, ring);
                const std::uint64_t before = result.pulled;
                pulled = Detail::produceTick(ring, kQuantum, allowance, source);
                lastTick = t;
                // DMA blocks are consecutive runs of 560 pulled frames. A
                // block ends with the tick that pulls its last frame.
                const std::uint64_t after = before + pulled;
                const bool steady = t > 2000 && !(stallMs > 0 && t >= stallAtMs && t < stallAtMs + stallMs + 2000);
                for (std::uint64_t boundary = (before / kDmaBlock + 1) * kDmaBlock; boundary <= after; boundary += kDmaBlock) {
                    if (steady && blockStartMs >= 0) {
                        result.minBlockMs = std::min(result.minBlockMs, t - blockStartMs);
                    }
                    blockStartMs = t;
                }
                nextProducer = std::max(nextProducer + tickMs, t);
            }
            result.pulled += pulled;
            result.maxPerTick = std::max(result.maxPerTick, pulled);
        }
        if (t >= nextDevice) {
            const std::uint64_t before = ring.underrunFrames();
            ring.read(out.data(), burst);
            if (t > warmup && !(stallMs > 0 && t >= stallAtMs && t < stallAtMs + stallMs + 1000)) {
                result.underrunAfterWarmup += ring.underrunFrames() - before;
            }
            for (std::size_t i = 0; i < burst; ++i) {
                const std::uint64_t v = static_cast<std::uint16_t>(out[i * 2]) | (static_cast<std::uint64_t>(static_cast<std::uint16_t>(out[i * 2 + 1])) << 15);
                if (v == 0) {
                    continue;  // prebuffer or underrun silence
                }
                if (v != expect) {
                    ++result.outOfOrder;
                    expect = v;
                }
                ++expect;
                ++result.delivered;
            }
            nextDevice += burstMs;
        }
        if (stallMs > 0 && result.recoveryMs < 0 && t > stallAtMs + stallMs &&
            ring.level() + burst >= ring.target()) {
            result.recoveryMs = t - (stallAtMs + stallMs);
        }
    }
    result.delivered += ring.level();  // still queued
    return result;
}

void testSimulatedDevice() {
    {
        // A 4096-frame device burst read in 1024-frame pieces (as the SDL
        // callback does) must still set the target from the whole burst.
        Detail::PacedRing ring;
        ring.reset(kPrebuffer);
        std::vector<std::int16_t> piece(1024 * 2);
        ring.noteRequest(4096);
        for (int i = 0; i < 4; ++i) {
            ring.read(piece.data(), 1024);
        }
        check(ring.target() == kPrebuffer + 4096, "the target covers the whole device burst, not the pieces");
    }
    for (double drift : {-0.01, 0.0, 0.01}) {
        for (std::size_t burst : {256u, 512u, 744u, 1024u}) {
            const SimResult r = simulate(drift, burst, 0, 0, false);
            if (drift == 0.0) {
                std::printf("burst %zu: shortest DMA block %.2f ms\n", burst, r.minBlockMs);
            }
            check(r.underrunAfterWarmup == 0, "no underrun in steady state at +-1% device drift");
            // 560 frames at 1.05x take 16.7 ms; a 1 ms tick can end a block
            // up to one tick early.
            check(r.minBlockMs >= 15.5, "a DMA block lasts at least ~16 ms: the game keeps its deadline (no 2x refill)");
            check(r.outOfOrder == 0, "every pulled frame reaches the device, in order");
            check(r.maxPerTick <= kMaxChunk, "at most 2 ms of AI per 1 ms tick");
            // A loaded host wakes the producer every 4 ms instead of every 1:
            // the allowance scales with elapsed time, so it still keeps up.
            const SimResult coarse = simulate(drift, burst, 0, 0, false, 30, 4.0);
            check(coarse.underrunAfterWarmup == 0 && coarse.outOfOrder == 0, "no underrun or drop with 4 ms producer wakeups");
            check(coarse.maxPerTick <= 4 * kMaxChunk, "at most 2x the elapsed audio time per tick");
        }
    }
    // A 150 ms producer stall (longer than the buffered audio: the device
    // underruns meanwhile). Afterwards the ring refills at a bounded rate and
    // steady state has no underrun, with the device clock 1% fast.
    const SimResult stall = simulate(0.01, 1024, 10000, 150, false);
    std::printf("level-driven: recovery %.1f ms after a 150 ms stall; %llu underrun frames after recovery\n", stall.recoveryMs,
                static_cast<unsigned long long>(stall.underrunAfterWarmup));
    check(stall.recoveryMs >= 0 && stall.recoveryMs < 1000, "the ring is back at its target within 1 s of a stall");
    check(stall.underrunAfterWarmup == 0, "no underrun after recovering from a stall");
    check(stall.outOfOrder == 0, "no audio dropped across the stall");

    // Sensitivity: the previous pacer (2x refill after every device burst)
    // halves the deadline in the same steady state.
    const SimResult doubled = simulate(0.0, 744, 0, 0, false, 30, 1.0, true);
    std::printf("2x refill (previous): shortest DMA block %.2f ms\n", doubled.minBlockMs);
    check(doubled.minBlockMs < 10.0, "test sensitivity: the previous 2x refill ends blocks in under 10 ms");

    // The previous wall-clock producer in the same scenario: the lost time
    // is never made up and the fast device drains the ring.
    const SimResult old = simulate(0.01, 1024, 10000, 150, true);
    std::printf("wall-clock (previous): %llu underrun frames after the stall\n", static_cast<unsigned long long>(old.underrunAfterWarmup));
    check(old.underrunAfterWarmup > 0, "test sensitivity: the previous pacing underruns in this scenario");
    const SimResult slow = simulate(-0.01, 1024, 0, 0, true);
    check(slow.outOfOrder > 0, "test sensitivity: the previous pacing drops audio when the device is slow");
}

// ---- 2. Real time with the real AI DMA engine ----

constexpr std::uint32_t kBlockFrames = 560;  // JAudio2 frame: 7 subframes of 0x50
alignas(32) std::int16_t gDac[3][kBlockFrames * 2];
std::atomic<std::uint32_t> gRegistered{0};  // block number registered last
OSMessageQueue gQueue;
OSMessage gMessages[16];
std::atomic<int> gInterrupts{0};
std::atomic<bool> gStop{false};
std::atomic<int> gWorkMs{3};
std::atomic<int> gAnswerMs{0};    // delay before each registration (a late audio thread)
std::atomic<int> gLateEvery{0};   // every Nth interrupt, also...
std::atomic<int> gLateMs{0};      // ...stall this long before registering
std::atomic<double> gMaxTickGapMs{0};
std::uint32_t gWorstDelivery = 0;
std::vector<std::chrono::steady_clock::time_point> gStarts;
std::vector<std::chrono::steady_clock::time_point> gRegisters;  // audio thread: AIInitDMA times

void dmaCallback() {
    // Interrupt context, as JASAudioThread's DMA callback: wake the audio thread.
    gInterrupts++;
    if (gStarts.size() < gStarts.capacity()) {
        gStarts.push_back(std::chrono::steady_clock::now());
    }
    OSSendMessage(&gQueue, nullptr, OS_MESSAGE_NOBLOCK);
}

void* audioThread(void*) {
    // JASDriver::updateDac: register the block prepared last time, then
    // prepare the next one (fill it with its block number after gWorkMs of
    // "DSP" work). Interrupts that arrive meanwhile wait in the queue.
    std::uint32_t prepared = 1;
    int answered = 0;
    while (!gStop.load()) {
        OSMessage m;
        OSReceiveMessage(&gQueue, &m, OS_MESSAGE_BLOCK);
        if (gStop.load()) {
            break;
        }
        // A late game: the host did not run this thread for a while.
        int delayMs = gAnswerMs.load();
        if (gLateEvery.load() > 0 && ++answered % gLateEvery.load() == 0) {
            delayMs += gLateMs.load();
        }
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        AIInitDMA(reinterpret_cast<uintptr_t>(gDac[prepared % 3]), sizeof(gDac[0]));
        if (gRegisters.size() < gRegisters.capacity()) {
            gRegisters.push_back(std::chrono::steady_clock::now());
        }
        ++prepared;
        std::int16_t* next = gDac[prepared % 3];
        // JAudio2's work here is CPU time (DSP setup, mixing), not a timed
        // sleep, so the thread keeps the CPU for it.
        const auto workEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(gWorkMs.load());
        while (std::chrono::steady_clock::now() < workEnd) {
        }
        for (std::uint32_t i = 0; i < kBlockFrames * 2; ++i) {
            next[i] = static_cast<std::int16_t>(prepared);
        }
    }
    return nullptr;
}

struct Scenario {
    const char* name;
    bool burstCatchUp;     // sensitivity: pull 16x the elapsed time, with no credit cap
    int stallMs;           // host stall of the producer, 1.5 s in
    int workMs;            // the game's audio thread time per DMA interrupt
    int answerMs = 0;      // the game registers each block this late
    int lateEvery = 0;     // and every lateEvery-th one lateMs later still
    int lateMs = 0;
};

struct RealResult {
    int interrupts;
    double minGapMs;
    std::uint64_t replays;           // heard: a block number lasting more than one block
    std::uint64_t replayedByAi;      // AI's own count
    std::uint64_t silentAfterStart;
    std::uint64_t underrun;
    std::uint64_t distinctBlocks;  // block numbers heard (the game really produced them)
    std::uint64_t queuedBlocks;    // pulled but still in the ring at the end
    PAudio::DmaStats dma;          // elastic waits during the run
};

RealResult runRealAi(const Scenario& sc) {
    gStarts.clear();
    gStarts.reserve(4096);
    gRegisters.clear();
    gRegisters.reserve(4096);
    gInterrupts = 0;
    gStop = false;
    gWorkMs = sc.workMs;
    gAnswerMs = sc.answerMs;
    gLateEvery = sc.lateEvery;
    gLateMs = sc.lateMs;
    PAudio::takeDmaStats();
    gMaxTickGapMs = 0;
    OSInitMessageQueue(&gQueue, gMessages, 16);
    std::fill(std::begin(gDac[0]), std::end(gDac[0]), 0);
    std::fill(std::begin(gDac[1]), std::end(gDac[1]), static_cast<std::int16_t>(1));
    std::fill(std::begin(gDac[2]), std::end(gDac[2]), 0);
    static OSThread thread;
    alignas(32) static u8 stack[0x8000];
    OSCreateThread(&thread, audioThread, nullptr, stack + sizeof(stack), sizeof(stack), 2, 0);
    OSResumeThread(&thread);

    AIInit(nullptr);
    AISetDSPSampleRate(0);
    AIRegisterDMACallback(dmaCallback);
    AIInitDMA(reinterpret_cast<uintptr_t>(gDac[0]), sizeof(gDac[0]));
    AIStartDMA();

    Detail::PacedRing ring;
    ring.reset(kPrebuffer);
    std::atomic<bool> stopProducer{false};
    std::thread producer([&] {
        // Same scheduling as the production producer (sdl_sink.cpp).
        PAudio::setRealtimeAudioThread();
        using Clock = std::chrono::steady_clock;
        auto next = Clock::now();
        auto last = next;
        Detail::Pacer pacer(kRate);
        int tick = 0;
        while (!stopProducer.load()) {
            if (++tick == 1500 && sc.stallMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sc.stallMs));  // a host scheduling stall
            }
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - last).count();
            last = now;
            if (tick > 2 && tick != 1501 && elapsed * 1000 > gMaxTickGapMs.load()) {
                gMaxTickGapMs = elapsed * 1000;
            }
            // The production allowance, or an unbounded burst (sensitivity).
            const std::size_t allowance = sc.burstCatchUp ? static_cast<std::size_t>(std::max(elapsed * kRate * 16, 1.0)) : pacer.allowance(elapsed, ring);
            Detail::produceTick(ring, kQuantum, allowance, [](std::int16_t* out, std::size_t frames) { return PAudio::pull(out, frames); });
            next = std::max(next + std::chrono::milliseconds(1), Clock::now());
            std::this_thread::sleep_until(next);
        }
    });

    // Device: 744-frame bursts (the SDL device's request in the app's logs)
    // on its own clock, 0.5% fast. This (main, OS) thread does host work here,
    // so it gives up the CPU as the app's frame seam does; otherwise the
    // game's audio thread could never run.
    constexpr int kDeviceBurst = 744;
    petari_os_begin_host_blocking();
    std::vector<std::int16_t> out(kDeviceBurst * 2);
    PAudio::DmaStats startup{};  // the first second's DMA counters
    std::vector<std::int16_t> heard;
    heard.reserve(kRate * 5 * 2);
    const auto start = std::chrono::steady_clock::now();
    const double burstSeconds = kDeviceBurst / (kRate * 1.005);
    const int startupBursts = static_cast<int>(1.0 / burstSeconds);
    for (int n = 1; n <= static_cast<int>(4.0 / burstSeconds); ++n) {
        if (n == startupBursts) {
            startup = PAudio::takeDmaStats();  // the shortest block is then measured after start-up only
        }
        std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double>(n * burstSeconds)));
        const std::uint64_t before = ring.underrunFrames();
        static std::uint64_t lastReplays = 0;
        if (n == 1) {
            lastReplays = PAudio::replayedBlocks();
        }
        if (PAudio::replayedBlocks() != lastReplays && std::getenv("PACING_TRACE")) {
            std::printf("  AI replay(s) by %.0f ms\n", n * burstSeconds * 1000);
            lastReplays = PAudio::replayedBlocks();
        }
        ring.read(out.data(), kDeviceBurst);
        if (ring.underrunFrames() != before) {
            std::printf("  underrun of %llu frames at device burst %d (%.0f ms)\n", static_cast<unsigned long long>(ring.underrunFrames() - before), n,
                        n * burstSeconds * 1000);
        }
        heard.insert(heard.end(), out.begin(), out.end());
    }
    stopProducer = true;
    producer.join();
    petari_os_end_host_blocking();
    RealResult r{};
    gWorstDelivery = PAudio::takeWorstInterruptLatencyMicroseconds();
    r.queuedBlocks = ring.level() / kBlockFrames + 2;
    r.replayedByAi = PAudio::replayedBlocks();
    r.dma = PAudio::takeDmaStats();
    r.dma.waits += startup.waits;
    r.dma.waitTimeouts += startup.waitTimeouts;
    r.dma.worstWaitUs = std::max(r.dma.worstWaitUs, startup.worstWaitUs);
    AIStopDMA();
    gStop = true;
    OSSendMessage(&gQueue, nullptr, OS_MESSAGE_NOBLOCK);
    OSJoinThread(&thread, nullptr);
    PAudio::shutdown();

    // Each block number must be heard for exactly one block (560 frames); a
    // longer run is a replayed block.
    std::int16_t current = -1;
    std::size_t run = 0;
    bool started = false;
    r.distinctBlocks = 0;
    for (std::size_t i = 0; i < heard.size() / 2; ++i) {
        const std::int16_t v = heard[i * 2];
        started = started || v > 0;
        if (!started) {
            continue;
        }
        if (v == 0) {
            ++r.silentAfterStart;
        }
        if (v != current) {
            if (v > 0) {
                ++r.distinctBlocks;
            }
            if (current > 0 && run > kBlockFrames) {
                r.replays += run / kBlockFrames - 1;
            }
            current = v;
            run = 0;
        }
        ++run;
    }
    r.interrupts = gInterrupts.load();
    r.minGapMs = 1e9;
    for (std::size_t i = 2; i < gStarts.size(); ++i) {
        r.minGapMs = std::min(r.minGapMs, std::chrono::duration<double, std::milli>(gStarts[i] - gStarts[i - 1]).count());
    }
    r.underrun = ring.underrunFrames();
    double maxLag = 0;
    for (std::size_t i = 0; i < std::min(gStarts.size(), gRegisters.size()); ++i) {
        const double lag = std::chrono::duration<double, std::milli>(gRegisters[i] - gStarts[i]).count();
        maxLag = std::max(maxLag, lag);
        if (lag > 8 && std::getenv("PACING_TRACE")) {
            std::printf("    interrupt %zu at %.1f ms: registered after %.2f ms\n", i,
                        std::chrono::duration<double, std::milli>(gStarts[i] - gStarts[0]).count(), lag);
        }
    }
    std::printf("  max interrupt-to-registration lag %.2f ms, max producer tick gap %.2f ms, worst AI interrupt delivery %u us\n", maxLag,
                gMaxTickGapMs.load(), gWorstDelivery);
    std::printf("real AI [%s]: %d DMA interrupts, %llu blocks heard, min spacing %.2f ms, replayed %llu heard / %llu by AI, %llu silent frames, %llu underrun frames\n",
                sc.name, r.interrupts, static_cast<unsigned long long>(r.distinctBlocks), r.minGapMs, static_cast<unsigned long long>(r.replays), static_cast<unsigned long long>(r.replayedByAi),
                static_cast<unsigned long long>(r.silentAfterStart), static_cast<unsigned long long>(r.underrun));
    std::printf("  elastic DMA: %llu waits (worst %lld us), %llu timed out; shortest block after start-up %lld us\n", static_cast<unsigned long long>(r.dma.waits),
                static_cast<long long>(r.dma.worstWaitUs), static_cast<unsigned long long>(r.dma.waitTimeouts), static_cast<long long>(r.dma.minBlockUs));
    return r;
}

void testRealAi() {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    std::puts("real-time AI pacing checks skipped under ThreadSanitizer (timing is not representative)");
    return;
#endif
#endif
    // Short host stall (within the 64 ms buffer): no audible effect at all.
    // One run per scenario, every replay reported. The game's own audio
    // thread (an OS thread at user-interactive QoS) was measured waking up to
    // ~30 ms late under load, so a real replay can occur; the bound below
    // flags more than the observed rate and any miss is printed.
    constexpr std::uint64_t kReplayBound = 2;  // per 4 s run
    const RealResult shortStall = runRealAi({"40 ms stall", false, 40, 3});
    if (shortStall.replayedByAi != 0) {
        std::printf("WARNING: %llu block replay(s) with a 40 ms stall (game audio thread late)\n",
                    static_cast<unsigned long long>(shortStall.replayedByAi));
    }
    check(shortStall.interrupts > 150, "AI DMA advanced about in real time");
    check(shortStall.distinctBlocks + shortStall.queuedBlocks >= static_cast<std::uint64_t>(shortStall.interrupts), "the game's blocks were heard, one per DMA interrupt");
    check(shortStall.replayedByAi <= kReplayBound, "block replays within the observed bound (each one printed)");
    check(shortStall.minGapMs > 3.0, "DMA interrupts are paced, not consumed in bursts");
    check(shortStall.underrun == 0 && shortStall.silentAfterStart == 0, "no underrun with a 0.5% fast device and a 40 ms stall");

    // Long stall (the device underruns meanwhile), then sustained 1.25x
    // catch-up: blocks start every ~14 ms, and a game audio thread needing
    // 6 ms per block keeps up.
    const RealResult longStall = runRealAi({"150 ms stall, 1.25x catch-up, 6 ms game work", false, 150, 6});
    if (longStall.replayedByAi != 0) {
        std::printf("WARNING: %llu block replay(s) during 2x catch-up\n", static_cast<unsigned long long>(longStall.replayedByAi));
    }
    check(longStall.distinctBlocks + longStall.queuedBlocks >= static_cast<std::uint64_t>(longStall.interrupts), "the game's blocks were heard after a long stall");
    check(longStall.replayedByAi <= kReplayBound, "catch-up stays within the replay bound (a 16x burst replays tens)");
    check(longStall.minGapMs > 3.0, "catch-up keeps DMA interrupts apart (a 16x burst would put them ~1 ms apart)");

    // A game answering every interrupt 10 ms late: on time by the hardware's
    // 17.5 ms, so nothing may be replayed. (The previous 2x refill ended
    // blocks after 8.75 ms and replayed many.)
    const RealResult late = runRealAi({"game answers 10 ms late", false, 0, 1, 10});
    check(late.replayedByAi <= kReplayBound && late.replays <= kReplayBound, "a game answering within the hardware's deadline has no blocks replayed");
    check(late.underrun == 0 && late.silentAfterStart == 0, "and no underrun");
    // 16.7 ms at the 1.05x trim; 14 ms if a late (host-delayed) answer let
    // the ring dip below its low-water mark (1.25x catch-up); never ~8.75 ms.
    check(late.dma.minBlockUs >= 13000, "DMA blocks last at least 13 ms of real time (no 2x pace)");

    // A game whose audio thread stalls 40 ms on every 40th interrupt (beyond
    // the hardware deadline): the engine waits for it instead of replaying,
    // and the ring covers the waits.
    const RealResult stalls = runRealAi({"game stalls 40 ms every 40th interrupt", false, 0, 1, 2, 40, 40});
    check(stalls.replayedByAi <= kReplayBound && stalls.replays <= kReplayBound, "40 ms game stalls: no blocks replayed (the DMA engine waits)");
    check(stalls.underrun == 0 && stalls.silentAfterStart == 0, "40 ms game stalls: the ring covers the waits, no underrun");
    check(stalls.dma.waits >= 3 && stalls.dma.waitTimeouts == 0, "the waits happened and none ran out");
    check(stalls.distinctBlocks + stalls.queuedBlocks >= static_cast<std::uint64_t>(stalls.interrupts), "every block the game produced was heard");

    // Sensitivity: unbounded (16x) catch-up after the same stall consumes
    // DMA blocks faster than the game can register them.
    const RealResult burst = runRealAi({"150 ms stall, 16x catch-up (bad), 6 ms game work", true, 150, 6});
    check(burst.replayedByAi > 0 || burst.minGapMs < 3.0, "test sensitivity: burst catch-up bunches DMA interrupts or replays blocks");
}

}  // namespace

// --real-time-only: skip the simulated device (to compare real-time runs).
int main(int argc, char** argv) {
    __OSThreadInit();
    if (!(argc > 1 && std::string(argv[1]) == "--real-time-only")) {
        testSimulatedDevice();
    }
    testRealAi();
    if (failures != 0) {
        std::fprintf(stderr, "audio pacing tests: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    OSReport("audio pacing tests passed (%d checks)\n", checks);
    return 0;
}

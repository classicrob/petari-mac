#include <petari/audio_sdl.hpp>
#include <petari/host_allocation.hpp>
#include <petari/platform/audio.hpp>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <condition_variable>
#include <mutex>

#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "paced_ring.hpp"

namespace {
SDL_AudioStream* device;
bool initialized;
std::atomic<bool> running{false};
std::atomic<std::uint64_t> submitted{0};
std::atomic<bool> failed{false};

// SDL requests audio in device-sized bursts, but AI DMA must advance about in
// real time so the game's interrupt and audio threads can prepare each next
// block. The producer owns AI::pull and keeps the ring at a target level; the
// realtime SDL callback only drains it. See paced_ring.hpp for why pacing is
// level-driven (locked to the device clock) rather than wall-clock-driven.
constexpr std::uint64_t prebuffer = 2048;
PetariNative::AudioSDL::Detail::PacedRing ring;
std::atomic<bool> stopProducer{false};
std::thread producer;

// PETARI_AUDIO_DIAG=1 diagnostics. The realtime producer and the SDL callback
// only update these atomics; a separate reporter thread (started by start(),
// joined by stop()) does all formatting and stderr I/O, so the realtime paths
// never block on the stderr lock or spend their budget on formatting.
bool diagnostics = false;  // set by start() before the threads start
std::atomic<std::int64_t> worstTickGapUs{0};     // producer: time between ticks
std::atomic<std::int64_t> worstPullUs{0};        // producer: one tick's AI pulls (includes lock waits)
std::atomic<std::uint64_t> pulledFrames{0};
std::atomic<std::int64_t> worstCallbackGapUs{0}; // SDL callback: time between calls
std::atomic<std::uint64_t> callbacks{0};
std::atomic<std::uint64_t> requestedFrames{0};
std::int64_t lastCallbackUs = 0;                 // SDL callback thread only
std::mutex reporterLock;
std::condition_variable reporterWake;
bool reporterStop = false;                       // under reporterLock
std::thread* reporter = nullptr;                 // joined and deleted by stop(); never left to static destruction

std::int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void raiseMax(std::atomic<std::int64_t>& worst, std::int64_t value) {
    std::int64_t current = worst.load(std::memory_order_relaxed);
    while (value > current && !worst.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

// Seconds since the process started, printed on the [audio] lines so they can
// be lined up with other timestamped output (the frame trace, stall reports).
double processSeconds() {
    struct kinfo_proc info {};
    std::size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) {
        return -1.0;
    }
    struct timeval now;
    gettimeofday(&now, nullptr);
    const struct timeval& started = info.kp_proc.p_starttime;
    return static_cast<double>(now.tv_sec - started.tv_sec) + (now.tv_usec - started.tv_usec) / 1e6;
}

void report() {
    PetariNative::HostAllocationScope host;
    std::uint64_t reportedReplays = PetariNative::Platform::Audio::replayedBlocks();
    auto previous = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(reporterLock);
    while (!reporterWake.wait_for(lock, std::chrono::seconds(1), [] { return reporterStop; })) {
        lock.unlock();
        // Counters cover the interval since the previous report, which can be
        // longer than a second if this thread was delayed; it is printed.
        const auto now = std::chrono::steady_clock::now();
        const double interval = std::chrono::duration<double>(now - previous).count();
        previous = now;
        // Ring starvation (underrun) versus late game audio production (AI
        // replaying a block), the ring level, and the largest device request.
        const std::uint64_t replays = PetariNative::Platform::Audio::replayedBlocks();
        std::fprintf(stderr, "[audio] level %llu/%llu frames, largest request %zu, underrun %llu frames, AI replayed %llu blocks (since previous report, %.2f s; at %.3f s process time)\n",
                     static_cast<unsigned long long>(ring.level()), static_cast<unsigned long long>(ring.target()), ring.largestRequest(),
                     static_cast<unsigned long long>(ring.takeUnderrunFrames()), static_cast<unsigned long long>(replays - reportedReplays), interval,
                     processSeconds());
        reportedReplays = replays;
        // The DMA engine: elastic waits for a late game (inaudible while the
        // ring covers them), the shortest block (the game's deadline), and
        // JAudio2's DSP holds (late DSP frames, audible, not replays).
        const auto dma = PetariNative::Platform::Audio::takeDmaStats();
        std::fprintf(stderr, "[audio-dma] waited for %llu late registrations (worst %lld us), %llu waits timed out; shortest block %lld us; "
                     "DSP holds %llu\n",
                     static_cast<unsigned long long>(dma.waits), static_cast<long long>(dma.worstWaitUs),
                     static_cast<unsigned long long>(dma.waitTimeouts), static_cast<long long>(dma.minBlockUs),
                     static_cast<unsigned long long>(dma.dspHolds));
        // Where the audio cycle spent its time (worst values): see
        // native/platform/audio/audio_timing.hpp.
        const auto t = PetariNative::Platform::Audio::takeTimingStats();
        std::fprintf(stderr, "[audio-timing] AI deliver %lld us; registration: %lld us after the latest DMA raise (%lld after its delivery), "
                     "%lld us after the oldest retained unanswered raise, up to %lld DMA interrupts per registration, %llu registrations missing blocks "
                     "(%llu beyond the record ring), "
                     "%lld DSP interrupts before it; DSP deliver %lld us, subframe render %lld us, frame %lld us\n",
                     static_cast<long long>(t.dmaDeliverUs), static_cast<long long>(t.latestRaiseToRegisterUs),
                     static_cast<long long>(t.latestDeliverToRegisterUs), static_cast<long long>(t.oldestRaiseToRegisterUs),
                     static_cast<long long>(t.generationsPerRegistration), static_cast<unsigned long long>(t.registrationsMissingBlocks),
                     static_cast<unsigned long long>(t.registrationsTruncated), static_cast<long long>(t.dspInterruptsBeforeRegister),
                     static_cast<long long>(t.dspDeliverUs), static_cast<long long>(t.subframeRenderUs), static_cast<long long>(t.frameUs));
        // The host side of the ring. Interval aggregates: they help
        // distinguish producer-side from device-side starvation around an
        // underrun, but do not by themselves establish its cause.
        std::fprintf(stderr, "[audio-host] producer: worst tick gap %lld us, worst pull %lld us, pulled %llu frames; device: %llu callbacks, "
                     "worst gap %lld us, requested %llu frames (since previous report, %.2f s)\n",
                     static_cast<long long>(worstTickGapUs.exchange(0, std::memory_order_relaxed)),
                     static_cast<long long>(worstPullUs.exchange(0, std::memory_order_relaxed)),
                     static_cast<unsigned long long>(pulledFrames.exchange(0, std::memory_order_relaxed)),
                     static_cast<unsigned long long>(callbacks.exchange(0, std::memory_order_relaxed)),
                     static_cast<long long>(worstCallbackGapUs.exchange(0, std::memory_order_relaxed)),
                     static_cast<unsigned long long>(requestedFrames.exchange(0, std::memory_order_relaxed)), interval);
        lock.lock();
    }
}

void produce(std::uint32_t rate) {
    PetariNative::HostAllocationScope host;
    using Clock = std::chrono::steady_clock;
    // Wake latency matters here: the producer must run every few ms (the
    // allowance tolerates late ticks, but not tens of ms of them).
    if (!PetariNative::Platform::Audio::setRealtimeAudioThread()) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
    const std::size_t quantum = rate / 1000;  // pull granularity: 1 ms of audio
    PetariNative::AudioSDL::Detail::Pacer pacer(rate);
    auto last = Clock::now();
    auto next = Clock::now();
    while (!stopProducer.load(std::memory_order_acquire)) {
        const auto now = Clock::now();
        const std::size_t allowance = pacer.allowance(std::chrono::duration<double>(now - last).count(), ring);
        if (diagnostics) {
            raiseMax(worstTickGapUs, std::chrono::duration_cast<std::chrono::microseconds>(now - last).count());
        }
        last = now;
        const std::size_t pulled = PetariNative::AudioSDL::Detail::produceTick(ring, quantum, allowance, [](std::int16_t* out, std::size_t frames) {
            return PetariNative::Platform::Audio::pull(out, frames);
        });
        if (diagnostics) {
            raiseMax(worstPullUs, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count());
            pulledFrames.fetch_add(pulled, std::memory_order_relaxed);
        }
        // Ticks are paced in real time; a late tick does not bring extra
        // ticks (the level, not the tick count, decides how much is pulled).
        // A short pull (the DMA engine waiting for the game's next block) is
        // retried on the next tick; the ring covers the gap.
        next = std::max(next + std::chrono::milliseconds(1), Clock::now());
        std::this_thread::sleep_until(next);
    }
}

void SDLCALL fill(void*, SDL_AudioStream* stream, int additionalBytes, int) {
    PetariNative::HostAllocationScope host;
    alignas(16) std::int16_t buffer[1024 * 2];
    int frames = additionalBytes > 0 ? (additionalBytes + 3) / 4 : 0;
    if (diagnostics) {
        const std::int64_t t = nowUs();
        if (lastCallbackUs != 0) {
            raiseMax(worstCallbackGapUs, t - lastCallbackUs);
        }
        lastCallbackUs = t;
        callbacks.fetch_add(1, std::memory_order_relaxed);
        requestedFrames.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
    }
    ring.noteRequest(static_cast<std::size_t>(frames));  // the whole burst, before splitting
    while (frames > 0) {
        const int count = std::min(frames, 1024);
        ring.read(buffer, static_cast<std::size_t>(count));
        if (!SDL_PutAudioStreamData(stream, buffer, count * 4)) {
            failed.store(true, std::memory_order_relaxed);
            return;
        }
        submitted.fetch_add(count, std::memory_order_relaxed);
        frames -= count;
    }
}
void stop(void*) {
    PetariNative::HostAllocationScope host;
    running.store(false);
    stopProducer.store(true, std::memory_order_release);
    if (producer.joinable()) producer.join();
    if (reporter != nullptr) {
        {
            std::lock_guard<std::mutex> guard(reporterLock);
            reporterStop = true;
        }
        reporterWake.notify_all();
        reporter->join();
        delete reporter;
        reporter = nullptr;
    }
    PetariNative::Platform::Audio::setTimingDiagnostics(false);
    if (device) SDL_DestroyAudioStream(device);
    device = nullptr;
    if (const auto replayed = PetariNative::Platform::Audio::replayedBlocks())
        std::fprintf(stderr, "AI replayed %llu DMA blocks (the game's audio thread was late)\n", static_cast<unsigned long long>(replayed));
    if (diagnostics) {
        const auto dma = PetariNative::Platform::Audio::takeDmaStats();
        if (dma.waits != 0 || dma.waitTimeouts != 0 || dma.dspHolds != 0)
            std::fprintf(stderr, "AI DMA waited for %llu late registrations (%llu timed out); DSP holds %llu (since the last report)\n",
                         static_cast<unsigned long long>(dma.waits), static_cast<unsigned long long>(dma.waitTimeouts),
                         static_cast<unsigned long long>(dma.dspHolds));
    }
    if (const auto missing = ring.takeUnderrunFrames())
        std::fprintf(stderr, "SDL audio underrun: %llu silent frames\n", static_cast<unsigned long long>(missing));
    if (initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    initialized = false;
    if (failed.exchange(false)) std::fprintf(stderr, "SDL audio failed to queue samples\n");
}
void start(std::uint32_t rate, void*) {
    PetariNative::HostAllocationScope host;
    if (device) stop(nullptr);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL audio initialization failed: %s\n", SDL_GetError());
        std::abort();
    }
    initialized = true;
    ring.reset(prebuffer);
    stopProducer.store(false);
    const char* diagValue = std::getenv("PETARI_AUDIO_DIAG");  // unset, empty, or leading '0' = off
    diagnostics = diagValue != nullptr && diagValue[0] != '\0' && diagValue[0] != '0';
    PetariNative::Platform::Audio::setTimingDiagnostics(diagnostics);
    lastCallbackUs = 0;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, nullptr);
    if (device) producer = std::thread(produce, rate);
    if (device && diagnostics) {
        reporterStop = false;
        reporter = new std::thread(report);
    }
    if (!device || !SDL_ResumeAudioStreamDevice(device)) {
        std::fprintf(stderr, "SDL audio output failed: %s\n", SDL_GetError());
        stop(nullptr);
        std::abort();
    }
    running.store(true);
}
}

namespace PetariNative::AudioSDL {
void install() {
    submitted.store(0);
    Platform::Audio::setSink({start, stop, nullptr});
}
void shutdown() {
    Platform::Audio::shutdown();
}
bool active() { return running.load() && !failed.load(); }
std::uint64_t submittedFrames() { return submitted.load(); }
}

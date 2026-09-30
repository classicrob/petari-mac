#include <petari/audio_sdl.hpp>
#include <petari/host_allocation.hpp>
#include <petari/platform/audio.hpp>
#include <petari/platform/os_host.hpp>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hints.h>
#include <CoreAudio/CoreAudio.h>
#include <cmath>
#include <string>
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
// Latency settings. The ring's base level (prebuffer, the lowest target) is the
// cushion for elastic DMA waits and host stalls; the device adds SDL's
// AudioQueue buffers (sample frames x buffer count) and CoreAudio's own latency.
// Measured (PETARI_AUDIO_DIAG "[audio] latency", this Mac): with 64 ms of ring and
// SDL's default 1024-frame device buffer, heard audio trailed AI DMA by about
// 75 ms (ring) + up to 70 ms (3 AudioQueue buffers at 44.1 kHz) + 27 ms
// (CoreAudio) = ~160-170 ms. Defaults now: 48 ms of ring and 256-frame device
// buffers (6 x 5.8 ms = 35 ms of AudioQueue), ~110 ms in all. Kept only because
// it passed the real-time pacing scenarios (40 ms producer stall, the game
// answering 10 ms late, 40 ms game stalls) quiet and under 16 CPU spinners with
// no underrun or replay, as the old setting did (native_audio_pacing,
// PACING_PREBUFFER/PACING_BURST; build/heap-headroom/latency-matrix*). A smaller
// ring (32 ms) failed them. PETARI_AUDIO_BUFFER_MS and PETARI_AUDIO_DEVICE_FRAMES
// override these (PETARI_AUDIO_BUFFER_MS=64 PETARI_AUDIO_DEVICE_FRAMES=1024 is
// the previous setting). The elastic wait limit is what the ring can cover: its
// base less one device request and a 3 ms margin, at most 40 ms.
struct LatencyConfig {
    double bufferMs = 48.0;
    int deviceFrames = 256;
};
LatencyConfig latencyConfig() {
    LatencyConfig config;
    if (const char* value = std::getenv("PETARI_AUDIO_BUFFER_MS"); value != nullptr && std::atof(value) >= 8.0) {
        config.bufferMs = std::atof(value);
    }
    if (const char* value = std::getenv("PETARI_AUDIO_DEVICE_FRAMES"); value != nullptr && std::atoi(value) > 0) {
        config.deviceFrames = std::atoi(value);
    }
    return config;
}
std::uint64_t prebuffer = 1536;  // set by start() from latencyConfig()
double waitLimitMs = 0.0;       // set by start()
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
// The device callback's thread: SDL's CoreAudio AudioQueue thread, which SDL only raises with
// pthread_setschedparam. Under heavy host load (load ~230, CU session 5) it went 250-815 ms
// without running while the real-time producer ticked every 1 ms, and CoreAudio's queued
// buffers ran dry. It gets the same Mach time-constraint policy as the producer, once, from
// its first callback (0 = not yet, 1 = real-time, 2 = refused).
std::atomic<int> deviceThreadRealtime{0};

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
    int reportedDeviceThread = 0;
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
        if (const int state = deviceThreadRealtime.load(std::memory_order_relaxed); state != reportedDeviceThread) {
            reportedDeviceThread = state;
            std::fprintf(stderr, "[audio] device callback thread: %s\n", state == 1 ? "real-time (Mach time constraint)" : "real-time refused, SDL's priority kept");
        }
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
    static thread_local bool promoted = false;
    if (!promoted) {
        promoted = true;
        deviceThreadRealtime.store(PetariNative::Platform::Audio::setRealtimeAudioThread() ? 1 : 2, std::memory_order_relaxed);
    }
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
    deviceThreadRealtime.store(0, std::memory_order_relaxed);
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
// CoreAudio's own output latency for the default device, in device frames.
struct HalLatency {
    bool known = false;
    double rate = 0.0;
    UInt32 deviceLatency = 0, safetyOffset = 0, streamLatency = 0, ioBuffer = 0;
};
HalLatency queryHalLatency() {
    HalLatency hal;
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioDeviceID deviceId = 0;
    UInt32 size = sizeof(deviceId);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &deviceId) != noErr || deviceId == 0) {
        return hal;
    }
    const auto get = [&](AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, void* out, UInt32 bytes) {
        AudioObjectPropertyAddress a{selector, scope, kAudioObjectPropertyElementMain};
        UInt32 n = bytes;
        return AudioObjectGetPropertyData(deviceId, &a, 0, nullptr, &n, out) == noErr;
    };
    Float64 rate = 0.0;
    hal.known = get(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate, sizeof(rate));
    hal.rate = rate;
    get(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &hal.deviceLatency, sizeof(UInt32));
    get(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &hal.safetyOffset, sizeof(UInt32));
    get(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, &hal.ioBuffer, sizeof(UInt32));
    AudioStreamID streams[8];
    AudioObjectPropertyAddress streamsAddress{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 streamBytes = sizeof(streams);
    if (AudioObjectGetPropertyData(deviceId, &streamsAddress, 0, nullptr, &streamBytes, streams) == noErr && streamBytes >= sizeof(AudioStreamID)) {
        AudioObjectPropertyAddress latencyAddress{kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 n = sizeof(UInt32);
        AudioObjectGetPropertyData(streams[0], &latencyAddress, 0, nullptr, &n, &hal.streamLatency);
    }
    return hal;
}

// One line at start (PETARI_AUDIO_DIAG): every stage between AI DMA and the
// speaker. The ring's live level is in the [audio] reports; its base and the
// largest device request bound it.
void reportLatency(std::uint32_t rate, const LatencyConfig& latency) {
    SDL_AudioSpec deviceSpec{};
    int sampleFrames = 0;
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(device), &deviceSpec, &sampleFrames);
    // SDL_coreaudio.m: 3 AudioQueue buffers, or ceil(15 ms / buffer) * 2 for buffers under 15 ms.
    const double bufferMs = deviceSpec.freq > 0 ? sampleFrames * 1000.0 / deviceSpec.freq : 0.0;
    const int queueBuffers = bufferMs <= 0.0 ? 0 : bufferMs < 15.0 ? static_cast<int>(std::ceil(15.0 / bufferMs)) * 2 : 3;
    const HalLatency hal = queryHalLatency();
    const double halMs = hal.known && hal.rate > 0.0
                             ? (hal.deviceLatency + hal.safetyOffset + hal.streamLatency + hal.ioBuffer) * 1000.0 / hal.rate
                             : -1.0;
    const double ringBaseMs = prebuffer * 1000.0 / rate;
    std::fprintf(stderr,
                 "[audio] latency: ring base %llu frames (%.1f ms) + device requests; SDL device %d Hz, %d frames x %d AudioQueue buffers "
                 "= %.1f ms; CoreAudio device latency %u + safety %u + stream %u + IO buffer %u frames at %.0f Hz = %.1f ms; "
                 "elastic wait limit %.1f ms (PETARI_AUDIO_BUFFER_MS %.0f, PETARI_AUDIO_DEVICE_FRAMES %d)\n",
                 static_cast<unsigned long long>(prebuffer), ringBaseMs, deviceSpec.freq, sampleFrames, queueBuffers, bufferMs * queueBuffers,
                 hal.deviceLatency, hal.safetyOffset, hal.streamLatency, hal.ioBuffer, hal.rate, halMs, waitLimitMs,
                 latency.bufferMs, latency.deviceFrames);
}

void start(std::uint32_t rate, void*) {
    PetariNative::HostAllocationScope host;
    // Opening the CoreAudio device waits ~75 ms on SDL's device thread: do it
    // with the OS CPU released when the calling game thread can release it.
    struct CpuRelease {
        const int released = petari_os_try_begin_host_blocking();
        ~CpuRelease() {
            if (released) petari_os_end_host_blocking();
        }
    } cpu;
    if (device) stop(nullptr);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL audio initialization failed: %s\n", SDL_GetError());
        std::abort();
    }
    initialized = true;
    const LatencyConfig latency = latencyConfig();
    prebuffer = static_cast<std::uint64_t>(latency.bufferMs * rate / 1000.0 + 0.5);
    {
        // Read when SDL opens the physical device (SDL_GetDefaultSampleFramesFromFreq).
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(latency.deviceFrames).c_str());
    }
    ring.reset(prebuffer);
    stopProducer.store(false);
    const char* diagValue = std::getenv("PETARI_AUDIO_DIAG");  // unset, empty, or leading '0' = off
    diagnostics = diagValue != nullptr && diagValue[0] != '\0' && diagValue[0] != '0';
    PetariNative::Platform::Audio::setTimingDiagnostics(diagnostics);
    lastCallbackUs = 0;
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, nullptr);
    if (device) {
        // The elastic wait limit from the device's actual request size.
        SDL_AudioSpec deviceSpec{};
        int sampleFrames = 0;
        SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(device), &deviceSpec, &sampleFrames);
        const double requestMs = deviceSpec.freq > 0 ? sampleFrames * 1000.0 / deviceSpec.freq : 25.0;
        const double waitMs = std::clamp(latency.bufferMs - requestMs - 3.0, 0.0, 40.0);
        PetariNative::Platform::Audio::setRegistrationWaitLimit(static_cast<std::uint32_t>(waitMs * 1000.0));
        waitLimitMs = waitMs;
        producer = std::thread(produce, rate);
    }
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
    if (diagnostics) {
        reportLatency(rate, latency);
    }
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

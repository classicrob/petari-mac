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

#include <pthread.h>
#include <pthread/qos.h>

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

void produce(std::uint32_t rate) {
    PetariNative::HostAllocationScope host;
    using Clock = std::chrono::steady_clock;
    // Wake latency matters here: the producer must run every few ms (the
    // allowance tolerates late ticks, but not tens of ms of them).
    if (!PetariNative::Platform::Audio::setRealtimeAudioThread()) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
    const std::size_t quantum = rate / 1000;  // pull granularity: 1 ms of audio
    const PetariNative::AudioSDL::Detail::Pacer pacer(rate);
    auto last = Clock::now();
    // PETARI_AUDIO_DIAG=1: once a second, report what could make audio
    // distort: ring starvation (device underrun) versus late game audio
    // production (AI replaying a block), plus the ring level and the largest
    // device request.
    const char* diagValue = std::getenv("PETARI_AUDIO_DIAG");  // unset, empty, or leading '0' = off
    const bool diagnostics = diagValue != nullptr && diagValue[0] != '\0' && diagValue[0] != '0';
    auto nextReport = Clock::now() + std::chrono::seconds(1);
    std::uint64_t reportedReplays = 0;
    auto next = Clock::now();
    while (!stopProducer.load(std::memory_order_acquire)) {
        if (diagnostics && Clock::now() >= nextReport) {
            const std::uint64_t replays = PetariNative::Platform::Audio::replayedBlocks();
            std::fprintf(stderr, "[audio] level %llu/%llu frames, largest request %zu, underrun %llu frames, AI replayed %llu blocks (last second)\n",
                         static_cast<unsigned long long>(ring.level()), static_cast<unsigned long long>(ring.target()),
                         ring.largestRequest(), static_cast<unsigned long long>(ring.takeUnderrunFrames()),
                         static_cast<unsigned long long>(replays - reportedReplays));
            reportedReplays = replays;
            nextReport += std::chrono::seconds(1);
        }
        const auto now = Clock::now();
        const std::size_t allowance = pacer.allowance(std::chrono::duration<double>(now - last).count());
        last = now;
        PetariNative::AudioSDL::Detail::produceTick(ring, quantum, allowance, [](std::int16_t* out, std::size_t frames) {
            PetariNative::Platform::Audio::pull(out, frames);
        });
        // Ticks are paced in real time; a late tick does not bring extra
        // ticks (the level, not the tick count, decides how much is pulled).
        next = std::max(next + std::chrono::milliseconds(1), Clock::now());
        std::this_thread::sleep_until(next);
    }
}

void SDLCALL fill(void*, SDL_AudioStream* stream, int additionalBytes, int) {
    PetariNative::HostAllocationScope host;
    alignas(16) std::int16_t buffer[1024 * 2];
    int frames = additionalBytes > 0 ? (additionalBytes + 3) / 4 : 0;
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
    if (device) SDL_DestroyAudioStream(device);
    device = nullptr;
    if (const auto replayed = PetariNative::Platform::Audio::replayedBlocks())
        std::fprintf(stderr, "AI replayed %llu DMA blocks (the game's audio thread was late)\n", static_cast<unsigned long long>(replayed));
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
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, nullptr);
    if (device) producer = std::thread(produce, rate);
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

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

namespace {
SDL_AudioStream* device;
bool initialized;
std::atomic<bool> running{false};
std::atomic<std::uint64_t> submitted{0};
std::atomic<bool> failed{false};
std::atomic<std::uint64_t> underrunFrames{0};

// SDL requests audio in device-sized bursts. AI DMA must instead advance in
// real time so the game's interrupt/audio threads can prepare the next block.
// The producer owns AI::pull; the realtime SDL callback only drains this ring.
constexpr std::uint64_t capacity = 8192;
constexpr std::uint64_t prebuffer = 2048;
std::array<std::int16_t, capacity * 2> samples{};
std::atomic<std::uint64_t> readAt{0}, writeAt{0};
std::atomic<bool> stopProducer{false};
std::thread producer;

void produce(std::uint32_t rate) {
    PetariNative::HostAllocationScope host;
    using Clock = std::chrono::steady_clock;
    const std::size_t quantum = rate / 1000;
    std::array<std::int16_t, 96> block{};
    auto next = Clock::now();
    while (!stopProducer.load(std::memory_order_acquire)) {
        PetariNative::Platform::Audio::pull(block.data(), quantum);
        const auto w = writeAt.load(std::memory_order_relaxed);
        const auto r = readAt.load(std::memory_order_acquire);
        if (w - r + quantum <= capacity) {
            for (std::size_t i = 0; i < quantum; ++i) {
                const auto at = (w + i) % capacity;
                samples[at * 2] = block[i * 2];
                samples[at * 2 + 1] = block[i * 2 + 1];
            }
            writeAt.store(w + quantum, std::memory_order_release);
        }
        next += std::chrono::milliseconds(1);
        // After a scheduling stall, avoid consuming many DMA blocks at once.
        // Allow up to one quantum of catch-up, then resume a steady clock.
        const auto now = Clock::now();
        if (next + std::chrono::milliseconds(1) < now) next = now;
        std::this_thread::sleep_until(next);
    }
}

void SDLCALL fill(void*, SDL_AudioStream* stream, int additionalBytes, int) {
    PetariNative::HostAllocationScope host;
    alignas(16) std::int16_t buffer[1024 * 2];
    int frames = additionalBytes > 0 ? (additionalBytes + 3) / 4 : 0;
    while (frames > 0) {
        const int count = std::min(frames, 1024);
        const auto r = readAt.load(std::memory_order_relaxed);
        const auto available = writeAt.load(std::memory_order_acquire) - r;
        const auto copied = std::min<std::uint64_t>(count, available);
        for (std::uint64_t i = 0; i < copied; ++i) {
            const auto at = (r + i) % capacity;
            buffer[i * 2] = samples[at * 2];
            buffer[i * 2 + 1] = samples[at * 2 + 1];
        }
        if (copied < static_cast<std::uint64_t>(count))
            underrunFrames.fetch_add(count - copied, std::memory_order_relaxed);
        std::fill(buffer + copied * 2, buffer + count * 2, 0);
        readAt.store(r + copied, std::memory_order_release);
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
    if (const auto missing = underrunFrames.exchange(0))
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
    samples.fill(0);
    readAt.store(0);
    writeAt.store(prebuffer);
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

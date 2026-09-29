#include <petari/audio_sdl.hpp>
#include <petari/host_allocation.hpp>
#include <petari/platform/audio.hpp>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
SDL_AudioStream* device;
bool initialized;
std::atomic<bool> running{false};
std::atomic<std::uint64_t> submitted{0};
std::atomic<bool> failed{false};

void SDLCALL fill(void*, SDL_AudioStream* stream, int additionalBytes, int) {
    PetariNative::HostAllocationScope host;
    alignas(16) std::int16_t buffer[1024 * 2];
    int frames = additionalBytes > 0 ? (additionalBytes + 3) / 4 : 0;
    while (frames > 0) {
        const int count = std::min(frames, 1024);
        PetariNative::Platform::Audio::pull(buffer, count);
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
    if (device) SDL_DestroyAudioStream(device);
    device = nullptr;
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
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, fill, nullptr);
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

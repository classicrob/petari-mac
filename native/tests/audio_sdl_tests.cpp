#include <petari/audio_sdl.hpp>
#include <revolution/ai.h>
#include <revolution/os.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void __OSThreadInit();
extern "C" bool SDL_SetHint(const char*, const char*);
namespace {
std::atomic<unsigned> callbacks{0};
std::array<std::chrono::steady_clock::time_point, 32> starts;
void dma() {
    const auto index = callbacks.load(std::memory_order_relaxed);
    if (index < starts.size()) starts[index] = std::chrono::steady_clock::now();
    callbacks.store(index + 1, std::memory_order_release);
}
}
int main() {
    __OSThreadInit();
    alignas(32) std::int16_t samples[640]{};
    for (unsigned test = 0; test < 4; ++test) {
        const unsigned rate = test % 2;
        const bool muted = test >= 2;
        if (!SDL_SetHint("PETARI_SMOKE_BACKGROUND", muted ? "1" : "0")) return 2;
        callbacks = 0;
        PetariNative::AudioSDL::install();
        AIInit(nullptr);
        AISetDSPSampleRate(rate);
        AIRegisterDMACallback(dma);
        AIInitDMA(reinterpret_cast<uintptr_t>(samples), sizeof(samples));
        AIStartDMA();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (callbacks.load() < starts.size() && std::chrono::steady_clock::now() < deadline) {
            OSRestoreInterrupts(OSDisableInterrupts());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool ok = callbacks >= starts.size() && PetariNative::AudioSDL::active() &&
                        PetariNative::AudioSDL::submittedFrames() >= 640 &&
                        PetariNative::AudioSDL::outputMuted() == muted;
        PetariNative::AudioSDL::shutdown();
        if (!ok || PetariNative::AudioSDL::active()) {
            std::fprintf(stderr, "Audio case %u failed (muted=%d, callbacks=%u)\n", test, muted, callbacks.load());
            return 1;
        }
        // A device request can span several DMA blocks, but their interrupts
        // must be paced so the game can prepare the next block between them.
        for (std::size_t i = 1; i < starts.size(); ++i) {
            const auto gap = std::chrono::duration<double, std::milli>(starts[i] - starts[i - 1]).count();
            if (gap < 2.0) {
                std::fprintf(stderr, "DMA blocks consumed in a burst: %.3f ms apart\n", gap);
                return 1;
            }
        }
    }
    std::puts("SDL audio callback, DMA interrupts, 32/48 kHz, reopen and silent realtime output passed.");
}

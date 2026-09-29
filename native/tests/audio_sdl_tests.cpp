#include <petari/audio_sdl.hpp>
#include <revolution/ai.h>
#include <revolution/os.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" void __OSThreadInit();
namespace {
std::atomic<unsigned> callbacks{0};
void dma() { ++callbacks; }
}
int main() {
    __OSThreadInit();
    alignas(32) std::int16_t samples[640]{};
    for (unsigned rate = 0; rate < 2; ++rate) {
        callbacks = 0;
        PetariNative::AudioSDL::install();
        AIInit(nullptr);
        AISetDSPSampleRate(rate);
        AIRegisterDMACallback(dma);
        AIInitDMA(reinterpret_cast<uintptr_t>(samples), sizeof(samples));
        AIStartDMA();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (callbacks.load() < 3 && std::chrono::steady_clock::now() < deadline) {
            OSRestoreInterrupts(OSDisableInterrupts());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool ok = callbacks >= 3 && PetariNative::AudioSDL::active() &&
                        PetariNative::AudioSDL::submittedFrames() >= 640;
        PetariNative::AudioSDL::shutdown();
        if (!ok || PetariNative::AudioSDL::active()) return 1;
    }
    std::puts("SDL audio callback, DMA interrupts, 32/48 kHz and reopen passed.");
}

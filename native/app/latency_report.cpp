// Input-to-present latency (petari/latency_probe.hpp): the Metal layer's
// presentation settings, one log line per measured press, and a test driver.
#include "latency_report.hpp"

#include <petari/input.hpp>
#include <petari/latency_probe.hpp>

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

namespace PetariNative::App::LatencyReport {
namespace {

namespace Probe = PetariNative::LatencyProbe;

void* gLayer = nullptr;
std::atomic<bool> gGameplay{false};

double ms(std::uint64_t from, std::uint64_t to) {
    return to >= from ? (to - from) / 1e6 : -1.0;
}

const char* stageName(int stage) {
    static const char* const kNames[] = {"idle", "pressed", "reported", "read", "copied", "shown", "presented"};
    return stage >= 0 && stage <= Probe::Presented ? kNames[stage] : "?";
}

}  // namespace

void logLayer(const char* when) {
    if (gLayer == nullptr) return;
    const auto count = reinterpret_cast<unsigned long (*)(void*, SEL)>(objc_msgSend)(gLayer, sel_registerName("maximumDrawableCount"));
    const auto sync = reinterpret_cast<BOOL (*)(void*, SEL)>(objc_msgSend)(gLayer, sel_registerName("displaySyncEnabled"));
    const auto transaction = reinterpret_cast<BOOL (*)(void*, SEL)>(objc_msgSend)(gLayer, sel_registerName("presentsWithTransaction"));
    std::fprintf(stderr, "[latency] metal layer (%s): maximumDrawableCount %lu, displaySyncEnabled %d, presentsWithTransaction %d\n",
                 when, count, sync ? 1 : 0, transaction ? 1 : 0);
}

void configureLayer(void* window) {
    auto* sdlWindow = static_cast<SDL_Window*>(window);
    auto* view = static_cast<SDL_MetalView>(SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow), "aurora.metal_view", nullptr));
    gLayer = view != nullptr ? SDL_Metal_GetLayer(view) : nullptr;
    if (gLayer == nullptr) return;
    // PETARI_MAX_DRAWABLES=2|3: CAMetalLayer's drawable queue (default 3).
    if (const char* value = std::getenv("PETARI_MAX_DRAWABLES"); value != nullptr && (value[0] == '2' || value[0] == '3')) {
        reinterpret_cast<void (*)(void*, SEL, unsigned long)>(objc_msgSend)(gLayer, sel_registerName("setMaximumDrawableCount:"),
                                                                        static_cast<unsigned long>(value[0] - '0'));
    }
    if (Probe::enabled()) logLayer("startup");
}

void frame(bool gameplay) {
    gGameplay.store(gameplay, std::memory_order_relaxed);
    if (!Probe::enabled()) return;
    static unsigned long frames = 0;
    if (++frames == 600) logLayer("after 600 frames");
    const int stage = Probe::stage.load();
    if (stage == Probe::Idle) return;
    const std::uint64_t press = Probe::pressNs.load();
    if (stage == Probe::Presented) {
        std::fprintf(stderr,
                     "[latency] press: report %.1f ms, game read %.1f ms (frame %u), display copy %.1f ms (frame %u), "
                     "shown %.1f ms (frame %u, %u frames after the read), present returned %.1f ms\n",
                     ms(press, Probe::reportNs.load()), ms(press, Probe::readNs.load()), Probe::readFrame.load(),
                     ms(press, Probe::copyNs.load()), Probe::copyFrame.load(), ms(press, Probe::shownNs.load()),
                     Probe::shownFrame.load(), Probe::shownFrame.load() - Probe::readFrame.load(),
                     ms(press, Probe::presentNs.load()));
        Probe::stage.store(Probe::Idle);
    } else if (Probe::nowNs() - press > 3'000'000'000ULL) {
        // A press the game never read as gameplay (menus, pause), or no new copy.
        std::fprintf(stderr, "[latency] press dropped at stage %s\n", stageName(stage));
        Probe::stage.store(Probe::Idle);
    }
}

void startTestDriver() {
    // PETARI_LATENCY_TEST=1: during gameplay, press Space (A) at random moments
    // 1.0-1.6 s apart, independent of the frame clock, for 60 ms.
    const char* value = std::getenv("PETARI_LATENCY_TEST");
    if (value == nullptr || value[0] == '\0' || value[0] == '0') return;
    std::thread([] {
        std::mt19937 random(12345);
        std::uniform_int_distribution<int> gap(1000, 1600);
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap(random)));
            if (!gGameplay.load(std::memory_order_relaxed)) continue;
            PetariNative::Input::keyEvent(PetariNative::Input::Key::Space, true, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            PetariNative::Input::keyEvent(PetariNative::Input::Key::Space, false, false);
        }
    }).detach();
}

}  // namespace PetariNative::App::LatencyReport

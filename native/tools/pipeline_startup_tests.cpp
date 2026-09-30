// Exercise the real startup loop without a window, GPU, game, or elapsed waits.
#include <petari/pipeline_startup.hpp>
#include <petari/input.hpp>
#include "../app/smoke_background.hpp"
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
Uint64 clockNs, readyAt, eventAt;
unsigned drawn, ended, updates, skips, pendingAtReturn, finished, preciseDelays, titleUpdates;
Uint64 eventWorkNs, drawWorkNs;
std::vector<Uint64> drawTimes;
bool canDraw, failCompile, delivered, focused, gotFocus, overlayAvailable, fallbackSeen;
SDL_Event inputEvent;
std::string title;
std::vector<PetariNative::PipelinePreparationView> views;
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void reset(Uint64 duration) {
    clockNs = 0; readyAt = duration; eventAt = 0;
    drawn = ended = updates = skips = pendingAtReturn = finished = preciseDelays = titleUpdates = 0;
    eventWorkNs = drawWorkNs = 0; drawTimes.clear();
    canDraw = focused = overlayAvailable = true;
    failCompile = delivered = gotFocus = fallbackSeen = false;
    inputEvent = {}; title.clear(); views.clear();
}
}
extern "C" Uint64 SDL_GetTicksNS() { return clockNs; }
extern "C" void SDL_DelayNS(Uint64 ns) { clockNs += ns; }
extern "C" void SDL_DelayPrecise(Uint64 ns) { ++preciseDelays; clockNs += ns; }
extern "C" SDL_WindowFlags SDL_GetWindowFlags(SDL_Window*) { return focused ? SDL_WINDOW_INPUT_FOCUS : 0; }
extern "C" bool SDL_GetWindowSize(SDL_Window*, int* width, int* height) {
    *width = clockNs < 3000000000 ? 800 : 1280; *height = clockNs < 3000000000 ? 600 : 720; return true;
}
extern "C" bool SDL_SetWindowTitle(SDL_Window*, const char* text) {
    ++titleUpdates; title = text; fallbackSeen |= title.find("Return to start now") != std::string::npos; return true;
}
extern "C" const AuroraEvent* aurora_update() {
    ++updates;
    clockNs += updates % 2 ? eventWorkNs : 0;
    static AuroraEvent events[2]; events[0] = {}; events[1] = {};
    if (inputEvent.type && !delivered && clockNs >= eventAt) {
        events[0].type = inputEvent.type == SDL_EVENT_QUIT ? AURORA_EXIT : AURORA_SDL_EVENT;
        events[0].sdl = inputEvent;
        delivered = true;
        if (inputEvent.type == SDL_EVENT_WINDOW_FOCUS_LOST) focused = false;
    }
    return events;
}
extern "C" bool aurora_begin_frame() { return canDraw; }
extern "C" void aurora_end_frame() { ++ended; }
extern "C" void petari_gx_pipeline_preparation_status(std::uint32_t* total, std::uint32_t* pending, std::uint32_t* failed) {
    *total = 100;
    *pending = clockNs >= readyAt ? 0 : 100 - static_cast<unsigned>(clockNs * 100 / readyAt);
    *failed = failCompile ? 1 : 0;
    pendingAtReturn = *pending;
}
extern "C" void petari_gx_pipeline_startup_finished() { ++finished; }
extern "C" bool petari_gx_pipeline_full_preparation() { return true; }
namespace PetariNative::Input { void focusChanged(bool value) { gotFocus = value; } }
namespace PetariNative::HomeMenu {
bool drawPipelinePreparation(const PipelinePreparationView& view, float width, float height) {
    check(width == (clockNs < 3000000000 ? 800 : 1280) && height == (clockNs < 3000000000 ? 600 : 720),
          "overlay did not receive current window size");
    drawTimes.push_back(clockNs); clockNs += drawWorkNs;
    ++drawn; views.push_back(view); return overlayAvailable;
}
}
int main() try {
    auto* window = reinterpret_cast<SDL_Window*>(1);
    reset(1500000000);
    check(PetariNative::App::preparePipelines(window), "warm preparation failed");
    check(drawn == 0 && ended == 0 && finished == 1 && clockNs < 1600000000, "warm launch flashed overlay or waited artificially");
    reset(5000000000);
    check(PetariNative::App::preparePipelines(window), "cold preparation failed");
    check(drawn >= 178 && drawn <= 183 && ended == drawn, "startup did not present at 60 Hz after reveal delay");
    check(updates > 295 && gotFocus && title == "Super Mario Galaxy", "events, focus or title not restored");
    check(!views.empty() && views.back().remainingSeconds >= 0, "remaining-time estimate was never populated");
    reset(5000000000);
    eventWorkNs = 2000000;
    drawWorkNs = 1000000;
    check(PetariNative::App::preparePipelines(window), "paced preparation failed");
    check(preciseDelays > 170 && titleUpdates <= 15, "visible pacing or bounded title update frequency failed");
    for (std::size_t i = 1; i < drawTimes.size(); ++i)
        check(drawTimes[i] - drawTimes[i - 1] >= 16666665 && drawTimes[i] - drawTimes[i - 1] <= 16666667,
              "variable event work leaked into the presentation cadence");
    reset(5000000000);
    eventAt = 2500000000; inputEvent.type = SDL_EVENT_KEY_DOWN; inputEvent.key.scancode = SDL_SCANCODE_RETURN;
    check(PetariNative::App::preparePipelines(window) && pendingAtReturn > 0 && finished == 1 && clockNs < 2600000000,
          "Return did not leave remaining work preparing");
    reset(5000000000);
    eventAt = 2500000000; inputEvent.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; inputEvent.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    check(PetariNative::App::preparePipelines(window) && pendingAtReturn > 0, "controller cannot skip");
    reset(5000000000);
    focused = false; inputEvent.type = SDL_EVENT_KEY_DOWN; inputEvent.key.scancode = SDL_SCANCODE_KP_ENTER;
    check(PetariNative::App::preparePipelines(window) && !pendingAtReturn && !gotFocus,
          "unfocused key skipped preparation or focus state was lost");
    reset(3000000000); canDraw = false;
    check(PetariNative::App::preparePipelines(window) && !ended && fallbackSeen, "unpresentable window blocked completion or lost fallback");
    reset(3000000000); overlayAvailable = false;
    check(PetariNative::App::preparePipelines(window) && fallbackSeen, "overlay failure prevented title fallback");
    reset(5000000000); eventAt = 500000000; inputEvent.type = SDL_EVENT_QUIT;
    check(!PetariNative::App::preparePipelines(window) && finished == 1, "quit was ignored during preparation");
    reset(0); failCompile = true;
    check(!PetariNative::App::preparePipelines(window), "failed shader preparation claimed success");
    reset(5000000000);
    PetariNative::App::SmokeBackground::enabled = true;
    focused = false;
    inputEvent.type = SDL_EVENT_KEY_DOWN;
    inputEvent.key.scancode = SDL_SCANCODE_RETURN;
    check(PetariNative::App::preparePipelines(window) && !pendingAtReturn && gotFocus && preciseDelays > 0,
          "background prep must ignore hardware skip, retain driver focus, and precisely pace unfocused frames");
    PetariNative::App::SmokeBackground::enabled = false;
    std::puts("Startup preparation: warm invisibility, 60 Hz/event pumping, skip, focus, resize, fallback and failure pass");
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}

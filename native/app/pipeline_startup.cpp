#include <petari/pipeline_startup.hpp>
#include <petari/input.hpp>

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace PetariNative::App {
bool preparePipelines(SDL_Window* window) {
    constexpr Uint64 framePeriod = 1000000000 / 60;
    constexpr Uint64 revealDelay = 2000000000;
    const Uint64 start = SDL_GetTicksNS();
    Uint64 deadline = start, sampleTime = start, lastFrame = 0;
    std::uint32_t total = 0, pending = 0, failed = 0;
    petari_gx_pipeline_preparation_status(&total, &pending, &failed);
    const std::uint32_t initialPending = pending;
    std::uint32_t sampleCompleted = total - pending;
    double rate = 0.0;
    bool shown = false, skipped = false;
    bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    unsigned displayedRemaining = pending + 1;
    std::vector<double> intervals;
    const bool global = petari_gx_pipeline_full_preparation();

    while (pending) {
        // Aurora owns resize, surface lifetime and controller discovery. Startup
        // key/button events stay here so Return cannot also press the game's A.
        for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; ++event) {
            if (event->type == AURORA_EXIT) {
                SDL_SetWindowTitle(window, "Super Mario Galaxy");
                std::fprintf(stderr, "[gx startup prep] result=quit remaining=%u\n", pending);
                return false;
            }
            if (event->type != AURORA_SDL_EVENT) continue;
            const auto& input = event->sdl;
            if (input.type == SDL_EVENT_WINDOW_FOCUS_LOST) focused = false;
            if (input.type == SDL_EVENT_WINDOW_FOCUS_GAINED) focused = true;
            if (!focused) continue;
            const bool key = input.type == SDL_EVENT_KEY_DOWN && !input.key.repeat &&
                (input.key.scancode == SDL_SCANCODE_RETURN || input.key.scancode == SDL_SCANCODE_KP_ENTER);
            const bool button = input.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                (input.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH || input.gbutton.button == SDL_GAMEPAD_BUTTON_START);
            if (key || button) skipped = true;
        }
        if (skipped) break;
        petari_gx_pipeline_preparation_status(&total, &pending, &failed);
        if (!pending) break;
        const Uint64 now = SDL_GetTicksNS();
        const std::uint32_t completed = total - pending;
        if (now - sampleTime >= 1000000000) {
            const double recent = completed >= sampleCompleted ?
                (completed - sampleCompleted) * 1e9 / (now - sampleTime) : 0.0;
            rate = rate > 0.0 ? 0.65 * rate + 0.35 * recent : recent;
            sampleCompleted = completed;
            sampleTime = now;
        }
        // Avoid flashing a preparation screen on the measured ~1.6 s warm path.
        if (now - start >= revealDelay) {
            if (!shown) {
                shown = true;
                std::fprintf(stderr, "[gx startup prep] screen=shown total=%u pending=%u elapsed_ms=%.3f\n",
                             total, pending, (now - start) / 1e6);
            }
            bool drawn = false;
            if (aurora_begin_frame()) {
                int width = 0, height = 0;
                SDL_GetWindowSize(window, &width, &height);
                const PipelinePreparationView view{completed, total, rate > 0.0 ? pending / rate : -1.0, global};
                drawn = HomeMenu::drawPipelinePreparation(view, static_cast<float>(width), static_cast<float>(height));
                aurora_end_frame();
                if (lastFrame) intervals.push_back((now - lastFrame) / 1e6);
                lastFrame = now;
            }
            // A title remains useful while hidden/minimized or if no overlay can draw.
            if (pending != displayedRemaining) {
                char title[160];
                std::snprintf(title, sizeof(title), "Super Mario Galaxy — Preparing shaders %u / %u (%u%%)%s",
                              completed, total, total ? static_cast<unsigned>(100ull * completed / total) : 100,
                              drawn ? "" : " — Return to start now");
                SDL_SetWindowTitle(window, title);
                displayedRemaining = pending;
            }
        }
        deadline += framePeriod;
        const Uint64 afterFrame = SDL_GetTicksNS();
        if (afterFrame < deadline) SDL_DelayNS(deadline - afterFrame);
        else if (afterFrame - deadline >= framePeriod) deadline = afterFrame;
    }

    // Global startup requests already use the utility-QoS background queue.
    // Leaving this loop does not cancel them; stage/draw requests still promote
    // matching jobs, and the normal stage gates protect stages not ready yet.
    Input::focusChanged((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    SDL_SetWindowTitle(window, "Super Mario Galaxy");
    petari_gx_pipeline_preparation_status(&total, &pending, &failed);
    std::sort(intervals.begin(), intervals.end());
    const double p99 = intervals.empty() ? 0.0 : intervals[static_cast<std::size_t>((intervals.size() - 1) * 0.99)];
    std::fprintf(stderr,
                 "[gx startup prep] result=%s total=%u remaining=%u failed=%u screen_shown=%u elapsed_ms=%.3f "
                 "frame_intervals=%zu frame_p99_ms=%.3f frame_max_ms=%.3f\n",
                 failed ? "failed" : skipped && pending ? "background" : "ready", total, pending, failed,
                 shown ? 1 : 0, (SDL_GetTicksNS() - start) / 1e6, intervals.size(), p99,
                 intervals.empty() ? 0.0 : intervals.back());
    if (!skipped && !failed)
        std::fprintf(stderr, "[gx warmup] prepared %u queued pipelines before gameplay in %.2f s\n",
                     initialPending, (SDL_GetTicksNS() - start) / 1e9);
    return failed == 0;
}
}

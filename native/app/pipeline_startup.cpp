#include <petari/pipeline_startup.hpp>
#include <petari/input.hpp>

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace PetariNative::App {
namespace {
struct PrepTiming {
    enum Part { Events, Status, Begin, Draw, End, Title, Sleep, Oversleep, Work, Cycle, FrameInterval, Count };
    std::array<double, Count> ms{};
    bool visible = false;
};
void reportTimings(const std::vector<PrepTiming>& samples) {
    constexpr const char* names[] = {"events", "status", "begin", "draw", "end", "title",
                                    "sleep", "oversleep", "work", "cycle", "frame_interval"};
    std::vector<double> values;
    values.reserve(samples.size());
    for (unsigned part = 0; part < PrepTiming::Count; ++part) {
        values.clear();
        for (const auto& sample : samples) values.push_back(sample.ms[part]);
        std::sort(values.begin(), values.end());
        const auto percentile = [&](double fraction) {
            return values.empty() ? 0.0 : values[static_cast<std::size_t>((values.size() - 1) * fraction)];
        };
        std::fprintf(stderr, "[gx startup timing] part=%s count=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f\n",
                     names[part], values.size(), percentile(.5), percentile(.95), percentile(.99),
                     values.empty() ? 0.0 : values.back());
    }
    std::vector<std::size_t> worst;
    worst.reserve(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) worst.push_back(i);
    std::partial_sort(worst.begin(), worst.begin() + std::min<std::size_t>(12, worst.size()), worst.end(),
        [&](auto a, auto b) { return std::max(samples[a].ms[PrepTiming::Cycle], samples[a].ms[PrepTiming::FrameInterval]) >
                                  std::max(samples[b].ms[PrepTiming::Cycle], samples[b].ms[PrepTiming::FrameInterval]); });
    for (std::size_t i = 0; i < std::min<std::size_t>(12, worst.size()); ++i) {
        const auto& sample = samples[worst[i]];
        if (std::max(sample.ms[PrepTiming::Cycle], sample.ms[PrepTiming::FrameInterval]) < 25.0) break;
        std::fprintf(stderr, "[gx startup hitch] iteration=%zu visible=%u", worst[i], sample.visible ? 1 : 0);
        for (unsigned part = 0; part < PrepTiming::Count; ++part)
            std::fprintf(stderr, " %s_ms=%.3f", names[part], sample.ms[part]);
        std::fputc('\n', stderr);
    }
}
}

bool preparePipelines(SDL_Window* window) {
    struct FinishStartup {
        ~FinishStartup() { petari_gx_pipeline_startup_finished(); }
    } finishStartup;
    constexpr Uint64 framePeriod = 1000000000 / 60;
    constexpr Uint64 revealDelay = 2000000000;
    const Uint64 start = SDL_GetTicksNS();
    Uint64 deadline = start, sampleTime = start, lastFrame = 0, nextTitle = 0;
    std::uint32_t total = 0, pending = 0, failed = 0;
    petari_gx_pipeline_preparation_status(&total, &pending, &failed);
    const std::uint32_t initialPending = pending;
    std::uint32_t sampleCompleted = total - pending;
    double rate = 0.0;
    bool shown = false, skipped = false;
    bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    unsigned displayedRemaining = pending + 1;
    std::vector<double> intervals;
    intervals.reserve(16384);
    std::vector<PrepTiming> timings;
    timings.reserve(16384);
    const bool global = petari_gx_pipeline_full_preparation();

    while (pending) {
        PrepTiming timing;
        const Uint64 cycleStart = SDL_GetTicksNS();
        Uint64 partStart = cycleStart;
        const auto mark = [&](PrepTiming::Part part) {
            const auto end = SDL_GetTicksNS();
            timing.ms[part] = (end - partStart) / 1e6;
            partStart = end;
        };
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
        mark(PrepTiming::Events);
        if (skipped) break;
        petari_gx_pipeline_preparation_status(&total, &pending, &failed);
        mark(PrepTiming::Status);
        if (!pending) break;
        // Process events before the presentation deadline: their variable cost
        // then consumes the available slack instead of moving every draw later.
        const Uint64 beforeWait = SDL_GetTicksNS();
        if (beforeWait < deadline) {
            if (shown && focused) SDL_DelayPrecise(deadline - beforeWait);
            else SDL_DelayNS(deadline - beforeWait);
            const auto awake = SDL_GetTicksNS();
            timing.ms[PrepTiming::Sleep] = (awake - beforeWait) / 1e6;
            timing.ms[PrepTiming::Oversleep] = awake > deadline ? (awake - deadline) / 1e6 : 0.0;
        } else if (beforeWait - deadline >= framePeriod) {
            deadline = beforeWait;
        }
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
            timing.visible = true;
            bool drawn = false;
            partStart = SDL_GetTicksNS();
            const bool began = aurora_begin_frame();
            mark(PrepTiming::Begin);
            if (began) {
                int width = 0, height = 0;
                SDL_GetWindowSize(window, &width, &height);
                const PipelinePreparationView view{completed, total, rate > 0.0 ? pending / rate : -1.0, global};
                drawn = HomeMenu::drawPipelinePreparation(view, static_cast<float>(width), static_cast<float>(height));
                mark(PrepTiming::Draw);
                aurora_end_frame();
                mark(PrepTiming::End);
                if (lastFrame) {
                    timing.ms[PrepTiming::FrameInterval] = (now - lastFrame) / 1e6;
                    intervals.push_back(timing.ms[PrepTiming::FrameInterval]);
                }
                lastFrame = now;
            }
            // A title remains useful while hidden/minimized or if no overlay can draw.
            partStart = SDL_GetTicksNS();
            if (pending != displayedRemaining && now >= nextTitle) {
                char title[160];
                std::snprintf(title, sizeof(title), "Super Mario Galaxy — Preparing shaders %u / %u (%u%%)%s",
                              completed, total, total ? static_cast<unsigned>(100ull * completed / total) : 100,
                              drawn ? "" : " — Return to start now");
                SDL_SetWindowTitle(window, title);
                displayedRemaining = pending;
                nextTitle = now + 250000000;
            }
            mark(PrepTiming::Title);
        }
        deadline += framePeriod;
        const Uint64 afterFrame = SDL_GetTicksNS();
        timing.ms[PrepTiming::Cycle] = (afterFrame - cycleStart) / 1e6;
        timing.ms[PrepTiming::Work] = timing.ms[PrepTiming::Cycle] - timing.ms[PrepTiming::Sleep];
        timings.push_back(timing);
    }

    reportTimings(timings);

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

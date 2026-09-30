// Tests for the native frame seam (native/app/frame_seam.cpp): the order of
// host work around a game frame, keeping an Aurora frame open, window events,
// the quit path, and the presentation and CPU-release hooks. Aurora, SDL and
// the app's other translation units are replaced by recording fakes here.

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <SDL3/SDL_video.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../app/host.hpp"
#include "../app/spike_profiler.hpp"
#include "../app/smoke.hpp"
#include "petari/app.hpp"
#include "petari/frame_telemetry.hpp"
#include "petari/host_allocation.hpp"

namespace App = PetariNative::App;

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

std::vector<std::string> calls;
int hostScopes = 0;
bool hostScopeMissed = false;
int beginFailures = 0;
std::vector<std::vector<AuroraEvent>> updates;  // returned by successive aurora_update calls
std::vector<AuroraEvent> current;
int windowWidth = 1280;
int windowHeight = 720;
App::Rect lastImage;
float lastWindowWidth = 0.0f;
float lastWindowHeight = 0.0f;
int imageUpdates = 0;
std::vector<SDL_EventType> inputEvents;
float overlay[4];

struct ForcedQuit {};

void hostCall(const char* name) {
    if (hostScopes == 0) {
        hostScopeMissed = true;
    }
    calls.push_back(name);
}

bool contains(const std::vector<std::string>& list, const std::vector<std::string>& expected) {
    return list == expected;
}

AuroraEvent sdlKey() {
    AuroraEvent event{};
    event.type = AURORA_SDL_EVENT;
    event.sdl.type = SDL_EVENT_KEY_DOWN;
    return event;
}

AuroraEvent exitEvent() {
    AuroraEvent event{};
    event.type = AURORA_EXIT;
    return event;
}

// Present hooks
bool composeCalled = false;
bool imageKnown = true;
App::Rect presentImage{160.0f, 0.0f, 960.0f, 720.0f};
void compose(void*) {
    hostCall("compose");
}
bool imageRect(App::Rect* rect, void*) {
    hostCall("imageRect");
    *rect = presentImage;
    return imageKnown;
}
void releaseBegin() {
    calls.push_back("release");
}
void releaseEnd() {
    calls.push_back("reacquire");
}

}  // namespace

// --- Fakes ---

namespace PetariNative {
HostAllocationScope::HostAllocationScope() {
    ++hostScopes;
}
HostAllocationScope::~HostAllocationScope() {
    --hostScopes;
}
}  // namespace PetariNative

namespace PetariNative::HomeMenu {
void drawImGuiOverlay(float x, float y, float width, float height) {
    hostCall("overlay");
    overlay[0] = x;
    overlay[1] = y;
    overlay[2] = width;
    overlay[3] = height;
}
}  // namespace PetariNative::HomeMenu

namespace PetariNative::App::Host {
void requestQuit() {
    calls.push_back("requestQuit");
}
void forceQuit() {
    calls.push_back("forceQuit");
    throw ForcedQuit{};
}
}  // namespace PetariNative::App::Host

namespace PetariNative::App::Events {
Smoke::PhysicalInputs physicalInputs() {
    return {};
}
bool input(const SDL_Event& event) {
    inputEvents.push_back(static_cast<SDL_EventType>(event.type));
    return true;
}
void pressButton(int button, bool down) {
    calls.push_back(std::string(down ? "press " : "release ") + (button == 0 ? "A" : button == 1 ? "B" : "StickUp"));
}
float pointerX = -1.0f, pointerY = -1.0f;
void movePointer(float x, float y) {
    calls.push_back("pointer");
    pointerX = x;
    pointerY = y;
}
void assertFocus() {
    calls.push_back("focus");
}
void setImage(const Rect& image, float width, float height) {
    ++imageUpdates;
    lastImage = image;
    lastWindowWidth = width;
    lastWindowHeight = height;
}
}  // namespace PetariNative::App::Events

namespace PetariNative::App::Smoke {
Observation smokeObservation;
bool wantedPlayer = false;
Observation observeGame(bool wantPlayer) {
    wantedPlayer = wantPlayer;
    hostCall("observe");
    return smokeObservation;
}
}  // namespace PetariNative::App::Smoke

// The soak telemetry is off in these tests.
namespace PetariNative::App::Soak {
bool startTelemetryFromEnvironment() {
    return false;
}
bool telemetryActive() {
    return false;
}
void gameFrame(unsigned long, int, const char*) {}
}  // namespace PetariNative::App::Soak

extern "C" {
const AuroraEvent* aurora_update() {
    hostCall("update");
    current.clear();
    if (!updates.empty()) {
        current = updates.front();
        updates.erase(updates.begin());
    }
    AuroraEvent none{};
    none.type = AURORA_NONE;
    current.push_back(none);
    return current.data();
}
bool aurora_begin_frame() {
    hostCall("begin");
    if (beginFailures > 0) {
        --beginFailures;
        return false;
    }
    return true;
}
void aurora_end_frame() {
    hostCall("end");
}
bool SDL_GetWindowSize(SDL_Window*, int* w, int* h) {
    *w = windowWidth;
    *h = windowHeight;
    return true;
}
// Smoke result gates in the seam; these tests never trip them.
std::uint64_t petari_layout_missing_reference_count() {
    return 0;
}
std::uint64_t petari_sound_missing_reference_count() {
    return 0;
}
std::uint64_t petari_gx_pipeline_manifest_failure_count() {
    return 0;
}
void petari_gx_pipeline_report() {}
}

namespace {

void reset() {
    calls.clear();
    inputEvents.clear();
    updates.clear();
    hostScopeMissed = false;
    beginFailures = 0;
}

void testFirstFrameWithoutHooks() {
    reset();
    App::Seam::attach(reinterpret_cast<SDL_Window*>(0x1));
    beginFailures = 2;
    App::Seam::openFirstFrame();
    check(contains(calls, {"update", "begin", "update", "begin", "update", "begin"}),
          "the first frame is retried until Aurora opens it, with events handled in between");
    check(!hostScopeMissed, "Aurora calls run under a host allocation scope");
    check(hostScopes == 0, "scope closed");
    check(imageUpdates == 1 && lastImage.x == 0.0f && lastImage.width == 1280.0f && lastImage.height == 720.0f &&
              lastWindowWidth == 1280.0f,
          "without a presentation rectangle the pointer spans the window");

    reset();
    petari_host_frame_seam();
    check(contains(calls, {"overlay", "end", "update", "begin"}),
          "seam without hooks: overlay, end frame, events, next frame");
    check(overlay[2] == 1280.0f && overlay[3] == 720.0f, "overlay over the whole window without a rectangle");
    check(imageUpdates == 1, "unchanged viewport is not re-sent");
}

void testSeamWithHooks() {
    App::setPresentHooks({compose, imageRect, nullptr});
    App::setCpuRelease({releaseBegin, releaseEnd});

    reset();
    petari_host_frame_seam();
    check(contains(calls, {"release", "compose", "imageRect", "overlay", "end", "update", "begin", "reacquire"}),
          "seam order: release CPU, compose, overlay, end frame, events, next frame, reacquire");
    check(!hostScopeMissed, "hooks and Aurora run under a host allocation scope");
    check(overlay[0] == 160.0f && overlay[2] == 960.0f, "overlay uses the presentation rectangle");
    check(imageUpdates == 2 && lastImage.x == 160.0f && lastWindowWidth == 1280.0f,
          "pointer viewport follows the presentation rectangle");

    reset();
    petari_host_frame_seam();
    check(imageUpdates == 2, "same rectangle, no viewport update");
    windowWidth = 1440;
    presentImage = {240.0f, 0.0f, 960.0f, 720.0f};
    petari_host_frame_seam();
    check(imageUpdates == 3 && lastImage.x == 240.0f && lastWindowWidth == 1440.0f, "resize updates the viewport");

    imageKnown = false;
    petari_host_frame_seam();
    check(lastImage.x == 0.0f && lastImage.width == 1440.0f, "no image shown: window-wide fallback");
    imageKnown = true;

    // Minimized: the seam keeps the game waiting until a frame opens again,
    // with the CPU released and events still handled.
    reset();
    beginFailures = 3;
    updates = {{}, {sdlKey()}, {}, {}};
    petari_host_frame_seam();
    check(contains(calls, {"release", "compose", "imageRect", "overlay", "end", "update", "begin", "update", "begin",
                           "update", "begin", "update", "begin", "reacquire"}),
          "the seam waits for a presentable window with the CPU released");
    check(inputEvents.size() == 1 && inputEvents[0] == SDL_EVENT_KEY_DOWN, "events reach input while waiting");
}

void testQuit() {
    reset();
    updates = {{sdlKey(), exitEvent()}};
    petari_host_frame_seam();
    check(inputEvents.size() == 1, "SDL events go to input");
    int requests = 0;
    for (const std::string& call : calls) {
        requests += call == "requestQuit";
    }
    check(requests == 1, "closing the window presses the power button once");
    check(calls.back() == "reacquire", "the game continues its shutdown");

    reset();
    updates = {{exitEvent()}};
    bool forced = false;
    try {
        petari_host_frame_seam();
    } catch (const ForcedQuit&) {
        forced = true;
    }
    check(forced, "a second close request quits immediately");
}

void testSmoke() {
    // Opt-in: openFirstFrame reads PETARI_SMOKE. Frames go through the real script.
    setenv("PETARI_SMOKE", "title", 1);
    setenv("PETARI_SMOKE_FRAMES", "100000", 1);
    reset();
    App::Seam::openFirstFrame();
    auto& observation = PetariNative::App::Smoke::smokeObservation;
    observation.scene = "Logo";
    observation.strap = true;
    observation.videoConfigured = true;
    // The strap tap comes at strap frame 480 (the first seam is the move from
    // boot to the Logo phase). Check the order of calls in the seam that taps.
    bool tapped = false;
    for (int i = 0; i < 600 && !tapped; i++) {
        reset();
        petari_host_frame_seam();
        for (const std::string& call : calls) {
            tapped = tapped || call == "press A";
        }
    }
    check(tapped, "the smoke taps A on the strap reminder");
    check(calls.size() >= 4 && calls[0] == "observe" && calls[1] == "focus" && calls[2] == "press A" &&
              calls[3] == "release",
          "the smoke reads the game and presses before the CPU is released");
    reset();
    observation.scene = "Intermission";
    petari_host_frame_seam();
    int requests = 0;
    for (const std::string& call : calls) {
        requests += call == "requestQuit";
    }
    check(requests == 1, "a decided smoke presses the power button");
    check(PetariNative::App::Smoke::processExitStatus() == 1, "and exits with the failure status");
    reset();
    petari_host_frame_seam();
    check(calls[0] == "observe" && calls[1] == "release", "after the result the seam runs as usual");

    // Playable: a published target's normalised position becomes a window
    // point over the presented image.
    setenv("PETARI_SMOKE", "playable", 1);
    App::Seam::openFirstFrame();
    observation = {};
    observation.scene = "Game";
    observation.stage = "FileSelect";
    observation.sceneReady = true;
    observation.milestones = {"FileSelector.FileSelect"};
    observation.targets.push_back({"FileSelect.Slot", 0, 0.25f, 0.5f,
                                   PetariNative::App::Smoke::kTargetEmpty | PetariNative::App::Smoke::kTargetSelectable});
    petari_host_frame_seam();  // image rectangle of this frame
    reset();
    observation.milestones.clear();
    petari_host_frame_seam();
    bool pointed = false;
    for (const std::string& call : calls) {
        pointed = pointed || call == "pointer";
    }
    check(pointed && std::fabs(PetariNative::App::Events::pointerX - (presentImage.x + 0.25f * presentImage.width)) < 0.01f &&
              std::fabs(PetariNative::App::Events::pointerY - (presentImage.y + 0.5f * presentImage.height)) < 0.01f,
          "a target's u,v becomes a window point over the presented image");
    check(!PetariNative::App::Smoke::wantedPlayer, "Mario's position is not requested before the game starts");
    unsetenv("PETARI_SMOKE");
}

namespace FrameStats = PetariNative::App::FrameStats;

FrameStats::Frame statsFrame(double ms, FrameStats::Phase phase) {
    FrameStats::Frame frame;
    frame.intervalMs = ms;
    frame.phase = phase;
    return frame;
}

void testFrameStats() {
    FrameStats::Recorder stats(4);
    std::uint64_t index = 0;
    const auto add = [&](double ms, FrameStats::Phase phase) {
        FrameStats::Frame frame = statsFrame(ms, phase);
        frame.index = ++index;
        frame.us[FrameStats::RetraceWait] = static_cast<std::uint32_t>(ms * 1000.0);
        stats.add(frame);
    };
    for (int i = 0; i < 96; i++) {
        add(16.68, FrameStats::Phase::Gameplay);
    }
    add(40.0, FrameStats::Phase::Gameplay);
    add(33.0, FrameStats::Phase::Gameplay);
    add(21.0, FrameStats::Phase::Loading);  // the run continues across phases for the total
    add(16.68, FrameStats::Phase::Gameplay);
    add(1500.0, FrameStats::Phase::Loading);
    add(9000.0, FrameStats::Phase::Menu);  // past the histogram: max still exact

    const auto& total = stats.totals(FrameStats::kPhaseCount);
    const auto& gameplay = stats.totals(static_cast<unsigned>(FrameStats::Phase::Gameplay));
    const auto& loading = stats.totals(static_cast<unsigned>(FrameStats::Phase::Loading));
    check(total.frames == 102 && gameplay.frames == 99 && loading.frames == 2, "frames counted per phase and in total");
    check(std::fabs(stats.percentile(FrameStats::kPhaseCount, 50) - 16.68) < 0.011, "p50 to the 10 us bin");
    check(std::fabs(stats.percentile(static_cast<unsigned>(FrameStats::Phase::Gameplay), 98) - 33.0) < 0.011,
          "nearest-rank: gameplay p98 of 99 frames is its second-worst frame");
    check(total.maxMs == 9000.0 && stats.percentile(FrameStats::kPhaseCount, 100) == 9000.0,
          "intervals past the histogram keep their exact maximum");
    check(std::fabs(stats.percentile(FrameStats::kPhaseCount, 99) - 1500.0) < 1.01, "1 ms bins above 50 ms");
    check(total.over16 == 5 && total.over33 == 3 && total.late == 5,
          "counts over 16.7 ms, over 33.3 ms, and late (over 1.25 fields)");
    check(total.longestLateRun == 3 && gameplay.longestLateRun == 2 && loading.longestLateRun == 1,
          "late runs: across phases in the total, within a phase for the phase");
    check(gameplay.lateSum[FrameStats::RetraceWait] == 73000, "attribution is summed over late frames");

    const std::string csv = "app_seam_frame_stats.csv";
    check(stats.writeCsv(csv.c_str()), "CSV written");
    std::ifstream in(csv);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    std::remove(csv.c_str());
    check(lines.size() == 5 && lines[0].rfind("frame,phase,unfocused,interval_ms,efb_captures,pipeline_resolves,texture_uploads,seam_compose_ms", 0) == 0,
          "CSV header and the ring's last 4 frames");
    check(lines[1].rfind("99,loading,0,21.000,", 0) == 0 && lines[4].rfind("102,menu,0,9000.000,", 0) == 0,
          "CSV rows oldest first");
    // Summaries print without crashing on these values.
    std::FILE* sink = std::fopen("/dev/null", "w");
    stats.writeSummary(sink);
    stats.writeWindow(sink);
    std::fclose(sink);
}

void testSeamTiming() {
    namespace Telemetry = PetariNative::FrameTelemetry;
    using std::chrono::milliseconds;
    App::setCpuRelease({releaseBegin, releaseEnd});
    reset();
    petari_host_frame_seam();
    const FrameStats::Recorder* stats = App::Seam::frameStats();
    check(stats != nullptr, "statistics exist after the first frame opened");
    const auto before = stats->totals(FrameStats::kPhaseCount);

    // One frame: retrace wait 4 ms, game work 6 ms, draw-done wait 2 ms, and
    // a 5 ms pipeline wait on another thread.
    std::this_thread::sleep_for(milliseconds(4));
    petari_host_frame_mark(PETARI_FRAME_MARK_RETRACE_DONE);
    Telemetry::add(Telemetry::PipelineWait, 5000000);
    std::this_thread::sleep_for(milliseconds(6));
    petari_host_frame_mark(PETARI_FRAME_MARK_END_FRAME);
    std::this_thread::sleep_for(milliseconds(2));
    petari_host_frame_seam();

    const auto& after = stats->totals(FrameStats::kPhaseCount);
    check(after.frames == before.frames + 1, "each seam after the first records one frame");
    const auto part = [&](unsigned p) { return after.allSum[p] - before.allSum[p]; };
    check(part(FrameStats::RetraceWait) >= 4000 && part(FrameStats::GameWork) >= 6000 &&
              part(FrameStats::DrawDoneWait) >= 2000,
          "frame-loop marks split the frame: retrace wait, game work, draw-done wait");
    std::uint64_t gameThread = 0;
    for (unsigned p = 0; p <= FrameStats::Unattributed; p++) {
        gameThread += part(p);
    }
    const double intervalUs = (after.sumMs - before.sumMs) * 1000.0;
    check(std::fabs(gameThread - intervalUs) <= 10.0, "game-thread parts and unattributed add up to the interval");
    check(part(FrameStats::PipelineWait) == 5000 && after.pipelineWaits == before.pipelineWaits + 1,
          "other threads' waits land in the frame they ended in");

    // A scene start that waited for stage shader preparation is a loading frame.
    const unsigned loading = static_cast<unsigned>(FrameStats::Phase::Loading);
    const auto loadingBefore = stats->totals(loading).frames;
    Telemetry::add(Telemetry::StagePrepWait, 9000000);
    petari_host_frame_seam();
    check(stats->totals(loading).frames == loadingBefore + 1 &&
              stats->totals(loading).allSum[FrameStats::StagePrepWait] >= 9000,
          "a frame with a stage shader-prep wait counts as loading, with the wait attributed");

    // Marks out of order (none this frame): the time stays unattributed.
    const auto second = stats->totals(FrameStats::kPhaseCount);
    std::this_thread::sleep_for(milliseconds(1));
    petari_host_frame_seam();
    const auto& third = stats->totals(FrameStats::kPhaseCount);
    check(third.allSum[FrameStats::RetraceWait] == second.allSum[FrameStats::RetraceWait] &&
              third.allSum[FrameStats::Unattributed] >= second.allSum[FrameStats::Unattributed] + 1000,
          "a frame without marks is unattributed, not guessed");

    // Focus loss marks the frame it happened in, and later frames until focus returns.
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    AuroraEvent lostEvent{};
    lostEvent.type = AURORA_SDL_EVENT;
    lostEvent.sdl = lost;
    // Focus is a flag beside the phase: automated runs play unfocused.
    const auto unfocusedCount = [&] { return stats->totals(FrameStats::kPhaseCount).unfocused; };
    const auto gameplayCount = [&] { return stats->totals(static_cast<unsigned>(FrameStats::Phase::Gameplay)).frames; };
    const auto unfocusedBefore = unfocusedCount();
    const auto gameplayBefore = gameplayCount();
    updates = {{lostEvent}};
    petari_host_frame_seam();  // the event arrives in this seam, inside the next frame
    petari_host_frame_seam();
    petari_host_frame_seam();
    check(unfocusedCount() == unfocusedBefore + 2, "frames with focus lost are flagged unfocused");
    check(gameplayCount() == gameplayBefore + 3, "every frame still counts in its phase, focused or not");
    SDL_Event gained{};
    gained.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    AuroraEvent gainedEvent{};
    gainedEvent.type = AURORA_SDL_EVENT;
    gainedEvent.sdl = gained;
    updates = {{gainedEvent}};
    petari_host_frame_seam();
    petari_host_frame_seam();
    petari_host_frame_seam();
    // The frame that began in the seam where focus returned was unfocused at its start.
    check(unfocusedCount() == unfocusedBefore + 4,
          "frames stay unfocused through the one focus returned in; later ones are not");
}

// A named busy loop for the spike profiler test.
__attribute__((noinline)) void spikeProfilerBusyWork(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    volatile std::uint64_t sink = 0;
    while (std::chrono::steady_clock::now() < end) {
        sink = sink + 1;
    }
}

void testSpikeProfiler() {
    namespace Profiler = PetariNative::App::SpikeProfiler;
    namespace Telemetry = PetariNative::FrameTelemetry;
    const std::string path = "app_seam_spike_profile.txt";
    setenv("PETARI_SPIKE_PROFILE", path.c_str(), 1);
    check(Profiler::startFromEnvironment() && Profiler::enabled(), "spike profiler starts from the environment");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t start = Telemetry::nowNs();
    spikeProfilerBusyWork(std::chrono::milliseconds(60));
    const std::uint64_t end = Telemetry::nowNs();
    Profiler::keepSpike(7, 60.0, start, end);
    Profiler::writeReport();
    std::ifstream in(path);
    const std::string report((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::remove(path.c_str());
    unsetenv("PETARI_SPIKE_PROFILE");
    check(report.find("=== frame 7: 60.00 ms ===") != std::string::npos, "the kept spike is reported");
    const auto game = report.find("-- game (main): ");
    check(game != std::string::npos && std::atoi(report.c_str() + game + 16) >= 20,
          "the game thread was sampled through the spike");
    check(report.find("spikeProfilerBusyWork") != std::string::npos, "samples are symbolized to the busy function");
}

}  // namespace

int main() {
    testFirstFrameWithoutHooks();
    testSeamWithHooks();
    testQuit();
    testSmoke();
    testFrameStats();
    testSeamTiming();
    testSpikeProfiler();
    std::printf("native app seam tests passed (%d checks)\n", checks);
    return 0;
}

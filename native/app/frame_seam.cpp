// The per-frame host seam (petari/app.hpp). Aurora and SDL side.

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <SDL3/SDL_video.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" uint64_t petari_gx_pipeline_manifest_failure_count();
extern "C" void petari_gx_pipeline_report();

#include "frame_stats.hpp"
#include "spike_profiler.hpp"
#include "host.hpp"
#include "smoke.hpp"
#include "smoke_domes.hpp"
#include "smoke_goodegg.hpp"
#include "smoke_soak.hpp"
#include "smoke_stage.hpp"
#include "soak_telemetry.hpp"
#include "petari/asset_diagnostics.hpp"
#include "petari/frame_telemetry.hpp"
#include "petari/home_menu.hpp"
#include "petari/host_allocation.hpp"
#include "petari/test_fixture.hpp"

namespace PetariNative::App {

namespace {

PresentHooks gPresent;
CpuRelease gRelease;
SDL_Window* gWindow = nullptr;
int gQuitRequests = 0;
Rect gLastImage;
float gLastWindowWidth = -1.0f;
float gLastWindowHeight = -1.0f;
bool gWarnedImageRect = false;

// Opt-in start-up telemetry (PETARI_TRACE_BOOT=1): the first frame that
// reaches the seam, then one line every kTraceInterval frames. Observation only.
constexpr unsigned kTraceInterval = 120;
using TraceClock = std::chrono::steady_clock;
struct Trace {
    bool enabled = false;
    unsigned long frames = 0;
    unsigned long lastFrames = 0;
    unsigned long unpresentableWaits = 0;  // failed aurora_begin_frame calls
    TraceClock::time_point start;
    TraceClock::time_point last;
    TraceClock::time_point previousFrame;
    double maxFrameMs = 0.0;
    unsigned slowFrames = 0;
};
Trace gTrace;

// Frame-time statistics (frame_stats.hpp): always collected, printed at exit
// with PETARI_TRACE_BOOT or PETARI_FRAME_STATS, per-frame rows written to
// PETARI_FRAME_CSV. Everything here runs on the game (main) thread.
namespace Telemetry = PetariNative::FrameTelemetry;
unsigned long environmentNumber(const char* name, unsigned long fallback);
struct Timing {
    FrameStats::Recorder* recorder = nullptr;
    FrameStats::Phase (*probe)() = nullptr;
    bool report = false;
    const char* csvPath = nullptr;
    double spikeMs = 1000.0 / 30.0;  // PETARI_SPIKE_PROFILE_MS
    std::uint64_t frames = 0;
    std::uint64_t lastEntry = 0;  // previous seam entry, 0 before the first
    std::uint64_t lastExit = 0;
    std::uint64_t seamNs[4] = {};  // previous seam: compose, end frame, begin frame, reacquire
    std::uint64_t retraceDone = 0;  // frame loop marks, this frame
    std::uint64_t retraceWake = 0;
    std::uint64_t endFrame = 0;
    bool unfocused = false;        // window focus, from events
    bool unfocusedInFrame = false; // unfocused or unpresentable at any time this frame
    std::uint64_t counterNs[Telemetry::CounterCount] = {};
    std::uint64_t counterEvents[Telemetry::CounterCount] = {};
};
Timing gTiming;

std::uint32_t microseconds(std::uint64_t ns) {
    const std::uint64_t us = ns / 1000;
    return us > 0xffffffffu ? 0xffffffffu : static_cast<std::uint32_t>(us);
}

// Since the previous call: time and events of a cross-thread counter.
std::uint64_t takeCounter(Telemetry::Counter counter, std::uint64_t* events = nullptr) {
    const std::uint64_t ns = Telemetry::slots[counter].ns.load(std::memory_order_relaxed);
    const std::uint64_t count = Telemetry::slots[counter].events.load(std::memory_order_relaxed);
    const std::uint64_t delta = ns - gTiming.counterNs[counter];
    if (events != nullptr) {
        *events = count - gTiming.counterEvents[counter];
    }
    gTiming.counterNs[counter] = ns;
    gTiming.counterEvents[counter] = count;
    return delta;
}

std::uint64_t takeMax(Telemetry::Counter counter) {
    return Telemetry::slots[counter].maxNs.exchange(0, std::memory_order_relaxed);
}

// At a seam entry: the frame since the previous seam entry.
void recordFrame(std::uint64_t entry, FrameStats::Phase phase) {
    Timing& t = gTiming;
    if (t.recorder == nullptr) {
        return;
    }
    if (t.lastEntry == 0) {
        for (unsigned counter = 0; counter < Telemetry::CounterCount; counter++) {
            takeCounter(static_cast<Telemetry::Counter>(counter));
            takeMax(static_cast<Telemetry::Counter>(counter));
        }
        t.lastEntry = entry;
        return;
    }
    FrameStats::Frame frame;
    frame.index = ++t.frames;
    frame.intervalMs = static_cast<double>(entry - t.lastEntry) / 1e6;
    frame.phase = phase;
    frame.unfocused = t.unfocusedInFrame;
    frame.us[FrameStats::SeamCompose] = microseconds(t.seamNs[0]);
    frame.us[FrameStats::SeamEndFrame] = microseconds(t.seamNs[1]);
    frame.us[FrameStats::SeamBeginFrame] = microseconds(t.seamNs[2]);
    frame.us[FrameStats::SeamReacquire] = microseconds(t.seamNs[3]);
    // The frame loop's marks, if they fall in order inside this frame.
    const bool retrace = t.retraceDone >= t.lastExit && t.retraceDone <= entry && t.lastExit != 0;
    const std::uint64_t workStart = retrace ? t.retraceDone : t.lastExit;
    const bool endFrame = t.endFrame >= workStart && t.endFrame <= entry && t.lastExit != 0;
    if (retrace) {
        frame.us[FrameStats::RetraceWait] = microseconds(t.retraceDone - t.lastExit);
        frame.us[FrameStats::RetraceWake] = microseconds(t.retraceWake);
    }
    if (retrace && endFrame) {
        frame.us[FrameStats::GameWork] = microseconds(t.endFrame - t.retraceDone);
    }
    if (endFrame) {
        frame.us[FrameStats::DrawDoneWait] = microseconds(entry - t.endFrame);
    }
    std::uint64_t attributed = 0;
    for (unsigned part = 0; part < FrameStats::Unattributed; part++) {
        attributed += frame.us[part];
    }
    const std::uint64_t intervalUs = (entry - t.lastEntry) / 1000;
    frame.us[FrameStats::Unattributed] = microseconds(intervalUs > attributed ? (intervalUs - attributed) * 1000 : 0);

    std::uint64_t events = 0;
    frame.us[FrameStats::PipelineWait] = microseconds(takeCounter(Telemetry::PipelineWait, &events));
    frame.pipelineWaits = static_cast<std::uint32_t>(events);
    frame.us[FrameStats::EfbStagingWait] = microseconds(takeCounter(Telemetry::EfbStagingWait));
    frame.us[FrameStats::EfbSubmitWait] = microseconds(takeCounter(Telemetry::EfbSubmitWait, &events));
    frame.efbCaptures = static_cast<std::uint32_t>(events);
    takeCounter(Telemetry::EfbCaptureLatency);
    frame.us[FrameStats::EfbCaptureMax] = microseconds(takeMax(Telemetry::EfbCaptureLatency));
    frame.us[FrameStats::DrawableAcquire] = microseconds(takeCounter(Telemetry::DrawableAcquire));
    frame.us[FrameStats::FrameSubmit] = microseconds(takeCounter(Telemetry::FrameSubmit));
    frame.us[FrameStats::PresentCall] = microseconds(takeCounter(Telemetry::PresentCall));
    takeCounter(Telemetry::ViTimerLate);
    frame.us[FrameStats::ViTimerLateMax] = microseconds(takeMax(Telemetry::ViTimerLate));
    takeCounter(Telemetry::ViInterruptLockWait);
    frame.us[FrameStats::ViLockWaitMax] = microseconds(takeMax(Telemetry::ViInterruptLockWait));
    frame.us[FrameStats::TextureHash] = microseconds(takeCounter(Telemetry::TextureHash));
    frame.us[FrameStats::TextureUpload] = microseconds(takeCounter(Telemetry::TextureUpload, &events));
    frame.textureUploads = static_cast<std::uint32_t>(events);
    frame.us[FrameStats::TokenBarrierWait] = microseconds(takeCounter(Telemetry::TokenBarrierWait));
    frame.us[FrameStats::StagePrepWait] = microseconds(takeCounter(Telemetry::StagePrepWait));
    // A scene start that waited for its stage's shaders is part of loading,
    // though the scene reports itself ready by the frame's end.
    if (frame.us[FrameStats::StagePrepWait] >= 1000) {
        frame.phase = FrameStats::Phase::Loading;
    }
    t.recorder->add(frame);
    if (frame.intervalMs > t.spikeMs && SpikeProfiler::enabled()) {
        SpikeProfiler::keepSpike(frame.index, frame.intervalMs, t.lastEntry, entry);
    }

    t.lastEntry = entry;
    t.retraceDone = t.retraceWake = t.endFrame = 0;
    t.unfocusedInFrame = t.unfocused;
}

void startTiming() {
    const auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    };
    const char* csv = std::getenv("PETARI_FRAME_CSV");
    gTiming.csvPath = csv != nullptr && csv[0] != '\0' ? csv : nullptr;
    gTiming.report = gTiming.csvPath != nullptr || enabled("PETARI_FRAME_STATS") || enabled("PETARI_TRACE_BOOT");
    if (SpikeProfiler::startFromEnvironment()) {
        const unsigned long spikeMs = environmentNumber("PETARI_SPIKE_PROFILE_MS", 0);
        if (spikeMs > 0) {
            gTiming.spikeMs = static_cast<double>(spikeMs);
        }
    }
    if (gTiming.recorder == nullptr) {
        // 30 minutes at 60 frames/s by default, about 100 bytes per frame.
        const std::size_t csvFrames = gTiming.csvPath != nullptr ? environmentNumber("PETARI_FRAME_CSV_FRAMES", 108000) : 0;
        gTiming.recorder = new FrameStats::Recorder(csvFrames);
    }
}

// The automated smoke run (smoke.hpp), when PETARI_SMOKE selects a script.
Smoke::Driver* gSmoke = nullptr;
// Or the stage script (smoke_stage.hpp), PETARI_SMOKE=stage.
Smoke::StageDriver* gStage = nullptr;
// Or Good Egg mission 1 (smoke_goodegg.hpp), PETARI_SMOKE=goodegg1.
Smoke::GoodEggDriver* gGoodEgg = nullptr;
// Or the long-session soak (smoke_soak.hpp), PETARI_SMOKE=soak.
Smoke::SoakDriver* gSoak = nullptr;
// Or the dome tour (smoke_domes.hpp), PETARI_SMOKE=domes.
Smoke::DomesDriver* gDomes = nullptr;

unsigned long environmentNumber(const char* name, unsigned long fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long number = std::strtoul(value, &end, 10);
    return end != nullptr && *end == '\0' && number > 0 ? number : fallback;
}

void startSmoke() {
    Soak::startTelemetryFromEnvironment();  // PETARI_SOAK_CSV, with any script or none
    Smoke::SoakConfig soak;
    if (Smoke::soakEnabledFromEnvironment(&soak)) {
        if (TestFixture::stage.empty()) {
            std::fprintf(stderr, "PETARI SMOKE: soak needs --test-fixture stage (with PETARI_STAGE); not running\n");
            return;
        }
        // Only a validated stage fixture's entry is ever changed.
        soak.setStage = [](const std::string& stage, int scenario) {
            if (!TestFixture::stage.empty()) {
                TestFixture::stage = stage;
                TestFixture::stageScenario = scenario;
            }
        };
        const unsigned long frames = environmentNumber("PETARI_SMOKE_FRAMES", 20000);  // per stage cycle
        const unsigned long stall = environmentNumber("PETARI_SMOKE_STALL_SECONDS", 60);
        gSoak = new Smoke::SoakDriver(frames, soak);
        std::fprintf(stderr, "PETARI SMOKE: script soak (%zu stage(s), %.0f minutes), stage frame limit %lu, stall limit %lu s\n",
                     soak.stages.size(), soak.minutes, frames, stall);
        std::fflush(stderr);
        Smoke::startWatchdog(static_cast<unsigned>(stall), 20);
        return;
    }
    Smoke::DomesConfig domes;
    if (Smoke::domesEnabledFromEnvironment(&domes)) {
        const unsigned long frames = environmentNumber("PETARI_SMOKE_FRAMES", 72000);
        const unsigned long stall = environmentNumber("PETARI_SMOKE_STALL_SECONDS", 60);
        gDomes = new Smoke::DomesDriver(frames, domes);
        std::fprintf(stderr, "PETARI SMOKE: script domes (dome %d, %zu extra mission(s)), frame limit %lu, stall limit %lu s\n",
                     domes.dome, domes.extras.size(), frames, stall);
        std::fflush(stderr);
        Smoke::startWatchdog(static_cast<unsigned>(stall), 20);
        return;
    }
    Smoke::GoodEggConfig goodEgg;
    if (Smoke::goodEggEnabledFromEnvironment(&goodEgg)) {
        const unsigned long frames = environmentNumber("PETARI_SMOKE_FRAMES", 72000);
        const unsigned long stall = environmentNumber("PETARI_SMOKE_STALL_SECONDS", 60);
        gGoodEgg = new Smoke::GoodEggDriver(frames, goodEgg);
        std::fprintf(stderr, "PETARI SMOKE: script goodegg1 (%s), frame limit %lu, stall limit %lu s\n",
                     goodEgg.synthetic ? "synthetic stage entry" : "galaxy route", frames, stall);
        std::fflush(stderr);
        Smoke::startWatchdog(static_cast<unsigned>(stall), 20);
        return;
    }
    Smoke::StageConfig stage;
    if (Smoke::stageEnabledFromEnvironment(&stage)) {
        const unsigned long frames = environmentNumber("PETARI_SMOKE_FRAMES", 12000);
        const unsigned long stall = environmentNumber("PETARI_SMOKE_STALL_SECONDS", 60);
        gStage = new Smoke::StageDriver(frames, stage);
        std::fprintf(stderr, "PETARI SMOKE: script stage (%s scenario %d, synthetic entry), frame limit %lu, stall limit %lu s\n",
                     stage.stage.c_str(), stage.scenario, frames, stall);
        std::fflush(stderr);
        Smoke::startWatchdog(static_cast<unsigned>(stall), 20);
        return;
    }
    Smoke::Script script = Smoke::Script::Title;
    if (!Smoke::enabledFromEnvironment(&script)) {
        return;
    }
    const unsigned long frames = environmentNumber("PETARI_SMOKE_FRAMES", 7200);
    const unsigned long stall = environmentNumber("PETARI_SMOKE_STALL_SECONDS", 60);
    gSmoke = new Smoke::Driver(frames, script);
    const char* scriptName = "title";
    switch (script) {
    case Smoke::Script::Title: break;
    case Smoke::Script::Playable: scriptName = "playable"; break;
    case Smoke::Script::Gameplay: scriptName = "gameplay"; break;
    case Smoke::Script::Reload: scriptName = "reload"; break;
    case Smoke::Script::Story: scriptName = "story"; break;
    case Smoke::Script::Galaxy: scriptName = "galaxy"; break;
    }
    std::fprintf(stderr, "PETARI SMOKE: script %s, frame limit %lu, stall limit %lu s\n", scriptName, frames, stall);
    std::fflush(stderr);
    Smoke::startWatchdog(static_cast<unsigned>(stall), 20);
}

// Game state is read here, before the seam releases the CPU, while this
// thread owns the game. Driver is Smoke::Driver or Smoke::StageDriver.
template <class Driver>
void runSmoke(Driver* driver) {
    Smoke::Observation observation = Smoke::observeGame(driver->wantsPlayer());
    observation.physical = Events::physicalInputs();
    const Smoke::Step step = driver->step(observation);
    for (const std::string& line : driver->log()) {
        std::fprintf(stderr, "PETARI SMOKE [frame %lu]: %s\n", driver->frame(), line.c_str());
    }
    if (!driver->log().empty()) {
        std::fflush(stderr);
    }
    if (step.assertFocus) {
        Events::assertFocus();
    }
    if (step.pointer) {
        // Normalised over the game image, as the input layer maps the pointer.
        Events::movePointer(gLastImage.x + step.pointerU * gLastImage.width,
                            gLastImage.y + step.pointerV * gLastImage.height);
    }
    for (const Smoke::Press& press : step.presses) {
        Events::pressButton(static_cast<int>(press.button), press.down);
    }
    if (step.warp && !Smoke::warpPlayer(step)) {
        std::fprintf(stderr, "PETARI SMOKE: warp requested with no player to move\n");
    }
    Smoke::heartbeat(driver->frame(), driver->phase());
    if (step.requestQuit) {
        // A route that passed while looking up layout panes/animations or sounds that do
        // not exist in the disc data did not really pass: those lookups were skipped.
        Smoke::Result result = driver->result();
        std::string reason = driver->reason();
        const uint64_t missingLayout = petari_layout_missing_reference_count();
        const uint64_t missingSound = petari_sound_missing_reference_count();
        const uint64_t manifestFailures = petari_gx_pipeline_manifest_failure_count();
        if ((missingLayout > 0 || missingSound > 0) &&
            (result == Smoke::Result::Pass || result == Smoke::Result::Assisted)) {
            result = Smoke::Result::Fail;
            reason += "; FAIL: missing asset references (layout " + std::to_string(missingLayout) + ", sound " +
                      std::to_string(missingSound) + "; see [layout]/[sound] missing lines)";
        }
        // A bundled stage shader manifest that exists but fails to load silently degrades
        // stage preparation to the shared fallback; treat it as a failed run.
        if (manifestFailures > 0 && (result == Smoke::Result::Pass || result == Smoke::Result::Assisted)) {
            result = Smoke::Result::Fail;
            reason += "; FAIL: " + std::to_string(manifestFailures) +
                      " pipeline manifest read failure(s) (see [gx stage prep] manifest read failed)";
        }
        Smoke::setProcessResult(result);
        Smoke::noteQuitRequested(observation.saveSequence);
        std::fprintf(stderr, "PETARI SMOKE RESULT: %s (%s); pressing the power button%s\n",
                     Smoke::resultName(result), reason.c_str(),
                     observation.saveSequence ? " while the save-data sequence is active" : "");
        std::fflush(stderr);
        Host::requestQuit();
    }
}

double seconds(TraceClock::duration d) {
    return std::chrono::duration<double>(d).count();
}

void traceFrame(const Rect& image) {
    if (!gTrace.enabled) {
        return;
    }
    ++gTrace.frames;
    const bool first = gTrace.frames == 1;
    const auto now = TraceClock::now();
    if (!first) {
        const double frameMs = seconds(now - gTrace.previousFrame) * 1000.0;
        if (frameMs > gTrace.maxFrameMs) gTrace.maxFrameMs = frameMs;
        if (frameMs > 1000.0 / 30.0) ++gTrace.slowFrames;
    }
    gTrace.previousFrame = now;
    if (!first && gTrace.frames % kTraceInterval != 0) {
        return;
    }
    if (!first && gTiming.recorder != nullptr) {
        gTiming.recorder->writeWindow(stderr);
    }
    const double sinceLast = seconds(now - gTrace.last);
    std::fprintf(stderr,
                 "Petari trace: frame %lu%s at %.2f s (%.1f frames/s since last), image %.0fx%.0f at %.0f,%.0f, "
                 "unpresentable waits %lu, max interval %.2f ms, intervals over 33.3 ms %u\n",
                 gTrace.frames, first ? " (first frame reached the seam)" : "", seconds(now - gTrace.start),
                 first || sinceLast <= 0.0 ? 0.0 : (gTrace.frames - gTrace.lastFrames) / sinceLast, image.width, image.height, image.x, image.y,
                 gTrace.unpresentableWaits, gTrace.maxFrameMs, gTrace.slowFrames);
    std::fflush(stderr);
    gTrace.last = now;
    gTrace.lastFrames = gTrace.frames;
    gTrace.maxFrameMs = 0.0;
    gTrace.slowFrames = 0;
}

void handleEvents(const AuroraEvent* event) {
    for (; event != nullptr && event->type != AURORA_NONE; ++event) {
        switch (event->type) {
        case AURORA_EXIT:
            if (++gQuitRequests == 1) {
                std::fprintf(stderr, "Petari: quitting; close the window again to quit immediately\n");
                Host::requestQuit();
            } else {
                Host::forceQuit();
            }
            break;
        case AURORA_SDL_EVENT:
            if (event->sdl.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                gTiming.unfocused = gTiming.unfocusedInFrame = true;
            } else if (event->sdl.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                gTiming.unfocused = false;
            } else if (event->sdl.type == SDL_EVENT_AUDIO_DEVICE_ADDED ||
                       event->sdl.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ||
                       event->sdl.type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED) {
                // Device switches migrate the stream and leave an audible gap; log them.
                std::fprintf(stderr, "[audio] SDL device event %s: device %u%s\n",
                             event->sdl.type == SDL_EVENT_AUDIO_DEVICE_ADDED     ? "added"
                             : event->sdl.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ? "removed"
                                                                                  : "format changed",
                             static_cast<unsigned>(event->sdl.adevice.which),
                             event->sdl.adevice.recording ? " (recording)" : "");
            }
            Events::input(event->sdl);
            break;
        default:
            break;
        }
    }
}

// Opens the next Aurora frame. While the window cannot present (minimized,
// hidden), aurora_update waits for window events and the game stays here.
void openFrame() {
    while (!aurora_begin_frame()) {
        ++gTrace.unpresentableWaits;
        gTiming.unfocusedInFrame = true;
        handleEvents(aurora_update());
    }
}

// The game image in window points, told to the input layer when it changes.
Rect updateImage() {
    int windowWidth = 0;
    int windowHeight = 0;
    if (gWindow != nullptr) {
        SDL_GetWindowSize(gWindow, &windowWidth, &windowHeight);
    }
    Rect image;
    const bool known = gPresent.imageRect != nullptr && gPresent.imageRect(&image, gPresent.user);
    if (!known) {
        if (gPresent.imageRect == nullptr && !gWarnedImageRect) {
            gWarnedImageRect = true;
            std::fprintf(stderr,
                         "Petari: no presentation image rectangle; the pointer and overlays use the whole window\n");
        }
        image = {0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight)};
    }
    const float width = static_cast<float>(windowWidth);
    const float height = static_cast<float>(windowHeight);
    if (image.x != gLastImage.x || image.y != gLastImage.y || image.width != gLastImage.width ||
        image.height != gLastImage.height || width != gLastWindowWidth || height != gLastWindowHeight) {
        gLastImage = image;
        gLastWindowWidth = width;
        gLastWindowHeight = height;
        Events::setImage(image, width, height);
    }
    return image;
}

}  // namespace

void setPresentHooks(const PresentHooks& hooks) {
    gPresent = hooks;
}

void setCpuRelease(const CpuRelease& release) {
    gRelease = release;
}

namespace Seam {

void attach(SDL_Window* window) {
    gWindow = window;
}

void openFirstFrame() {
    HostAllocationScope host;
    const char* trace = std::getenv("PETARI_TRACE_BOOT");
    gTrace.enabled = trace != nullptr && trace[0] != '\0' && trace[0] != '0';
    gTrace.start = gTrace.last = TraceClock::now();
    startTiming();
    startSmoke();
    if (gPresent.composeFrame == nullptr) {
        std::fprintf(stderr,
                     "Petari: no presentation hook; the window shows Aurora's frame as is (not the GXCopyDisp "
                     "image, and without VI black or dimming)\n");
    }
    if (gRelease.begin == nullptr || gRelease.end == nullptr) {
        std::fprintf(stderr,
                     "Petari: no CPU release for the frame seam; other game threads wait while the window "
                     "presents and handles events\n");
    }
    // As in the regular seam, other game threads run while the host opens the frame
    // (first-use Dawn/ImGui pipeline creation can take hundreds of milliseconds).
    const bool release = gRelease.begin != nullptr && gRelease.end != nullptr;
    if (release) {
        gRelease.begin();
    }
    handleEvents(aurora_update());
    openFrame();
    updateImage();
    if (release) {
        gRelease.end();
    }
    if (gTrace.enabled) {
        std::fprintf(stderr, "Petari trace: first Aurora frame open; starting the game\n");
        std::fflush(stderr);
    }
}

void setPhaseProbe(FrameStats::Phase (*probe)()) {
    gTiming.probe = probe;
}

const FrameStats::Recorder* frameStats() {
    return gTiming.recorder;
}

void reportFrameStats() {
    // Exit can come on a game thread whose plain new/delete use the game heap.
    HostAllocationScope host;
    SpikeProfiler::writeReport();
    static std::atomic<bool> reported{false};
    if (gTiming.recorder == nullptr || !gTiming.report || reported.exchange(true)) {
        return;
    }
    gTiming.recorder->writeSummary(stderr);
    if (gTiming.csvPath != nullptr) {
        if (gTiming.recorder->writeCsv(gTiming.csvPath)) {
            std::fprintf(stderr, "Petari frame times: per-frame rows written to %s\n", gTiming.csvPath);
        } else {
            std::fprintf(stderr, "Petari frame times: cannot write %s\n", gTiming.csvPath);
        }
    }
    // Pipeline scheduler state at exit (pending/failed/skipped); normal exits skip aurora_shutdown.
    petari_gx_pipeline_report();
    std::fflush(stderr);
}

}  // namespace Seam

}  // namespace PetariNative::App

using namespace PetariNative::App;

extern "C" void petari_host_frame_mark(int mark) {
    const std::uint64_t now = Telemetry::nowNs();
    if (mark == PETARI_FRAME_MARK_RETRACE_DONE) {
        gTiming.retraceDone = now;
        // The retrace that ended the wait, if one came after the seam.
        const std::uint64_t retrace = Telemetry::lastRetraceNs.load(std::memory_order_relaxed);
        gTiming.retraceWake = retrace > gTiming.lastExit && retrace <= now ? now - retrace : 0;
    } else if (mark == PETARI_FRAME_MARK_END_FRAME) {
        gTiming.endFrame = now;
    }
}

extern "C" void petari_host_frame_seam(void) {
    const std::uint64_t entry = Telemetry::nowNs();
    PetariNative::HostAllocationScope host;
    // Game state is readable here, before the CPU is released.
    recordFrame(entry, gTiming.probe != nullptr ? gTiming.probe() : FrameStats::Phase::Gameplay);
    if (gSmoke != nullptr) {
        runSmoke(gSmoke);
    } else if (gStage != nullptr) {
        runSmoke(gStage);
    } else if (gSoak != nullptr) {
        runSmoke(gSoak);
    } else if (gGoodEgg != nullptr) {
        runSmoke(gGoodEgg);
    } else if (gDomes != nullptr) {
        runSmoke(gDomes);
    }
    if (Soak::telemetryActive()) {  // PETARI_SOAK_CSV: frame times and, periodically, the JKR heaps
        static unsigned long frames = 0;
        Soak::gameFrame(++frames, gSoak != nullptr ? gSoak->cycle() : 0, gSoak != nullptr ? gSoak->phase() : "");
    }
    const bool release = gRelease.begin != nullptr && gRelease.end != nullptr;
    if (release) {
        gRelease.begin();
    }

    const std::uint64_t composeStart = Telemetry::nowNs();
    if (gPresent.composeFrame != nullptr) {
        gPresent.composeFrame(gPresent.user);
    }
    const Rect image = updateImage();
    traceFrame(image);
    PetariNative::HomeMenu::drawImGuiOverlay(image.x, image.y, image.width, image.height);
    const std::uint64_t endStart = Telemetry::nowNs();
    aurora_end_frame();

    const std::uint64_t beginStart = Telemetry::nowNs();
    handleEvents(aurora_update());
    openFrame();

    const std::uint64_t reacquireStart = Telemetry::nowNs();
    if (release) {
        gRelease.end();
    }
    const std::uint64_t exit = Telemetry::nowNs();
    gTiming.seamNs[0] = endStart - composeStart;
    gTiming.seamNs[1] = beginStart - endStart;
    gTiming.seamNs[2] = reacquireStart - beginStart;
    gTiming.seamNs[3] = exit - reacquireStart;
    gTiming.lastExit = exit;
}

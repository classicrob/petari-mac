// The per-frame host seam (petari/app.hpp). Aurora and SDL side.

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <SDL3/SDL_video.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "host.hpp"
#include "smoke.hpp"
#include "petari/home_menu.hpp"
#include "petari/host_allocation.hpp"

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
};
Trace gTrace;

// The automated smoke run (smoke.hpp), when PETARI_SMOKE selects a script.
Smoke::Driver* gSmoke = nullptr;

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
    if (!Smoke::enabledFromEnvironment()) {
        return;
    }
    const unsigned long frames = environmentNumber("PETARI_SMOKE_FRAMES", 7200);
    const unsigned long stall = environmentNumber("PETARI_SMOKE_STALL_SECONDS", 60);
    gSmoke = new Smoke::Driver(frames);
    std::fprintf(stderr, "PETARI SMOKE: script title, frame limit %lu, stall limit %lu s\n", frames, stall);
    std::fflush(stderr);
    Smoke::startWatchdog(static_cast<unsigned>(stall), 20);
}

// Game state is read here, before the seam releases the CPU, while this
// thread owns the game.
void runSmoke() {
    const Smoke::Observation observation = Smoke::observeGame();
    const Smoke::Step step = gSmoke->step(observation);
    for (const std::string& line : gSmoke->log()) {
        std::fprintf(stderr, "PETARI SMOKE [frame %lu]: %s\n", gSmoke->frame(), line.c_str());
    }
    if (!gSmoke->log().empty()) {
        std::fflush(stderr);
    }
    if (step.assertFocus) {
        Events::assertFocus();
    }
    for (const Smoke::Press& press : step.presses) {
        Events::pressButton(press.button == Smoke::Button::A, press.down);
    }
    Smoke::heartbeat(gSmoke->frame(), gSmoke->phase());
    if (step.requestQuit) {
        Smoke::setProcessResult(gSmoke->result());
        Smoke::noteQuitRequested(observation.saveSequence);
        std::fprintf(stderr, "PETARI SMOKE RESULT: %s (%s); pressing the power button%s\n",
                     Smoke::resultName(gSmoke->result()), gSmoke->reason().c_str(),
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
    if (!first && gTrace.frames % kTraceInterval != 0) {
        return;
    }
    const auto now = TraceClock::now();
    const double sinceLast = seconds(now - gTrace.last);
    std::fprintf(stderr,
                 "Petari trace: frame %lu%s at %.2f s (%.1f frames/s since last), image %.0fx%.0f at %.0f,%.0f, "
                 "unpresentable waits %lu\n",
                 gTrace.frames, first ? " (first frame reached the seam)" : "", seconds(now - gTrace.start),
                 first || sinceLast <= 0.0 ? 0.0 : (gTrace.frames - gTrace.lastFrames) / sinceLast, image.width, image.height, image.x, image.y,
                 gTrace.unpresentableWaits);
    std::fflush(stderr);
    gTrace.last = now;
    gTrace.lastFrames = gTrace.frames;
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
    handleEvents(aurora_update());
    openFrame();
    updateImage();
    if (gTrace.enabled) {
        std::fprintf(stderr, "Petari trace: first Aurora frame open; starting the game\n");
        std::fflush(stderr);
    }
}

}  // namespace Seam

}  // namespace PetariNative::App

using namespace PetariNative::App;

extern "C" void petari_host_frame_seam(void) {
    PetariNative::HostAllocationScope host;
    if (gSmoke != nullptr) {
        runSmoke();
    }
    const bool release = gRelease.begin != nullptr && gRelease.end != nullptr;
    if (release) {
        gRelease.begin();
    }

    if (gPresent.composeFrame != nullptr) {
        gPresent.composeFrame(gPresent.user);
    }
    const Rect image = updateImage();
    traceFrame(image);
    PetariNative::HomeMenu::drawImGuiOverlay(image.x, image.y, image.width, image.height);
    aurora_end_frame();

    handleEvents(aurora_update());
    openFrame();

    if (release) {
        gRelease.end();
    }
}

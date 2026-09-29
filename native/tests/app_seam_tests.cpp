// Tests for the native frame seam (native/app/frame_seam.cpp): the order of
// host work around a game frame, keeping an Aurora frame open, window events,
// the quit path, and the presentation and CPU-release hooks. Aurora, SDL and
// the app's other translation units are replaced by recording fakes here.

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <SDL3/SDL_video.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../app/host.hpp"
#include "../app/smoke.hpp"
#include "petari/app.hpp"
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

}  // namespace

int main() {
    testFirstFrameWithoutHooks();
    testSeamWithHooks();
    testQuit();
    testSmoke();
    std::printf("native app seam tests passed (%d checks)\n", checks);
    return 0;
}

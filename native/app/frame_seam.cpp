// The per-frame host seam (petari/app.hpp). Aurora and SDL side.

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <SDL3/SDL_video.h>

#include <cstdio>

#include "host.hpp"
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
}

}  // namespace Seam

}  // namespace PetariNative::App

using namespace PetariNative::App;

extern "C" void petari_host_frame_seam(void) {
    PetariNative::HostAllocationScope host;
    const bool release = gRelease.begin != nullptr && gRelease.end != nullptr;
    if (release) {
        gRelease.begin();
    }

    if (gPresent.composeFrame != nullptr) {
        gPresent.composeFrame(gPresent.user);
    }
    const Rect image = updateImage();
    PetariNative::HomeMenu::drawImGuiOverlay(image.x, image.y, image.width, image.height);
    aurora_end_frame();

    handleEvents(aurora_update());
    openFrame();

    if (release) {
        gRelease.end();
    }
}

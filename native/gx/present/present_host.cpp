// SDK side of XFB presentation: the application's presentation hooks
// (petari/app.hpp), fed from the native VI and SC state. No Aurora headers.

#include <petari/app.hpp>
#include <petari/latency_probe.hpp>
#include <petari/platform/sc.hpp>
#include <petari/platform/vi.hpp>
#include <revolution/sc.h>

#include "present.h"

namespace {

namespace VI = PetariNative::Platform::VI;
namespace SC = PetariNative::Platform::SC;

// The frame the seam is about to present: the XFB VI latched at the last
// retrace, and VI's black and dimming state.
void composeFrame(void*) {
    const VI::DisplayState display = VI::displayState();
    // PETARI_PRESENT_XFB=latest: the newest display copy instead of the latched
    // one (the copy is in this frame's FIFO, drained before Aurora presents).
    // VI's black and dimming state still apply.
    const void* xfb = display.frameBuffer;
    if (PetariNative::PresentTiming::latest() && display.frameBuffer != nullptr) {
        if (const void* newest = PetariNative::PresentTiming::lastDisplayCopy.load(std::memory_order_relaxed)) xfb = newest;
    }
    // Latency probe: this seam's Aurora frame presents that XFB.
    const std::uint32_t frame = PetariNative::LatencyProbe::frame.load(std::memory_order_relaxed);
    if (!display.black) PetariNative::LatencyProbe::noteShown(xfb, frame);
    PetariNative::LatencyProbe::frame.store(frame + 1, std::memory_order_relaxed);
    PetariPresentVideo video{};
    video.xfb = xfb;
    video.configured = display.configured;
    video.black = display.black;
    video.dimmed = display.dimmed;
    // The TV's aspect. In 16:9 the game draws an anamorphic 640-wide image.
    const bool wide = SC::current().aspectRatio == SC_ASPECT_RATIO_16x9;
    video.aspectWidth = wide ? 16 : 4;
    video.aspectHeight = wide ? 9 : 3;
    petari_present_set_video(&video);
}

bool imageRect(PetariNative::App::Rect* rect, void*) {
    return petari_present_image_rect(&rect->x, &rect->y, &rect->width, &rect->height) != 0;
}

}  // namespace

extern "C" void petari_present_install(void) {
    // The first seam composes before anything is presented, so the aspect is
    // known for the pointer mapping from the start.
    composeFrame(nullptr);
    petari_present_enable();
    PetariNative::App::setPresentHooks({composeFrame, imageRect, nullptr});
}

// SDK side of XFB presentation: the application's presentation hooks
// (petari/app.hpp), fed from the native VI and SC state. No Aurora headers.

#include <petari/app.hpp>
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
    PetariPresentVideo video{};
    video.xfb = display.frameBuffer;
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

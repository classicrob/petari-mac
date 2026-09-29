#include <dolphin/gx/GXStruct.h>
#include "dolphin/vi/vi_internal.hpp"

extern "C" void petari_backend_configure_vi(const void* mode) {
    aurora::vi::configure(static_cast<const GXRenderModeObj*>(mode));
}

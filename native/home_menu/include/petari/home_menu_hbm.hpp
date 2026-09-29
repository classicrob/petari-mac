#pragma once
// The native menu behind HomeButtonMenuWrapper.cpp's HBM entry points (see
// petari/home_menu.hpp). Needs the SDK headers.

#include "petari/home_menu.hpp"

#include <revolution/hbm.h>

namespace PetariNative::HomeMenu {

// All controllers combined; the Nunchuk stick reads as Up/Down.
FrameInput frameInput(const HBMControllerData* controllers);

namespace Hbm {
void create(const HBMDataInfo* info);
void init();
void calc(const HBMControllerData* controllers);
void draw();
HBMSelectBtnNum getSelectBtnNum();
void setAdjustFlag(int flag);
void startBlackOut();
}  // namespace Hbm

}  // namespace PetariNative::HomeMenu

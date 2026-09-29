// The HBM entry points of HomeButtonMenuWrapper.cpp, served by the native menu.
// Compiled with the SDK headers; home_menu.cpp stays free of them.

#include "petari/home_menu_hbm.hpp"

#include <revolution/wpad.h>

namespace PetariNative::HomeMenu {

static_assert(static_cast<int>(Selection::None) == HBM_SELECT_NULL);
static_assert(static_cast<int>(Selection::Resume) == HBM_SELECT_HOMEBTN);
static_assert(static_cast<int>(Selection::Quit) == HBM_SELECT_BTN1);
static_assert(static_cast<int>(Selection::Restart) == HBM_SELECT_BTN2);
static_assert(Sound::EventBeginExitAnim == HBMSEV_BEGIN_EXIT_ANIM);
static_assert(Sound::EventBeginBlackOut == HBMSEV_BEGIN_BLACKOUT);
static_assert(Sound::EventEndMenu == HBMSEV_END_MENU);
static_assert(Sound::EventPlaySound == HBMSEV_PLAY_SOUND);
static_assert(Sound::HomeButton == HBMSE_HOME_BUTTON);
static_assert(Sound::ReturnApp == HBMSE_RETURN_APP);
static_assert(Sound::GotoMenu == HBMSE_GOTO_MENU);
static_assert(Sound::ResetApp == HBMSE_RESET_APP);
static_assert(Sound::Focus == HBMSE_FOCUS);
static_assert(Sound::Select == HBMSE_SELECT);
static_assert(Sound::Cancel == HBMSE_CANCEL);

namespace {

// A Nunchuk stick past this reads as Up or Down.
constexpr float kStickThreshold = 0.5f;

std::uint32_t gStickHold[WPAD_MAX_CONTROLLERS];

std::uint32_t buttons(u32 wpad) {
    std::uint32_t bits = 0;
    if (wpad & WPAD_BUTTON_UP) {
        bits |= Button::Up;
    }
    if (wpad & WPAD_BUTTON_DOWN) {
        bits |= Button::Down;
    }
    if (wpad & WPAD_BUTTON_A) {
        bits |= Button::A;
    }
    if (wpad & WPAD_BUTTON_B) {
        bits |= Button::B;
    }
    if (wpad & WPAD_BUTTON_PLUS) {
        bits |= Button::Plus;
    }
    if (wpad & WPAD_BUTTON_HOME) {
        bits |= Button::Home;
    }
    return bits;
}

}  // namespace

FrameInput frameInput(const HBMControllerData* controllers) {
    FrameInput input;
    if (controllers == nullptr) {
        return input;
    }
    for (int chan = 0; chan < WPAD_MAX_CONTROLLERS; chan++) {
        const HBMKPadData& pad = controllers->wiiCon[chan];
        const KPADStatus* status = pad.kpad;
        if (status == nullptr) {
            gStickHold[chan] = 0;
            continue;
        }
        input.hold |= buttons(status->hold);
        input.trigger |= buttons(status->trig);

        std::uint32_t stick = 0;
        if (pad.use_devtype == WPAD_DEV_FREESTYLE) {
            const float y = status->ex_status.fs.stick.y;
            if (y > kStickThreshold) {
                stick = Button::Up;
            } else if (y < -kStickThreshold) {
                stick = Button::Down;
            }
        }
        input.hold |= stick;
        input.trigger |= stick & ~gStickHold[chan];
        gStickHold[chan] = stick;

        if (!input.pointerValid && status->dpd_valid_fg > 0) {
            input.pointerValid = true;
            input.pointerX = pad.pos.x;
            input.pointerY = pad.pos.y;
        }
    }
    return input;
}

namespace Hbm {

void create(const HBMDataInfo* info) {
    Config config;
    if (info != nullptr) {
        config.sound = info->sound_callback;
        config.frameDelta = info->frameDelta;
        config.widescreenAdjustX = info->adjust.x;
    }
    instance().create(config);
    publish(View{});
}

void init() {
    for (std::uint32_t& hold : gStickHold) {
        hold = 0;
    }
    instance().open();
}

void calc(const HBMControllerData* controllers) {
    Menu& menu = instance();
    menu.update(frameInput(controllers));
    if (menu.selection() != Selection::None) {
        // HomeButtonLayout hides itself this frame and stops calling draw.
        publish(View{});
    }
}

void draw() {
    publish(instance().view());
}

HBMSelectBtnNum getSelectBtnNum() {
    return static_cast<HBMSelectBtnNum>(static_cast<int>(instance().selection()));
}

void setAdjustFlag(int flag) {
    instance().setWidescreen(flag != 0);
}

void startBlackOut() {
    instance().startBlackOut();
}

}  // namespace Hbm

}  // namespace PetariNative::HomeMenu

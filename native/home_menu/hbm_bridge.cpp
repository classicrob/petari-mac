// The HBM entry points of HomeButtonMenuWrapper.cpp, served by the native menu.
// Compiled with the SDK headers; home_menu.cpp stays free of them.

#include "petari/home_menu_hbm.hpp"
#include "petari/player_launch.hpp"

#include <cstdio>

#include <revolution/wpad.h>

#ifdef PETARI_HOME_MENU_INPUT
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "petari/host_allocation.hpp"
#include "petari/input.hpp"
#include "petari/mods.hpp"
#include "petari/platform/mod_folder.hpp"
#include "petari/camera_settings.hpp"
#endif

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
    if (wpad & WPAD_BUTTON_LEFT) {
        bits |= Button::Left;
    }
    if (wpad & WPAD_BUTTON_RIGHT) {
        bits |= Button::Right;
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
            const float x = status->ex_status.fs.stick.x;
            const float y = status->ex_status.fs.stick.y;
            if (y > kStickThreshold) {
                stick = Button::Up;
            } else if (y < -kStickThreshold) {
                stick = Button::Down;
            } else if (x < -kStickThreshold) {
                stick = Button::Left;
            } else if (x > kStickThreshold) {
                stick = Button::Right;
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

#ifdef PETARI_HOME_MENU_INPUT
// The Controls page, from the bindings in effect now (controls.txt included).
void refreshControls() {
    HostAllocationScope scope;  // HBMInit runs on a game thread
    const std::vector<Input::ControlsLine> summary = Input::controlsSummary(Input::bindings());
    ControlsEntry entries[kMaxControls];
    const int count = std::min(static_cast<int>(summary.size()), kMaxControls);
    for (int i = 0; i < count; i++) {
        std::strncpy(entries[i].action, summary[i].action.c_str(), sizeof(entries[i].action) - 1);
        std::strncpy(entries[i].inputs, summary[i].inputs.c_str(), sizeof(entries[i].inputs) - 1);
        std::strncpy(entries[i].pad, summary[i].pad.c_str(), sizeof(entries[i].pad) - 1);
    }
    instance().setControls(entries, count);
}

// The Mods page toggles, from the mod settings in effect now.
void refreshMods() {
    ModsEntry entries[kMaxMods];
    const int count = std::min(static_cast<int>(Mods::Mod::Count), kMaxMods);
    for (int i = 0; i < count; i++) {
        const auto mod = static_cast<Mods::Mod>(i);
        std::strncpy(entries[i].label, Mods::description(mod), sizeof(entries[i].label) - 1);
        entries[i].on = Mods::enabled(mod);
    }
    instance().setMods(entries, count);
}

// The Odyssey camera page, from the camera settings in effect now.
void refreshCamera() {
    CameraOptions options;
    options.on = CameraSettings::enabled();
    options.speed = CameraSettings::speed();
    options.invertX = CameraSettings::invertX();
    options.invertY = CameraSettings::invertY();
    instance().setCamera(options);
}

// Applies and saves changes made on the Odyssey camera page this frame.
void applyCameraChange() {
    CameraOptions options;
    if (!instance().takeCameraChange(&options)) {
        return;
    }
    CameraSettings::setEnabled(options.on);
    CameraSettings::setSpeed(options.speed);
    CameraSettings::setInvertX(options.invertX);
    CameraSettings::setInvertY(options.invertY);
    std::string error;
    if (!CameraSettings::save(&error)) {
        std::fprintf(stderr, "petari: camera: %s\n", error.c_str());
    }
}

// The Mod folder page: the disc-file mods found, with their saved states.
void refreshFolderMods() {
    FolderModEntry entries[kMaxFolderMods];
    const auto mods = Platform::ModFolder::detected();
    const auto states = Platform::ModFolder::detectedStates();
    const int count = std::min(static_cast<int>(mods.size()), kMaxFolderMods);
    for (int i = 0; i < count; i++) {
        const auto& mod = mods[i];
        std::snprintf(entries[i].label, sizeof(entries[i].label), "%s (%u files)", mod.title.c_str(), mod.fileCount);
        const auto state = states.find(mod.name);
        entries[i].on = state != states.end() && state->second;
    }
    instance().setFolderMods(entries, count);
}

// Saves the Mod folder toggles made this frame to mods.txt.
void applyFolderToggles() {
    const std::uint64_t toggles = instance().takeFolderToggles();
    if (toggles == 0) {
        return;
    }
    const auto mods = Platform::ModFolder::detected();
    for (int i = 0; i < static_cast<int>(mods.size()) && i < kMaxFolderMods; i++) {
        if (toggles & (std::uint64_t{1} << i)) {
            std::string error;
            if (!Platform::ModFolder::setEnabled(mods[i].name, instance().folderMods()[i].on, &error)) {
                std::fprintf(stderr, "petari: mod folder: %s\n", error.c_str());
            }
        }
    }
}

// Applies and saves toggles made on the Mods page this frame.
void applyModToggles() {
    const std::uint32_t toggles = instance().takeModToggles();
    if (toggles == 0) {
        return;
    }
    for (int i = 0; i < static_cast<int>(Mods::Mod::Count) && i < kMaxMods; i++) {
        if (toggles & (1u << i)) {
            Mods::setEnabled(static_cast<Mods::Mod>(i), instance().mods()[i].on);
        }
    }
    std::string error;
    if (!Mods::save(&error)) {
        std::fprintf(stderr, "petari: mods: %s\n", error.c_str());
    }
}
#endif

void init() {
    for (std::uint32_t& hold : gStickHold) {
        hold = 0;
    }
#ifdef PETARI_HOME_MENU_INPUT
    refreshControls();
    refreshMods();
    refreshFolderMods();
    refreshCamera();
#endif
    instance().open();
}

void calc(const HBMControllerData* controllers) {
    Menu& menu = instance();
    menu.update(frameInput(controllers));
#ifdef PETARI_HOME_MENU_INPUT
    applyModToggles();
    applyFolderToggles();
    applyCameraChange();
#endif
    // Level Select's Go, once the menu has closed: the game starts the warp
    // at its next normal gameplay frame (GameScene::update).
    const char* stage = nullptr;
    int mission = 0;
    if (menu.takeLevelRequest(&stage, &mission)) {
        std::snprintf(PlayerLaunch::warpStage, sizeof(PlayerLaunch::warpStage), "%s", stage);
        PlayerLaunch::warpScenario = mission;
        PlayerLaunch::warpPending.store(true);
    }
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

// Tests for the native HOME Button Menu replacement: the menu's lifecycle,
// selections, cancellation, black-out, and focus/repeat rules; the HBM bridge
// reading the game's KPAD data; the game's own HomeButtonMenuWrapper.cpp
// calling the menu through RSO::HBM*; and the Dear ImGui overlay's output.

#include <revolution/hbm.h>
#include <revolution/kpad.h>
#include <revolution/wpad.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Game/System/HomeButtonMenuWrapper.hpp"
#include "petari/home_menu.hpp"
#include "petari/home_menu_hbm.hpp"

#ifdef PETARI_HOME_MENU_TEST_IMGUI
#include <imgui.h>
#endif

namespace HM = PetariNative::HomeMenu;
using HM::Button::A;
using HM::Button::B;
using HM::Button::Down;
using HM::Button::Home;
using HM::Button::Plus;
using HM::Button::Up;

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

struct SoundEvent {
    int event;
    int num;
};
std::vector<SoundEvent> sounds;

int recordSound(int event, int num) {
    sounds.push_back({event, num});
    return event == HM::Sound::EventPlaySound ? 1 : 0;
}

bool played(int num) {
    for (const SoundEvent& s : sounds) {
        if (s.event == HM::Sound::EventPlaySound && s.num == num) {
            return true;
        }
    }
    return false;
}

bool sent(int event) {
    for (const SoundEvent& s : sounds) {
        if (s.event == event) {
            return true;
        }
    }
    return false;
}

int playedCount(int num) {
    int count = 0;
    for (const SoundEvent& s : sounds) {
        if (s.event == HM::Sound::EventPlaySound && s.num == num) {
            count++;
        }
    }
    return count;
}

HM::FrameInput none() {
    return {};
}

HM::FrameInput press(std::uint32_t buttons) {
    HM::FrameInput input;
    input.hold = buttons;
    input.trigger = buttons;
    return input;
}

HM::FrameInput hold(std::uint32_t buttons) {
    HM::FrameInput input;
    input.hold = buttons;
    return input;
}

HM::FrameInput pointer(float x, float y, std::uint32_t trigger = 0) {
    HM::FrameInput input;
    input.pointerValid = true;
    input.pointerX = x;
    input.pointerY = y;
    input.hold = trigger;
    input.trigger = trigger;
    return input;
}

void center(const HM::Rect& r, float* x, float* y) {
    *x = (r.x0 + r.x1) * 0.5f;
    *y = (r.y0 + r.y1) * 0.5f;
}

HM::Menu newMenu(float frameDelta = 1.0f) {
    HM::Menu menu;
    HM::Config config;
    config.sound = recordSound;
    config.frameDelta = frameDelta;
    menu.create(config);
    sounds.clear();
    return menu;
}

// Opens the menu and runs the opening fade.
void openToList(HM::Menu& menu) {
    menu.open();
    for (int i = 0; i < 10; i++) {
        menu.update(none());
    }
    check(menu.phase() == HM::Phase::List, "opening fade takes 10 frames");
}

int framesUntilSelection(HM::Menu& menu, int limit = 100) {
    for (int i = 1; i <= limit; i++) {
        menu.update(none());
        if (menu.selection() != HM::Selection::None) {
            return i;
        }
    }
    return -1;
}

void testLifecycle() {
    HM::Menu menu = newMenu();
    check(menu.selection() == HM::Selection::None, "no selection before opening");
    check(!menu.view().visible, "hidden before opening");
    menu.update(press(A));
    check(menu.phase() == HM::Phase::Closed, "update before opening does nothing");

    menu.open();
    check(menu.phase() == HM::Phase::Opening, "open starts the fade in");
    check(played(HM::Sound::HomeButton), "open plays the HOME button sound");
    check(menu.view().visible && menu.view().panelOpacity == 0.0f, "visible at zero opacity on the first frame");
    menu.update(press(A));
    check(menu.phase() == HM::Phase::Opening && menu.focus() == 0, "input ignored while opening");
    for (int i = 0; i < 4; i++) {
        menu.update(none());
    }
    check(std::fabs(menu.view().panelOpacity - 0.5f) < 1e-5f, "panel half faded in after 5 frames");
    check(std::fabs(menu.view().dimOpacity - 0.25f) < 1e-5f, "dim follows the panel");
    for (int i = 0; i < 5; i++) {
        menu.update(none());
    }
    check(menu.phase() == HM::Phase::List, "list after the fade in");
    const HM::View list = menu.view();
    check(list.itemCount == 3 && std::strcmp(list.items[0].label, "Resume") == 0 &&
              std::strcmp(list.items[1].label, "Restart from Title") == 0 && std::strcmp(list.items[2].label, "Quit") == 0,
          "list offers Resume, Restart from Title, Quit");
    check(list.items[0].focused && !list.items[1].focused, "Resume focused first");

    for (int i = 0; i < 300; i++) {
        menu.update(none());
    }
    check(menu.selection() == HM::Selection::None, "menu waits for the player");

    sounds.clear();
    menu.update(press(A));
    check(menu.phase() == HM::Phase::Closing, "Resume closes");
    check(played(HM::Sound::ReturnApp) && sent(HM::Sound::EventBeginExitAnim), "resume sound and exit event");
    check(framesUntilSelection(menu) == 10, "resume reported after the 10-frame fade out");
    check(menu.selection() == HM::Selection::Resume, "resume result");
    check(sent(HM::Sound::EventEndMenu), "end-of-menu event");
    check(!menu.view().visible, "hidden once finished");
    menu.update(press(A));
    check(menu.selection() == HM::Selection::Resume, "result stable until reopened");

    menu.open();
    check(menu.selection() == HM::Selection::None && menu.phase() == HM::Phase::Opening && menu.focus() == 0,
          "reopening starts fresh");
}

void testSelections() {
    for (int quit = 0; quit < 2; quit++) {
        HM::Menu menu = newMenu();
        openToList(menu);
        menu.update(press(Down));
        if (quit) {
            menu.update(press(Down));
        }
        sounds.clear();
        menu.update(press(A));
        check(menu.phase() == HM::Phase::Confirm, "Restart and Quit ask first");
        check(played(HM::Sound::Select), "select sound");
        check(menu.confirming() == (quit ? HM::Selection::Quit : HM::Selection::Restart), "confirming the chosen action");
        const HM::View confirm = menu.view();
        check(confirm.itemCount == 2 && std::strcmp(confirm.items[1].label, "Cancel") == 0, "confirm offers Cancel");
        check(std::strcmp(confirm.items[0].label, quit ? "Quit" : "Restart") == 0, "confirm names the action");
        check(menu.focus() == 1, "Cancel focused by default");
        menu.update(press(A));
        check(menu.phase() == HM::Phase::List && menu.focus() == (quit ? 2 : 1), "A on Cancel returns to the item");

        menu.update(press(A));
        menu.update(press(Up));
        check(menu.focus() == 0, "Up to the action");
        sounds.clear();
        menu.update(press(A));
        check(menu.phase() == HM::Phase::BlackOut, "accepting fades to black");
        check(played(quit ? HM::Sound::GotoMenu : HM::Sound::ResetApp), "leave sound");
        check(sent(HM::Sound::EventBeginBlackOut), "black-out event");
        for (int i = 0; i < 15; i++) {
            menu.update(none());
        }
        check(std::fabs(menu.view().blackOpacity - 0.5f) < 1e-5f, "half black after 15 frames");
        check(menu.view().panelOpacity == 1.0f, "panel stays under the black");
        check(menu.selection() == HM::Selection::None, "no result before full black");
        check(framesUntilSelection(menu) == 15, "result at full black after 30 frames");
        check(menu.selection() == (quit ? HM::Selection::Quit : HM::Selection::Restart), "leave result");
    }
}

void testCancellation() {
    HM::Menu menu = newMenu();
    openToList(menu);
    menu.update(press(Plus | B));  // Escape
    check(menu.phase() == HM::Phase::Closing, "Escape in the list resumes");
    check(framesUntilSelection(menu) == 10 && menu.selection() == HM::Selection::Resume, "Escape resume result");

    openToList(menu);
    menu.update(press(Down));
    menu.update(press(A));
    sounds.clear();
    menu.update(press(Plus | B));
    check(menu.phase() == HM::Phase::List && menu.focus() == 1, "Escape in a confirmation goes back");
    check(played(HM::Sound::Cancel), "cancel sound");

    menu.update(press(A));
    sounds.clear();
    menu.update(press(Home));
    check(menu.phase() == HM::Phase::Closing, "Home closes from a confirmation");
    check(played(HM::Sound::ReturnApp), "Home plays the return sound");
    check(framesUntilSelection(menu) == 10 && menu.selection() == HM::Selection::Resume, "Home resumes");

    openToList(menu);
    menu.update(press(B));
    check(menu.phase() == HM::Phase::Closing, "B without the pointer on an item is back");

    openToList(menu);
    menu.update(pointer(0.95f, 0.95f, B));
    check(menu.phase() == HM::Phase::Closing, "a left click outside the items is back");

    openToList(menu);
    menu.update(press(Down));
    menu.update(press(A));
    menu.update(pointer(0.95f, 0.95f, B));
    check(menu.phase() == HM::Phase::List, "a left click outside a confirmation cancels it");

    // Held Home from the frame that opened the menu does not close it.
    openToList(menu);
    menu.update(hold(Home));
    check(menu.phase() == HM::Phase::List, "only a new Home press closes");
    // Input during the closing fade is ignored.
    menu.update(press(Home));
    menu.update(press(Down));
    menu.update(press(A));
    check(menu.phase() == HM::Phase::Closing, "input ignored while closing");
}

void testBlackOut() {
    HM::Menu menu = newMenu();
    menu.startBlackOut();
    check(menu.phase() == HM::Phase::Closed && menu.selection() == HM::Selection::None,
          "black-out while never opened does nothing");

    openToList(menu);
    menu.update(press(A));
    framesUntilSelection(menu);
    menu.startBlackOut();
    check(menu.selection() == HM::Selection::Resume, "black-out after the menu closed does nothing");

    for (HM::Phase at : {HM::Phase::Opening, HM::Phase::List, HM::Phase::Confirm, HM::Phase::Closing}) {
        sounds.clear();
        menu.open();
        if (at != HM::Phase::Opening) {
            for (int i = 0; i < 10; i++) {
                menu.update(none());
            }
        }
        if (at == HM::Phase::Confirm) {
            menu.update(press(Down));
            menu.update(press(A));
        } else if (at == HM::Phase::Closing) {
            menu.update(press(A));
        }
        check(menu.phase() == at, "reached the phase to interrupt");
        menu.startBlackOut();
        check(menu.phase() == HM::Phase::BlackOut, "console reset starts the black-out");
        check(sent(HM::Sound::EventBeginBlackOut), "black-out event on reset");
        menu.update(press(Home));
        menu.update(press(Plus));
        check(menu.phase() == HM::Phase::BlackOut, "no input during the black-out");
        check(framesUntilSelection(menu) == 28, "black-out completes 30 frames after it began");
        check(menu.selection() == HM::Selection::Restart, "black-out reports the reset result");
    }

    // A reset during a Quit fade keeps its timing and becomes the reset result.
    openToList(menu);
    menu.update(press(Down));
    menu.update(press(Down));
    menu.update(press(A));
    menu.update(press(Up));
    menu.update(press(A));
    for (int i = 0; i < 20; i++) {
        menu.update(none());
    }
    const float black = menu.view().blackOpacity;
    menu.startBlackOut();
    check(menu.view().blackOpacity == black, "black-out continues from the current opacity");
    check(framesUntilSelection(menu) == 10, "black-out timing unchanged");
    check(menu.selection() == HM::Selection::Restart, "reset replaces the quit result");
}

void testFocusAndRepeat() {
    HM::Menu menu = newMenu();
    openToList(menu);
    sounds.clear();
    menu.update(press(Up));
    check(menu.focus() == 0 && sounds.empty(), "focus stops at the top, silently");
    menu.update(press(Down));
    check(menu.focus() == 1 && playedCount(HM::Sound::Focus) == 1, "Down moves the focus with a sound");
    menu.update(hold(Down));
    menu.update(press(Up | Down));
    check(menu.focus() == 1, "Up and Down together do nothing");

    // Repeat: the press moves once, holding repeats after 24 frames, then
    // every 6. A fresh list to have room to move.
    openToList(menu);
    menu.update(press(Down));
    check(menu.focus() == 1, "first move on the press");
    for (int i = 0; i < 23; i++) {
        menu.update(hold(Down));
    }
    check(menu.focus() == 1, "no repeat before 24 frames");
    menu.update(hold(Down));
    check(menu.focus() == 2, "repeat at 24 frames");
    menu.update(hold(0));
    menu.update(press(Up));
    for (int i = 0; i < 24; i++) {
        menu.update(hold(Up));
    }
    check(menu.focus() == 0, "repeat upward");
    // Repeat interval, on the three-item list: Down press at 0, hold to 24 (1 -> 2 is the press+repeat).
    openToList(menu);
    menu.update(press(Down));
    for (int i = 0; i < 24; i++) {
        menu.update(hold(Down));
    }
    check(menu.focus() == 2, "press + first repeat");
    sounds.clear();
    for (int i = 0; i < 12; i++) {
        menu.update(hold(Down));
    }
    check(menu.focus() == 2 && sounds.empty(), "repeats stop at the end without sound");

    // Interval, counted on the Up side: from item 2, press Up at 0 -> 1, repeat at 24 -> 0.
    openToList(menu);
    menu.update(press(Down));
    menu.update(press(Down));
    menu.update(press(Up));
    for (int i = 0; i < 23; i++) {
        menu.update(hold(Up));
    }
    check(menu.focus() == 1, "Up repeat not yet");
    menu.update(hold(Up));
    check(menu.focus() == 0, "Up repeat at 24");

    // A key held into the menu does not start repeating.
    openToList(menu);
    for (int i = 0; i < 60; i++) {
        menu.update(hold(Down));
    }
    check(menu.focus() == 0, "no repeat without a press in the menu");

    // Frame delta scales timing.
    HM::Menu fast = newMenu(2.0f);
    fast.open();
    for (int i = 0; i < 5; i++) {
        fast.update(none());
    }
    check(fast.phase() == HM::Phase::List, "opening at frameDelta 2 takes 5 updates");
    fast.update(press(Down));
    for (int i = 0; i < 12; i++) {
        fast.update(hold(Down));
    }
    check(fast.focus() == 2, "repeat at frameDelta 2 after 12 updates");

    // Pointer focus: moving onto an item focuses it; a resting pointer does not
    // override the keyboard; A activates the focus; B activates the item under
    // the pointer.
    openToList(menu);
    const HM::View view = menu.view();
    float x, y;
    center(view.items[2].rect, &x, &y);
    sounds.clear();
    menu.update(pointer(x, y));
    check(menu.focus() == 2 && playedCount(HM::Sound::Focus) == 1, "pointer moving onto Quit focuses it");
    menu.update(pointer(x, y));
    check(playedCount(HM::Sound::Focus) == 1, "no sound while resting");
    HM::FrameInput up = pointer(x, y, Up);
    menu.update(up);
    check(menu.focus() == 1, "keyboard moves the focus under a resting pointer");
    menu.update(pointer(x, y));
    check(menu.focus() == 1, "resting pointer does not steal the focus back");
    menu.update(pointer(x + 0.01f, y));
    check(menu.focus() == 2, "moving the pointer takes the focus");
    menu.update(pointer(0.95f, 0.95f));
    check(menu.focus() == 2, "leaving the items keeps the focus");
    menu.update(pointer(x, y, B));
    check(menu.phase() == HM::Phase::Confirm && menu.confirming() == HM::Selection::Quit, "left click on Quit");

    openToList(menu);
    center(menu.view().items[1].rect, &x, &y);
    menu.update(pointer(x, y));
    menu.update(pointer(0.95f, 0.95f));
    menu.update(press(A));
    check(menu.phase() == HM::Phase::Confirm && menu.confirming() == HM::Selection::Restart, "A activates the focus");

    // The pointer resting where it was during the opening fade does not focus.
    menu = newMenu();
    menu.open();
    center(menu.view().items[2].rect, &x, &y);
    for (int i = 0; i < 10; i++) {
        menu.update(pointer(x, y));
    }
    menu.update(pointer(x, y));
    check(menu.phase() == HM::Phase::List && menu.focus() == 0, "resting pointer from before the list does not focus");

    // Widescreen keeps the panel's shape.
    menu = newMenu();
    openToList(menu);
    const float narrow = menu.view().panel.x1;
    menu.setWidescreen(true);
    check(std::fabs(menu.view().panel.x1 - narrow * 608.0f / 832.0f) < 1e-5f, "16:9 narrows the panel in pointer space");
    center(menu.view().items[0].rect, &x, &y);
    check(menu.view().items[0].rect.x1 < narrow, "items narrow too");
}

void setPad(KPADStatus* status, u32 holdBits, u32 trigBits) {
    std::memset(status, 0, sizeof(*status));
    status->hold = holdBits;
    status->trig = trigBits;
}

void testBridgeInput() {
    HBMControllerData data;
    KPADStatus pads[WPAD_MAX_CONTROLLERS];
    for (int i = 0; i < WPAD_MAX_CONTROLLERS; i++) {
        data.wiiCon[i].kpad = nullptr;
        data.wiiCon[i].use_devtype = WPAD_DEV_CORE;
        data.wiiCon[i].pos.x = data.wiiCon[i].pos.y = 0.0f;
        setPad(&pads[i], 0, 0);
    }
    HM::FrameInput input = HM::frameInput(&data);
    check(input.hold == 0 && input.trigger == 0 && !input.pointerValid, "no controllers, no input");
    check(HM::frameInput(nullptr).hold == 0, "null controller data");

    data.wiiCon[0].kpad = &pads[0];
    setPad(&pads[0], WPAD_BUTTON_A | WPAD_BUTTON_PLUS | WPAD_BUTTON_1, WPAD_BUTTON_A);
    data.wiiCon[1].kpad = &pads[1];
    setPad(&pads[1], WPAD_BUTTON_HOME | WPAD_BUTTON_B | WPAD_BUTTON_UP | WPAD_BUTTON_DOWN, WPAD_BUTTON_HOME);
    input = HM::frameInput(&data);
    check(input.hold == (A | Plus | Home | B | Up | Down), "buttons of every controller combined");
    check(input.trigger == (A | Home), "triggers combined");

    // Pointer from the first controller aiming at the screen.
    pads[0].dpd_valid_fg = 0;
    pads[1].dpd_valid_fg = 2;
    data.wiiCon[0].pos = {0.9f, 0.9f};
    data.wiiCon[1].pos = {-0.25f, 0.5f};
    input = HM::frameInput(&data);
    check(input.pointerValid && input.pointerX == -0.25f && input.pointerY == 0.5f, "pointer from a valid controller");

    // Nunchuk stick: W/S (up is positive) with an edge per channel.
    data.wiiCon[1].kpad = nullptr;
    setPad(&pads[0], 0, 0);
    data.wiiCon[0].use_devtype = WPAD_DEV_FREESTYLE;
    pads[0].ex_status.fs.stick.y = 1.0f;
    input = HM::frameInput(&data);
    check(input.hold == Up && input.trigger == Up, "stick up is a press");
    input = HM::frameInput(&data);
    check(input.hold == Up && input.trigger == 0, "stick held is a hold");
    pads[0].ex_status.fs.stick.y = -1.0f;
    input = HM::frameInput(&data);
    check(input.hold == Down && input.trigger == Down, "stick down");
    pads[0].ex_status.fs.stick.y = 0.3f;
    input = HM::frameInput(&data);
    check(input.hold == 0, "stick inside the dead zone");
    data.wiiCon[0].use_devtype = WPAD_DEV_CORE;
    pads[0].ex_status.fs.stick.y = 1.0f;
    check(HM::frameInput(&data).hold == 0, "no stick without a Nunchuk");
}

// The game's wrapper, driven as HomeButtonLayout drives it.
void testWrapper() {
    RSO::setupRsoHomeButtonMenu();

    HBMDataInfo info;
    std::memset(&info, 0, sizeof(info));
    info.sound_callback = recordSound;
    info.frameDelta = 1.0f;
    info.adjust.x = 832.0f / 608.0f;
    info.adjust.y = 1.0f;
    RSO::HBMCreate(&info);
    RSO::HBMSetAdjustFlag(1);
    check(!HM::publishedView().visible, "nothing published after create");
    check(RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL, "no selection after create");
    RSO::HBMStartBlackOut();  // forceToDeactive with the menu closed
    check(RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL, "black-out while closed is ignored");

    HBMControllerData data;
    KPADStatus pad;
    for (int i = 0; i < WPAD_MAX_CONTROLLERS; i++) {
        data.wiiCon[i].kpad = nullptr;
        data.wiiCon[i].use_devtype = WPAD_DEV_FREESTYLE;
        data.wiiCon[i].pos.x = data.wiiCon[i].pos.y = 0.0f;
    }
    data.wiiCon[0].kpad = &pad;

    // exeActive's first step, then one HBMCalc per frame; draw() in between.
    sounds.clear();
    RSO::HBMInit();
    RSO::HBMSetAdjustFlag(1);
    check(played(HM::Sound::HomeButton), "HBMInit plays through the game's callback");
    int frames = 0;
    auto frame = [&](u32 holdBits, u32 trigBits) {
        setPad(&pad, holdBits, trigBits);
        RSO::HBMDraw();
        RSO::HBMCalc(&data);
        frames++;
    };
    frame(WPAD_BUTTON_HOME, 0);
    check(HM::publishedView().visible, "HBMDraw publishes the view");
    check(HM::publishedView().panel.x1 < 0.42f, "adjust flag applied");
    for (int i = 0; i < 12; i++) {
        frame(0, 0);
    }
    frame(WPAD_BUTTON_DOWN, WPAD_BUTTON_DOWN);
    frame(WPAD_BUTTON_A, WPAD_BUTTON_A);
    check(HM::instance().phase() == HM::Phase::Confirm, "Restart chosen through RSO::HBMCalc");
    frame(WPAD_BUTTON_UP, WPAD_BUTTON_UP);
    frame(WPAD_BUTTON_A, WPAD_BUTTON_A);
    while (RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL && frames < 200) {
        frame(0, 0);
        if (RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL) {
            check(HM::publishedView().visible, "view published while running");
        }
    }
    check(RSO::HBMGetSelectBtnNum() == HBM_SELECT_BTN2, "Restart reports HBM_SELECT_BTN2");
    check(!HM::publishedView().visible, "view hidden once a selection is reported");

    // Console reset while open: the black-out reports BTN2.
    RSO::HBMInit();
    frame(0, 0);
    RSO::HBMStartBlackOut();
    frames = 0;
    while (RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL && frames < 200) {
        frame(0, 0);
    }
    check(RSO::HBMGetSelectBtnNum() == HBM_SELECT_BTN2 && frames == 30, "forced black-out reports BTN2");

    // Quit reports BTN1; Home reports HOMEBTN.
    RSO::HBMInit();
    for (int i = 0; i < 10; i++) {
        frame(0, 0);
    }
    frame(WPAD_BUTTON_DOWN, WPAD_BUTTON_DOWN);
    frame(WPAD_BUTTON_DOWN, WPAD_BUTTON_DOWN);
    frame(WPAD_BUTTON_A, WPAD_BUTTON_A);
    frame(WPAD_BUTTON_UP, WPAD_BUTTON_UP);
    frame(WPAD_BUTTON_A, WPAD_BUTTON_A);
    frames = 0;
    while (RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL && frames < 200) {
        frame(0, 0);
    }
    check(RSO::HBMGetSelectBtnNum() == HBM_SELECT_BTN1, "Quit reports HBM_SELECT_BTN1");

    RSO::HBMInit();
    for (int i = 0; i < 10; i++) {
        frame(0, 0);
    }
    frame(WPAD_BUTTON_HOME, WPAD_BUTTON_HOME);
    frames = 0;
    while (RSO::HBMGetSelectBtnNum() == HBM_SELECT_NULL && frames < 200) {
        frame(0, 0);
    }
    check(RSO::HBMGetSelectBtnNum() == HBM_SELECT_HOMEBTN && frames == 10, "Home reports HBM_SELECT_HOMEBTN");
}

#ifdef PETARI_HOME_MENU_TEST_IMGUI
struct Bounds {
    int vertices = 0;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    float blackAlpha = 0.0f;  // most opaque pure-black vertex
    bool focusColor = false;
};

Bounds drawOverlay(float x, float y, float w, float h) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    HM::drawImGuiOverlay(x, y, w, h);
    ImGui::Render();
    Bounds bounds;
    const ImDrawData* data = ImGui::GetDrawData();
    for (int i = 0; i < data->CmdListsCount; i++) {
        const ImDrawList* list = data->CmdLists[i];
        for (const ImDrawVert& v : list->VtxBuffer) {
            bounds.vertices++;
            bounds.x0 = std::fmin(bounds.x0, v.pos.x);
            bounds.y0 = std::fmin(bounds.y0, v.pos.y);
            bounds.x1 = std::fmax(bounds.x1, v.pos.x);
            bounds.y1 = std::fmax(bounds.y1, v.pos.y);
            const ImVec4 c = ImGui::ColorConvertU32ToFloat4(v.col);
            if (c.x == 0.0f && c.y == 0.0f && c.z == 0.0f) {
                bounds.blackAlpha = std::fmax(bounds.blackAlpha, c.w);
            }
            if (c.w > 0.99f && c.x > 0.9f && c.y > 0.7f && c.z < 0.3f) {
                bounds.focusColor = true;
            }
        }
    }
    return bounds;
}

void testImGuiOverlay() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    HM::publish(HM::View{});
    check(drawOverlay(160.0f, 40.0f, 960.0f, 720.0f).vertices == 0, "hidden menu draws nothing");

    HM::Menu menu = newMenu();
    openToList(menu);
    HM::publish(menu.view());
    const Bounds list = drawOverlay(160.0f, 40.0f, 960.0f, 720.0f);
    check(list.vertices > 100, "list draws the dim, panel, items, and text");
    check(list.x0 >= 160.0f - 0.5f && list.x1 <= 1120.0f + 0.5f && list.y0 >= 40.0f - 0.5f && list.y1 <= 760.0f + 0.5f,
          "drawing stays inside the game image");
    check(list.x0 < 161.0f && list.y1 > 759.0f, "dim covers the image");
    check(list.focusColor, "focused item highlighted");
    check(std::fabs(list.blackAlpha - 0.5f) < 0.01f, "only the half dim is black while the list is shown");
    check(!ImGui::GetIO().WantCaptureMouse && !ImGui::GetIO().WantCaptureKeyboard, "overlay takes no input");

    // Every confirmation's text fits the panel, in 4:3 and 16:9 images.
    for (int quit = 0; quit < 2; quit++) {
        for (int wide = 0; wide < 2; wide++) {
            HM::Menu confirm = newMenu();
            confirm.setWidescreen(wide != 0);
            openToList(confirm);
            confirm.update(press(Down));
            if (quit) {
                confirm.update(press(Down));
            }
            confirm.update(press(A));
            HM::View view = confirm.view();
            view.dimOpacity = 0.0f;  // leave only the panel and its contents
            HM::publish(view);
            const float w = wide ? 1280.0f : 960.0f;
            const float x = (1280.0f - w) * 0.5f;
            const Bounds panel = drawOverlay(x, 40.0f, w, 720.0f);
            const float px0 = x + (view.panel.x0 + 1.0f) * 0.5f * w;
            const float px1 = x + (view.panel.x1 + 1.0f) * 0.5f * w;
            // Allow for the border stroke (half its width) and the antialiasing fringe.
            const float stroke = 720.0f * 0.003f * 0.5f + 1.0f;
            check(panel.vertices > 0 && panel.x0 >= px0 - stroke && panel.x1 <= px1 + stroke,
                  "confirmation text fits the panel");
        }
    }

    menu.update(press(Down));
    menu.update(press(A));
    menu.update(press(Up));
    menu.update(press(A));
    for (int i = 0; i < 30; i++) {
        menu.update(none());
    }
    check(menu.selection() == HM::Selection::Restart, "black-out finished");
    menu.open();
    for (int i = 0; i < 10; i++) {
        menu.update(none());
    }
    menu.startBlackOut();
    for (int i = 0; i < 29; i++) {
        menu.update(none());
    }
    const float black = menu.view().blackOpacity;
    check(black > 0.95f && black < 1.0f, "nearly black one frame before the result");
    HM::publish(menu.view());
    const Bounds blackOut = drawOverlay(0.0f, 0.0f, 1280.0f, 800.0f);
    check(std::fabs(blackOut.blackAlpha - black) < 0.01f, "black-out draws its opacity over the image");
    check(blackOut.x0 <= 0.0f && blackOut.y1 >= 800.0f, "black-out covers the image");

    ImGui::DestroyContext();
}
#endif

}  // namespace

int main() {
    testLifecycle();
    testSelections();
    testCancellation();
    testBlackOut();
    testFocusAndRepeat();
    testBridgeInput();
    testWrapper();
#ifdef PETARI_HOME_MENU_TEST_IMGUI
    testImGuiOverlay();
#endif
    std::printf("native home menu tests passed (%d checks)\n", checks);
    return 0;
}

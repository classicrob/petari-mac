#include "petari/home_menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace PetariNative::HomeMenu {

namespace {

constexpr float kDimOpacity = 0.5f;

// Layout in KPAD space before the widescreen correction.
constexpr float kPanelHalfWidth = 0.5f;
constexpr float kPanelTop = -0.66f;
constexpr float kPanelBottomMargin = 0.06f;  // below the last item
constexpr float kItemHalfWidth = 0.44f;
constexpr float kItemTop = -0.3f;
constexpr float kItemHeight = 0.19f;
constexpr float kItemSpacing = 0.24f;

// The Controls page: a larger panel, the lines, and Back at the bottom.
constexpr float kControlsHalfWidth = 0.86f;
constexpr float kControlsTop = -0.92f;
constexpr float kControlsTitleY = -0.82f;
constexpr float kControlsMessageY = -0.73f;
constexpr float kControlsLinesTop = -0.66f;
constexpr float kControlsLinesBottom = 0.62f;
constexpr float kControlsLinesHalfWidth = 0.8f;
constexpr float kControlsBackTop = 0.68f;
constexpr float kControlsBackHeight = 0.15f;
constexpr float kControlsBackHalfWidth = 0.2f;

constexpr const char* kListLabels[] = {"Resume", "Controls", "Mods", "Restart from Title", "Quit"};
constexpr int kListCount = 5;
constexpr int kListResume = 0;
constexpr int kListControls = 1;
constexpr int kListMods = 2;
constexpr int kListRestart = 3;
constexpr int kListQuit = 4;
constexpr int kConfirmAccept = 0;
constexpr int kConfirmCancel = 1;

float progress(float time, float length) {
    return std::clamp(time / length, 0.0f, 1.0f);
}

}  // namespace

void Menu::create(const Config& config) {
    mConfig = config;
    if (!(mConfig.frameDelta > 0.0f)) {
        mConfig.frameDelta = 1.0f;
    }
    if (!(mConfig.widescreenAdjustX > 0.0f)) {
        mConfig.widescreenAdjustX = 1.0f;
    }
    mCreated = true;
    mPhase = Phase::Closed;
    mResult = Selection::None;
}

void Menu::setWidescreen(bool widescreen) {
    mWidescreen = widescreen;
}

void Menu::open() {
    if (!mCreated) {
        return;
    }
    mPhase = Phase::Opening;
    mResult = Selection::None;
    mConfirming = Selection::None;
    mFocus = kListResume;
    mTime = 0.0f;
    mBlackStart = 0.0f;
    mPanelStart = 0.0f;
    mRepeatDir = 0;
    mRepeatTime = 0.0f;
    mPointerWasValid = false;
    play(Sound::HomeButton);
}

void Menu::update(const FrameInput& input) {
    const float dt = mConfig.frameDelta;
    const bool pointerMoved = input.pointerValid &&
                              (!mPointerWasValid || std::fabs(input.pointerX - mPointerX) > 1e-4f ||
                               std::fabs(input.pointerY - mPointerY) > 1e-4f);
    mPointerWasValid = input.pointerValid;
    if (input.pointerValid) {
        mPointerX = input.pointerX;
        mPointerY = input.pointerY;
    }

    switch (mPhase) {
    case Phase::Closed:
    case Phase::Finished:
        return;
    case Phase::Opening:
        mTime += dt;
        if (mTime >= kFadeFrames) {
            mPhase = Phase::List;
            mTime = 0.0f;
        }
        return;
    case Phase::Closing:
        mTime += dt;
        if (mTime >= kFadeFrames) {
            finish();
        }
        return;
    case Phase::BlackOut:
        mTime += dt;
        if (mTime >= kBlackOutFrames) {
            finish();
        }
        return;
    case Phase::List:
    case Phase::Confirm:
    case Phase::Controls:
    case Phase::Mods:
        break;
    }

    const int hovered = input.pointerValid ? itemAt(input.pointerX, input.pointerY) : -1;

    if (input.trigger & Button::Home) {
        play(Sound::ReturnApp);
        close();
        return;
    }
    // Escape presses Plus and B together; Plus alone decides that it is a back.
    if (input.trigger & Button::Plus) {
        back();
        return;
    }
    if (input.trigger & Button::A) {
        activate(mFocus);
        return;
    }
    if (input.trigger & Button::B) {
        if (hovered >= 0) {
            setFocus(hovered, false);
            activate(hovered);
        } else {
            back();
        }
        return;
    }

    if (pointerMoved && hovered >= 0) {
        setFocus(hovered, true);
    }

    std::uint32_t dir = input.hold & (Button::Up | Button::Down);
    if (dir == (Button::Up | Button::Down)) {
        dir = 0;
    }
    if (dir != 0 && (input.trigger & dir)) {
        mRepeatDir = dir;
        mRepeatTime = 0.0f;
        moveFocus(dir == Button::Up ? -1 : 1);
    } else if (dir != 0 && dir == mRepeatDir) {
        mRepeatTime += dt;
        while (mRepeatTime >= kRepeatDelayFrames) {
            moveFocus(dir == Button::Up ? -1 : 1);
            mRepeatTime -= kRepeatIntervalFrames;
        }
    } else {
        // Held since before the menu took it (or released): no repeat
        // without a fresh press.
        mRepeatDir = 0;
        mRepeatTime = 0.0f;
    }
}

void Menu::setControls(const ControlsEntry* entries, int count) {
    mControlCount = std::clamp(count, 0, kMaxControls);
    for (int i = 0; i < mControlCount; i++) {
        mControls[i] = entries[i];
        // Always terminated, even if the caller filled every byte.
        mControls[i].action[sizeof(mControls[i].action) - 1] = '\0';
        mControls[i].inputs[sizeof(mControls[i].inputs) - 1] = '\0';
        mControls[i].pad[sizeof(mControls[i].pad) - 1] = '\0';
    }
}

void Menu::setMods(const ModsEntry* entries, int count) {
    mModCount = std::clamp(count, 0, kMaxMods);
    for (int i = 0; i < mModCount; i++) {
        mMods[i] = entries[i];
        mMods[i].label[sizeof(mMods[i].label) - 1] = '\0';
    }
}

std::uint32_t Menu::takeModToggles() {
    const std::uint32_t toggles = mModToggles;
    mModToggles = 0;
    return toggles;
}

void Menu::startBlackOut() {
    switch (mPhase) {
    case Phase::Opening:
    case Phase::List:
    case Phase::Confirm:
    case Phase::Controls:
    case Phase::Mods:
    case Phase::Closing:
        blackOut(Selection::Restart);
        break;
    case Phase::BlackOut:
        // Already fading after Restart or Quit: the console reset takes over,
        // and HomeButtonLayout expects the reset result.
        mResult = Selection::Restart;
        break;
    case Phase::Closed:
    case Phase::Finished:
        break;
    }
}

View Menu::view() const {
    View view;
    view.phase = mPhase;
    switch (mPhase) {
    case Phase::Closed:
    case Phase::Finished:
        return view;
    case Phase::Opening:
        view.panelOpacity = progress(mTime, kFadeFrames);
        break;
    case Phase::List:
    case Phase::Confirm:
    case Phase::Controls:
    case Phase::Mods:
        view.panelOpacity = 1.0f;
        break;
    case Phase::Closing:
        view.panelOpacity = mPanelStart * (1.0f - progress(mTime, kFadeFrames));
        break;
    case Phase::BlackOut:
        view.panelOpacity = mPanelStart;
        view.blackOpacity = mBlackStart + (1.0f - mBlackStart) * progress(mTime, kBlackOutFrames);
        break;
    }
    view.visible = true;
    view.dimOpacity = kDimOpacity * view.panelOpacity;

    const float ax = adjustX();

    const bool confirm = mConfirming != Selection::None;
    const bool controls = mPhase == Phase::Controls;
    if (controls) {
        view.title = "Controls";
        view.message = "Change them in controls.txt in the game's user folder.";
        view.items[0].label = "Back";
        view.lineCount = mControlCount;
        for (int i = 0; i < mControlCount; i++) {
            view.lines[i] = mControls[i];
        }
        view.linesArea = {-kControlsLinesHalfWidth / ax, kControlsLinesTop, kControlsLinesHalfWidth / ax,
                          kControlsLinesBottom};
    } else if (mPhase == Phase::Mods) {
        view.title = "Mods";
        view.message = "Off by default. Saved in mods.txt; keys in controls.txt.";
        for (int i = 0; i < mModCount; i++) {
            view.items[i].label = mModLabels[i];
        }
        view.items[mModCount].label = "Back";
    } else if (!confirm) {
        view.title = "Super Mario Galaxy";
        view.message = "Paused";
        for (int i = 0; i < kListCount; i++) {
            view.items[i].label = kListLabels[i];
        }
    } else {
        const bool restart = mConfirming == Selection::Restart;
        view.title = restart ? "Restart from the title screen?" : "Quit Super Mario Galaxy?";
        view.message = "Progress since your last save will be lost.";
        view.items[kConfirmAccept].label = restart ? "Restart" : "Quit";
        view.items[kConfirmCancel].label = "Cancel";
    }
    view.itemCount = itemCount();
    if (controls) {
        view.panel = {-kControlsHalfWidth / ax, kControlsTop, kControlsHalfWidth / ax,
                      itemRect(0).y1 + kPanelBottomMargin};
        view.titleY = kControlsTitleY;
        view.messageY = kControlsMessageY;
    } else {
        view.panel = {-kPanelHalfWidth / ax, kPanelTop, kPanelHalfWidth / ax,
                      itemRect(view.itemCount - 1).y1 + kPanelBottomMargin};
        const float firstItemTop = itemRect(0).y0;
        view.titleY = kPanelTop + (firstItemTop - kPanelTop) * 0.33f;
        view.messageY = kPanelTop + (firstItemTop - kPanelTop) * 0.66f;
    }
    for (int i = 0; i < view.itemCount; i++) {
        view.items[i].rect = itemRect(i);
        view.items[i].focused = i == mFocus;
    }
    return view;
}

int Menu::itemCount() const {
    if (mPhase == Phase::Controls) {
        return 1;
    }
    if (mPhase == Phase::Mods) {
        return mModCount + 1;
    }
    return mConfirming != Selection::None ? 2 : kListCount;
}

Rect Menu::itemRect(int index) const {
    const float ax = adjustX();
    if (mPhase == Phase::Controls) {
        return {-kControlsBackHalfWidth / ax, kControlsBackTop, kControlsBackHalfWidth / ax,
                kControlsBackTop + kControlsBackHeight};
    }
    const float top = kItemTop + kItemSpacing * static_cast<float>(index);
    return {-kItemHalfWidth / ax, top, kItemHalfWidth / ax, top + kItemHeight};
}

int Menu::itemAt(float x, float y) const {
    for (int i = 0; i < itemCount(); i++) {
        if (itemRect(i).contains(x, y)) {
            return i;
        }
    }
    return -1;
}

void Menu::play(int num) const {
    if (mConfig.sound != nullptr) {
        mConfig.sound(Sound::EventPlaySound, num);
    }
}

void Menu::event(int event) const {
    if (mConfig.sound != nullptr) {
        mConfig.sound(event, 0);
    }
}

void Menu::setFocus(int focus, bool withSound) {
    if (focus == mFocus) {
        return;
    }
    mFocus = focus;
    if (withSound) {
        play(Sound::Focus);
    }
}

void Menu::moveFocus(int delta) {
    setFocus(std::clamp(mFocus + delta, 0, itemCount() - 1), true);
}

void Menu::activate(int index) {
    if (mPhase == Phase::List) {
        switch (index) {
        case kListResume:
            play(Sound::ReturnApp);
            close();
            break;
        case kListControls:
            play(Sound::Select);
            mPhase = Phase::Controls;
            mFocus = 0;
            mRepeatDir = 0;
            break;
        case kListMods:
            play(Sound::Select);
            for (int i = 0; i < mModCount; i++) {
                std::snprintf(mModLabels[i], sizeof(mModLabels[i]), "%s: %s", mMods[i].label, mMods[i].on ? "On" : "Off");
            }
            mPhase = Phase::Mods;
            mFocus = 0;
            mRepeatDir = 0;
            break;
        case kListRestart:
        case kListQuit:
            play(Sound::Select);
            mConfirming = index == kListRestart ? Selection::Restart : Selection::Quit;
            mPhase = Phase::Confirm;
            mFocus = kConfirmCancel;
            mRepeatDir = 0;
            break;
        }
    } else if (mPhase == Phase::Controls) {
        play(Sound::Cancel);
        mPhase = Phase::List;
        mFocus = kListControls;
        mRepeatDir = 0;
    } else if (mPhase == Phase::Mods) {
        if (index >= 0 && index < mModCount) {
            play(Sound::Select);
            mMods[index].on = !mMods[index].on;
            mModToggles |= 1u << index;
            std::snprintf(mModLabels[index], sizeof(mModLabels[index]), "%s: %s", mMods[index].label,
                          mMods[index].on ? "On" : "Off");
        } else {
            play(Sound::Cancel);
            mPhase = Phase::List;
            mFocus = kListMods;
            mRepeatDir = 0;
        }
    } else if (mPhase == Phase::Confirm) {
        if (index == kConfirmAccept) {
            play(mConfirming == Selection::Restart ? Sound::ResetApp : Sound::GotoMenu);
            blackOut(mConfirming);
        } else {
            play(Sound::Cancel);
            mFocus = mConfirming == Selection::Restart ? kListRestart : kListQuit;
            mConfirming = Selection::None;
            mPhase = Phase::List;
            mRepeatDir = 0;
        }
    }
}

void Menu::back() {
    if (mPhase == Phase::Confirm) {
        activate(kConfirmCancel);
    } else if (mPhase == Phase::Controls) {
        activate(0);
    } else if (mPhase == Phase::Mods) {
        activate(mModCount);
    } else {
        play(Sound::ReturnApp);
        close();
    }
}

void Menu::close() {
    mPanelStart = view().panelOpacity;
    mPhase = Phase::Closing;
    mTime = 0.0f;
    mResult = Selection::Resume;
    event(Sound::EventBeginExitAnim);
}

void Menu::blackOut(Selection result) {
    const View current = view();
    mPanelStart = current.panelOpacity;
    mBlackStart = current.blackOpacity;
    mPhase = Phase::BlackOut;
    mTime = 0.0f;
    mResult = result;
    event(Sound::EventBeginBlackOut);
}

void Menu::finish() {
    mPhase = Phase::Finished;
    event(Sound::EventEndMenu);
}

// --- Game instance and published view ---

namespace {
std::mutex gViewMutex;
View gView;
}  // namespace

Menu& instance() {
    static Menu menu;
    return menu;
}

View publishedView() {
    std::lock_guard<std::mutex> lock(gViewMutex);
    return gView;
}

void publish(const View& view) {
    std::lock_guard<std::mutex> lock(gViewMutex);
    gView = view;
}

}  // namespace PetariNative::HomeMenu

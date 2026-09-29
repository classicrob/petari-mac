#include "petari/home_menu.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace PetariNative::HomeMenu {

namespace {

constexpr float kDimOpacity = 0.5f;

// Layout in KPAD space before the widescreen correction.
constexpr float kPanelHalfWidth = 0.42f;
constexpr float kPanelTop = -0.56f;
constexpr float kPanelBottomMargin = 0.06f;  // below the last item
constexpr float kItemHalfWidth = 0.34f;
constexpr float kItemTop = -0.2f;
constexpr float kItemHeight = 0.2f;
constexpr float kItemSpacing = 0.26f;

constexpr const char* kListLabels[] = {"Resume", "Restart from Title", "Quit"};
constexpr int kListResume = 0;
constexpr int kListRestart = 1;
constexpr int kListQuit = 2;
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

void Menu::startBlackOut() {
    switch (mPhase) {
    case Phase::Opening:
    case Phase::List:
    case Phase::Confirm:
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
    if (!confirm) {
        view.title = "Super Mario Galaxy";
        view.message = "Paused";
        for (int i = 0; i < 3; i++) {
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
    view.panel = {-kPanelHalfWidth / ax, kPanelTop, kPanelHalfWidth / ax,
                  itemRect(view.itemCount - 1).y1 + kPanelBottomMargin};
    for (int i = 0; i < view.itemCount; i++) {
        view.items[i].rect = itemRect(i);
        view.items[i].focused = i == mFocus;
    }
    return view;
}

int Menu::itemCount() const {
    return mConfirming != Selection::None ? 2 : 3;
}

Rect Menu::itemRect(int index) const {
    const float ax = adjustX();
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
        case kListRestart:
        case kListQuit:
            play(Sound::Select);
            mConfirming = index == kListRestart ? Selection::Restart : Selection::Quit;
            mPhase = Phase::Confirm;
            mFocus = kConfirmCancel;
            mRepeatDir = 0;
            break;
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

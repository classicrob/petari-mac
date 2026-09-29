#pragma once
// Native replacement for the Wii HOME Button Menu.
//
// On the Wii, HomeButtonLayout drives the SDK's HOME Button Menu, which the
// game loads as PowerPC machine code (HomeButtonMenuWrapperRSO.rso). That
// module cannot run natively, so the native branch of HomeButtonMenuWrapper.cpp
// binds the wrapper's seven HBM entry points to this menu instead. The menu
// keeps the contract HomeButtonLayout relies on:
//
// - HBMCreate once; HBMInit each time the layout becomes active; HBMCalc every
//   active frame with the game's own KPAD data (MR::getHBMKPadData).
// - HBMGetSelectBtnNum stays HBM_SELECT_NULL while the menu runs, which keeps
//   the scene paused. It reports a selection only after the closing
//   animation: HBM_SELECT_HOMEBTN (resume) after a fade out, HBM_SELECT_BTN1
//   (quit; the game calls OSReturnToMenu, which exits the process natively) and
//   HBM_SELECT_BTN2 (application reset to the title) after a fade to black,
//   because the layout then starts its reset with the screen already black.
// - HBMStartBlackOut (a console reset while the menu is open) fades to black
//   and reports HBM_SELECT_BTN2; the layout's _25 flag only matters for that
//   result. While the menu is closed it has no effect.
//
// The menu offers Mac actions (Resume, Controls, Restart from Title, Quit) and
// does not present Wii system features. Controls lists the keyboard and mouse
// controls as currently bound (setControls; the bridge refreshes them from the
// input layer each time the menu opens, so controls.txt remaps show). Rendering is a Dear ImGui overlay drawn in the
// host frame (drawImGuiOverlay); the state machine itself has no rendering or
// SDK dependencies.
//
// Controls come from the game's KPAD data, so the approved bindings apply
// unchanged (native/CONTROLS.md, native/input):
// - Home (F1): resume.
// - Plus (Escape): back; from the list it resumes.
// - A (Space, right mouse): activate the focused item, or the item under the
//   pointer.
// - B alone (left mouse): activate the item under the pointer; elsewhere, back.
// - Up/Down (arrow keys, W/S via the Nunchuk stick): move the focus, with
//   hold repeat.
// - Pointer (mouse): moving onto an item focuses it.

#include <cstdint>

namespace PetariNative::HomeMenu {

// Values equal HBMSelectBtnNum.
enum class Selection : int { None = -1, Resume = 0, Quit = 1, Restart = 2 };

enum class Phase : std::uint8_t {
    Closed,    // before the first open
    Opening,   // fade in; input ignored
    List,      // Resume / Controls / Restart from Title / Quit
    Confirm,   // confirming Restart or Quit
    Controls,  // the controls page, with Back
    Closing,   // fade out before reporting Resume
    BlackOut,  // fade to black before reporting Restart or Quit
    Finished,  // selection() reports the result
};

// Controller buttons as the menu reads them (bits of FrameInput).
namespace Button {
constexpr std::uint32_t Up = 1u << 0;
constexpr std::uint32_t Down = 1u << 1;
constexpr std::uint32_t A = 1u << 2;
constexpr std::uint32_t B = 1u << 3;
constexpr std::uint32_t Plus = 1u << 4;
constexpr std::uint32_t Home = 1u << 5;
}  // namespace Button

// One frame of controller input, all controllers combined.
struct FrameInput {
    std::uint32_t hold = 0;
    std::uint32_t trigger = 0;
    bool pointerValid = false;
    // KPAD pointer position: -1..1 across the game image, y down.
    float pointerX = 0.0f;
    float pointerY = 0.0f;
};

// HBM sound events, as HBMSoundCallback receives them (see revolution/hbm).
namespace Sound {
constexpr int EventBeginExitAnim = 2;  // HBMSEV_BEGIN_EXIT_ANIM
constexpr int EventBeginBlackOut = 3;  // HBMSEV_BEGIN_BLACKOUT
constexpr int EventEndMenu = 4;        // HBMSEV_END_MENU
constexpr int EventPlaySound = 5;      // HBMSEV_PLAY_SOUND; num is one of:
constexpr int HomeButton = 0;          // HBMSE_HOME_BUTTON
constexpr int ReturnApp = 1;           // HBMSE_RETURN_APP
constexpr int GotoMenu = 2;            // HBMSE_GOTO_MENU
constexpr int ResetApp = 3;            // HBMSE_RESET_APP
constexpr int Focus = 4;               // HBMSE_FOCUS
constexpr int Select = 5;              // HBMSE_SELECT
constexpr int Cancel = 6;              // HBMSE_CANCEL
}  // namespace Sound

using SoundCallback = int (*)(int event, int num);

struct Config {
    SoundCallback sound = nullptr;
    // Animation time per HBMCalc, in 60 Hz frames (HBMDataInfo::frameDelta).
    float frameDelta = 1.0f;
    // Horizontal correction for a 16:9 image (HBMDataInfo::adjust.x), applied
    // while the widescreen flag (HBMSetAdjustFlag) is set so the panel keeps
    // its shape.
    float widescreenAdjustX = 832.0f / 608.0f;
};

// Animation lengths in 60 Hz frames, and focus repeat timing.
constexpr float kFadeFrames = 10.0f;
constexpr float kBlackOutFrames = 30.0f;
constexpr float kRepeatDelayFrames = 24.0f;
constexpr float kRepeatIntervalFrames = 6.0f;

// Rectangles in KPAD pointer space (-1..1 across the game image, y down).
struct Rect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

constexpr int kMaxItems = 4;
constexpr int kMaxControls = 20;

// One line of the Controls page: what the player does, and the inputs.
struct ControlsEntry {
    char action[40] = {};
    char inputs[64] = {};
};

struct ViewItem {
    const char* label = "";
    Rect rect;
    bool focused = false;
};

// What the overlay draws.
struct View {
    bool visible = false;
    Phase phase = Phase::Closed;
    float dimOpacity = 0.0f;    // darkening of the paused game behind the panel
    float panelOpacity = 0.0f;  // panel, text, and items
    float blackOpacity = 0.0f;  // full-image black, over everything
    Rect panel;
    const char* title = "";
    const char* message = "";  // may be empty
    float titleY = 0.0f;       // text centers, KPAD space
    float messageY = 0.0f;
    int itemCount = 0;
    ViewItem items[kMaxItems];
    // The Controls page: lines drawn as two columns in linesArea, one row each.
    int lineCount = 0;
    ControlsEntry lines[kMaxControls];
    Rect linesArea;
};

class Menu {
public:
    void create(const Config& config);  // HBMCreate
    void setWidescreen(bool widescreen);  // HBMSetAdjustFlag
    void open();                          // HBMInit
    void update(const FrameInput& input);  // HBMCalc
    void startBlackOut();                 // HBMStartBlackOut
    // The Controls page contents (at most kMaxControls lines; longer text is
    // cut). Kept across opens.
    void setControls(const ControlsEntry* entries, int count);
    Selection selection() const { return mPhase == Phase::Finished ? mResult : Selection::None; }  // HBMGetSelectBtnNum

    Phase phase() const { return mPhase; }
    int focus() const { return mFocus; }
    // Pending action while in Confirm (Restart or Quit).
    Selection confirming() const { return mConfirming; }
    View view() const;

private:
    int itemCount() const;
    Rect itemRect(int index) const;
    float adjustX() const { return mWidescreen ? mConfig.widescreenAdjustX : 1.0f; }
    void play(int num) const;
    void event(int event) const;
    void setFocus(int focus, bool withSound);
    void moveFocus(int delta);
    void activate(int index);
    void back();
    void close();
    void blackOut(Selection result);
    void finish();
    int itemAt(float x, float y) const;

    Config mConfig;
    bool mCreated = false;
    bool mWidescreen = false;
    Phase mPhase = Phase::Closed;
    Selection mResult = Selection::None;
    Selection mConfirming = Selection::None;
    int mFocus = 0;
    float mTime = 0.0f;          // frames into the current animation
    float mBlackStart = 0.0f;    // black opacity when the black-out began
    float mPanelStart = 0.0f;    // panel opacity when the closing began
    std::uint32_t mRepeatDir = 0;  // Button::Up or Button::Down being repeated
    float mRepeatTime = 0.0f;
    bool mPointerWasValid = false;
    float mPointerX = 0.0f;
    float mPointerY = 0.0f;
    int mControlCount = 0;
    ControlsEntry mControls[kMaxControls];
};

// --- The game's menu instance (HomeButtonMenuWrapper.cpp's native branch) ---

Menu& instance();
// Snapshot published by HBMDraw, hidden once a selection is reported. Any thread.
View publishedView();
void publish(const View& view);

// Draws publishedView() with Dear ImGui (target petari_home_menu_imgui). Call
// on the thread that owns the ImGui context, between aurora_begin_frame() and
// aurora_end_frame(). The rectangle is the game image in ImGui display
// coordinates. Draws nothing while the menu is hidden. The overlay window
// takes no input, so ImGui never captures the game's keyboard or mouse.
void drawImGuiOverlay(float imageX, float imageY, float imageWidth, float imageHeight);

}  // namespace PetariNative::HomeMenu

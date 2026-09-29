#pragma once
// Automated smoke run past the title screen (PETARI_SMOKE=title). Opt-in: with
// PETARI_SMOKE unset nothing here runs.
//
// The driver sees what the game shows (scene, stage, strap reminder, save-data
// sequence, milestones) once per frame at the frame seam, and answers only
// with Wii Remote button presses through the native input layer's bindings.
// Presses go through the game's real KPAD path; nothing skips a load or
// changes game state. When it is done it presses the power button, so the
// game shuts down through its own reset process.
//
// Script "title":
// 1. Logo scene. While the strap reminder is shown, tap A after 480 frames and
//    every 120 frames after (the game accepts any button after 450 frames and
//    moves on by itself at 1200).
// 2. Wait for scene Game, stage FileSelect, fully initialised, and the
//    TitleSequence.LogoDisplay milestone (the title reads A and B from then,
//    after its BGM is prepared); 10 frames later hold A and B together for 20
//    frames (the "Press [A][B]" title). Up to 3 attempts, 300 frames apart,
//    until FileSelector.TitleEnd. FAIL if TitleSequence.BgmPrepare is not
//    followed by LogoDisplay within 1800 frames (STM_TITLE not prepared).
// 3. If the Mii error window appears (FileSelector.RFLError, a key window),
//    tap A after 90 frames to dismiss it, as a player would.
// 4. PASS at FileSelector.FileSelect (the files are selectable).
// BLOCKED when the save-data sequence stays active for 900 frames (a Yes/No
// prompt needs the pointer, which this script does not guess at). FAIL on an
// unexpected scene or stage, or when the frame limit runs out.
//
// Script "playable" (PETARI_SMOKE=playable) continues from file select with
// the game's UI targets (petari/ui_observe.hpp), pointing only at targets the
// game publishes and pressing A only once the game reports the pointer over a
// selectable target:
// 5. The lowest empty FileSelect.Slot. The prompts System_FileSelect001 and
//    System_FileSelect013 (yes/no) are answered Prompt.Yes. The blocking
//    "saving" window System_Save01 (type 1) is left alone while the new file
//    is saved (after FileSelector.Create, before FileSelector.MiiSelect) and
//    while the chosen icon is saved (after System_FileSelect013 was answered
//    Yes, before FileSelector.FileConfirm).
//    Any other prompt, or these elsewhere, is BLOCKED with its message ID.
// 6. FileSelector.MiiSelect: MiiSelect.Mario. FileSelector.FileConfirm:
//    FileSelect.Start. After FileSelector.DemoStartWait any scene and stage
//    may follow.
// 7. The prologue: tap A 30 frames after each ProloguePictureBook.PageReady
//    (ProloguePictureBook::exeKeyWait, five pages) and PrologueLetter.Ready
//    milestone, never otherwise. The storybook's PictureBook.PageReady does
//    not drive the prologue. FAIL if no prologue
//    milestone arrives for 3600 frames.
// 8. Prologue.GameStart: 120 frames later record Mario's position, hold the
//    input bound to the stick's up for 90 frames, and PASS if he moved at
//    least 50 units within 30 frames of letting go.
//
// Script "gameplay" (PETARI_SMOKE=gameplay): the playable path to
// Prologue.GameStart, then gameplay checks (below). Script "reload"
// (PETARI_SMOKE=reload), for a copied save: the title, then the lowest
// selectable NON-empty FileSelect.Slot, FileSelector.FileConfirm,
// FileSelect.Start, the prologue as needed, and the same gameplay checks. It
// FAILs if FileSelector.Create is reached or the saving window System_Save01
// appears (the save must not be rewritten), and answers no prompts.
//
// Gameplay checks, all through bound inputs (A jump, Plus pause, the stick):
// a. Ready: Mario present, no demo, pausing permitted, for 60 frames in a row.
// b. Idle 120 frames: he stays within 5 units (so a fall cannot pass as
//    movement) and on the ground.
// c. Jump (tap A): he leaves the ground within 30 frames, rises at least 60
//    units against gravity, and lands within 180 frames.
// d. Stick up 45 frames, then stick down 45 frames: each moves him at least
//    40 units across the ground (gravity component removed), in opposite
//    directions (cosine below -0.5).
// e. Pause: hold the default Plus binding (Escape) for 18 frames while
//    pausing is permitted, then release; a PauseMenu.Open after that hold.
//    (PauseButtonCheckerInGame opens the menu at a 12-frame Plus/Minus hold,
//    but not while A or B is held, and Escape is also bound to B: if the menu
//    does not open, the FAIL reports the game's buttons during the hold.) Holding the stick
//    for 45 frames while paused must not move him (under 1 unit). Tap Plus:
//    a PauseMenu.Close after that press (earlier ones, e.g. the menu's kill at
//    scene start, do not count). Then stick up 45 frames moves him at least 40
//    units: PASS.
// Every position is logged. When the result is decided, inputs still held
// are released at once.

#include <string>
#include <vector>

namespace PetariNative::App::Smoke {

struct Observation {
    std::string scene;          // current SceneControlInfo scene ("" before the first)
    std::string stage;
    bool sceneReady = false;    // scene initialisation finished
    bool strap = false;         // strap reminder (Logo scene) on screen
    bool saveSequence = false;  // save-data handling sequence active
    bool videoConfigured = false;  // VI has latched a render mode
    bool videoBlack = true;        // VI output blanked
    std::vector<std::string> milestones;  // recorded since the previous frame

    struct Target {
        std::string id;
        int index = 0;
        float u = 0.0f;  // normalised pointer screen position (petari/ui_observe.hpp)
        float v = 0.0f;
        unsigned flags = 0;
    };
    struct Prompt {
        std::string messageId;
        int type = 0;  // 0 key, 1 blocking, 2 yes/no
    };
    std::vector<Target> targets;  // shown this frame
    std::vector<Prompt> prompts;  // appeared since the previous frame
    bool playerValid = false;     // filled only while the driver wants it (wantsPlayer)
    float playerX = 0.0f, playerY = 0.0f, playerZ = 0.0f;
    bool playerOnGround = false;  // with playerValid: MR::isOnGroundPlayer
    float gravityX = 0.0f, gravityY = -1.0f, gravityZ = 0.0f;  // MR::getPlayerGravity
    bool demoActive = false;      // MR::isDemoActive
    bool pausePermitted = false;  // GameScene::isPermitToPauseMenu
    // The game's view of the Wii Remote (channel 0), with playerValid.
    bool padA = false, padB = false, padPlus = false, padMinus = false;
    bool padOperating = false;  // MR::isOperatingWPad: blocks the pause button
};

// Target flags (petari/ui_observe.hpp).
constexpr unsigned kTargetPointing = 1u;
constexpr unsigned kTargetEmpty = 2u;
constexpr unsigned kTargetSelectable = 4u;

enum class Script { Title, Playable, Gameplay, Reload };

enum class Button { A, B, StickUp, StickDown, Plus, Minus };

struct Press {
    Button button;
    bool down;
};

struct Step {
    std::vector<Press> presses;
    bool assertFocus = false;  // before presses: the input layer ignores them while unfocused
    bool pointer = false;      // move the pointer to (u, v), normalised over the game image
    float pointerU = 0.0f;
    float pointerV = 0.0f;
    bool requestQuit = false;  // press the power button (once, when the result is decided)
};

enum class Result { Running, Pass, Fail, Blocked };

// Process exit status for a result: 0 pass, 1 fail, 2 blocked.
int exitStatus(Result result);
const char* resultName(Result result);

class Driver {
public:
    explicit Driver(unsigned long frameLimit, Script script = Script::Title);

    // Once per frame, at the seam.
    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    // Static name of the current phase, for logs and the watchdog.
    const char* phase() const;
    // Log lines produced by the last step.
    const std::vector<std::string>& log() const { return mLog; }
    // Whether the next observation should include Mario's position (only in
    // the game, after Prologue.GameStart).
    bool wantsPlayer() const {
        return mPhase == Phase::Move || mPhase >= Phase::Ready ||
               (mPhase == Phase::Prologue && mScript != Script::Playable);
    }

private:
    enum class Phase {
        Boot, Logo, WaitTitle, TitleReady, Holding, WaitTitleEnd, WaitFileSelect,
        // playable
        ChooseSlot, WaitMiiSelect, ChooseMario, WaitFileConfirm, ChooseStart, WaitDemo, Prologue, Move,
        // gameplay and reload (after the prologue); keep these last before Done
        Ready, Idle, Jump, Forward, Backward, PauseOpen, Paused, PauseClose, Resume,
        Done
    };
    enum class Slot { Any, Empty, NonEmpty };
    // Points at a target and taps A once the game reports the pointer over it.
    // Returns true on the frame A is pressed.
    bool aimAndPress(const Observation& observation, const char* id, Slot slot, Step& step);
    void playable(const Observation& observation, Step& step);
    void gameplay(const Observation& observation, Step& step);
    bool gameplayReady(const Observation& observation);
    bool seen(const std::string& milestone) const;
    // How often a milestone was recorded so far (new events after an input are
    // those beyond the count taken at the input).
    unsigned long seenCount(const std::string& milestone) const;

    void finish(Result result, const std::string& reason, Step& step);
    void tap(Button button, unsigned long holdFrames, Step& step);
    void pressTitle(Step& step);
    void note(const std::string& line);

    struct Release {
        unsigned long frame;
        Button button;
    };

    unsigned long mFrameLimit;
    unsigned long mFrame = 0;
    Phase mPhase = Phase::Boot;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    std::string mLastScene;
    std::string mLastStage;
    bool mLastStrap = false;
    bool mLastSave = false;
    int mLastVideo = -1;  // configured * 2 + black; -1 before the first frame
    unsigned long mStrapFrames = 0;
    unsigned long mSaveFrames = 0;
    unsigned long mPhaseFrames = 0;
    int mTitleAttempts = 0;
    long mBgmPrepareAt = -1;
    long mLogoDisplayAt = -1;
    long mRflTapAt = -1;
    // playable
    Script mScript;
    std::vector<std::string> mSeen;
    std::string mPrompt;          // allowed prompt being answered
    bool mIconConfirmed = false;  // System_FileSelect013 answered Yes
    unsigned long mAimFrames = 0;
    unsigned long mAimMissing = 0;
    unsigned long mPointingFrames = 0;
    long mPrologueTapAt = -1;
    unsigned long mSinceProgress = 0;
    unsigned long mMoveFrame = 0;
    float mStartX = 0.0f, mStartY = 0.0f, mStartZ = 0.0f;
    // gameplay
    unsigned long mReadyFrames = 0;
    bool mLeftGround = false;
    float mMaxRise = 0.0f;
    float mForwardX = 0.0f, mForwardY = 0.0f, mForwardZ = 0.0f;  // forward displacement
    bool mPausePressed = false;
    unsigned long mPauseOpenCount = 0;   // PauseMenu.Open events before our Plus
    unsigned long mPauseCloseCount = 0;  // PauseMenu.Close events before our second Plus
    std::string mPadDuringHold;          // game buttons while the pause button was held
};

// --- Process-wide state for the app (smoke.cpp) ---

// Whether PETARI_SMOKE selects a known script ("title", "playable",
// "gameplay" or "reload"), and which; prints why not otherwise.
bool enabledFromEnvironment(Script* script);
// Exit status the power exit handler uses: the smoke result, or 0 when the
// smoke is not running or has not decided.
int processExitStatus();
void setProcessResult(Result result);

// A host thread that ends the process if frames stop: status 124 when no
// frame reaches the seam for stallSeconds (loading hangs included), 125 when
// the game has not exited shutdownSeconds after the smoke pressed the power
// button. It reads only values published below, never game state.
void startWatchdog(unsigned stallSeconds, unsigned shutdownSeconds);
void heartbeat(unsigned long frame, const char* phase);
void noteQuitRequested(bool saveSequenceActive);

}  // namespace PetariNative::App::Smoke

namespace PetariNative::App::Smoke {
// smoke_game.cpp (SDK side): what the game shows now, and the milestones
// recorded since the previous call. Main thread, at the seam, while it holds
// the CPU (before the seam releases it).
Observation observeGame(bool wantPlayer);
}  // namespace PetariNative::App::Smoke

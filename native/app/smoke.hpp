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
//
// Script "story" (PETARI_SMOKE=story), for a copied save: the reload path to
// gameplay, then input-only walks along waypoint routes (native/app/smoke.cpp
// kRoutes): from the start to the plaza, where the PrologueA movie starts;
// after it and the attack, to the castle, where PrologueB starts; PASS when
// the Game scene is ready on HeavensDoorGalaxy. Each walk starts with a
// calibration (stick up, then right, 20 frames each) that learns how the
// camera axes relate to the stick, then steers each frame toward the next
// waypoint with 8-way keys relative to the camera. A waypoint counts within
// 600 units on the town road and 100 on the castle walk (its clearance), a
// segment's last one (inside the movie trigger) within 200 and 80. Stuck (less
// than 50 units closer in 180 frames): a sidestep,
// then a jump; the fifth time on one waypoint FAILs. FAIL also on any prompt,
// an open talk, an unexpected scene or stage, or a segment/movie timeout.
// Mario's position is needed only while walking: during the movies and the
// stage load he may be absent (the scene is torn down after PrologueB), and
// no input is sent. PASS needs HeavensDoorGalaxy ready for 60 frames in a row
// (so it has updated and drawn), within 3600 frames of PrologueB's end.
//
// Script "galaxy" (PETARI_SMOKE=galaxy): load a copied observatory fixture,
// walk the AstroBaseA collision-checked corridor to Terrace with calibrated
// stick inputs, point at its Blue Star, reveal Good Egg if new, then select
// Good Egg, Start and first mission. Reveal waits for the observed Open/Wait
// target with a bounded timeout and sends no new input.
// Only published selectable targets can receive A; visible non-choice lecture
// pages may receive a debounced A. Require AstroDome scenario 1 and then
// EggStarGalaxy scenario 1 before the gameplay checks above. Route, target and
// load timeouts fail; backing out, death and wrong stages/missions fail too.
// Fixture bootstrap is separate from this input-only driver. Static collision
// inspection supports the route; a live unattended PASS is still required to
// validate it against actual actors, cameras and game timing.
//
// Assisted runs (every script): physical keyboard and mouse input still works
// during a smoke run. A press or release (not a key repeat) of a key or mouse
// button bound to a game action, from the driver's first frame on, is logged
// with its phase and position, and a run that would PASS ends ASSISTED (exit
// status 3) instead: someone helped. The first frame on which such input was
// seen is reported, with the latest input of that frame. FAIL and BLOCKED keep their status and
// name the assistance. The driver's own presses go to the input layer directly
// and never count. Pointer motion and focus changes are logged but do not make
// a run assisted: no story phase uses the pointer, but a PASS then reports
// them rather than claiming the run was untouched.
// Steering and distances use the plane perpendicular to the observed gravity
// field at Mario. Non-finite or degenerate gravity/camera axes FAIL. So does
// gravity tilted beyond 60 degrees from the stage's down: a limitation of this
// route's ground-plane waypoints, not a game fault.

#include <string>
#include <vector>

namespace PetariNative::App::Smoke {

// Physical (hardware) input since launch, counted by the app's event handler
// (Events::input) and copied into each Observation by the seam. Fixed size: it
// is written on the event path without allocating.
struct PhysicalInputs {
    unsigned long gameplay = 0;  // presses and releases of keys/mouse buttons bound to a game action (no repeats)
    unsigned long pointer = 0;   // pointer motion events
    unsigned long focus = 0;     // window focus changes
    char last[64] = {};          // the latest gameplay input, e.g. "key W (StickUp) down"; the driver
                                 // sees one per frame, so it reports frames, not first key edges
};

struct Observation {
    std::string scene;          // current SceneControlInfo scene ("" before the first)
    std::string stage;
    int scenario = 0;
    int selectedScenario = 0;   // the scenario chosen (SceneControlInfo::mSelectedScenarioNo; differs for hidden stars)
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
    float gravityX = 0.0f, gravityY = -1.0f, gravityZ = 0.0f;  // gravity field at Mario (Mario::getAirGravityVec)
    bool demoActive = false;      // MR::isDemoActive
    bool pausePermitted = false;  // GameScene::isPermitToPauseMenu
    // The game's view of the Wii Remote (channel 0), with playerValid.
    bool padA = false, padB = false, padPlus = false, padMinus = false;
    bool padOperating = false;  // MR::isOperatingWPad: blocks the pause button
    // Camera axes (MR::getCamXdir / getCamZdir), with playerValid; the story
    // route steers relative to them.
    float camXx = 1.0f, camXy = 0.0f, camXz = 0.0f;
    float camZx = 0.0f, camZy = 0.0f, camZz = 1.0f;
    // Camera rotation, with playerValid: the game's D-pad left/right triggers
    // this frame, and whether the active camera allows rotation
    // (CameraDirector::isEnableToRound*; when it does not, the game plays its
    // "can't" sound, SE_SY_CAMERA_NG, instead of rotating).
    bool padLeftTrigger = false, padRightTrigger = false;
    bool camRoundLeft = false, camRoundRight = false;
    bool talkActive = false;  // MR::isSystemTalking: a talk window is open (story route FAILs)
    bool playerDead = false;  // MR::isPlayerDead
    PhysicalInputs physical;  // filled by the seam (Events::physicalInputs)

    // For mission scripts (smoke_goodegg.hpp). Actors published this frame
    // (petari/actor_observe.hpp; kinds in Game/Util/NativeActorObserve.hpp),
    // always taken. The rest only with playerValid.
    struct Actor {
        std::string kind;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float dx = 0.0f, dy = 0.0f, dz = 0.0f;
        int state = 0;
        unsigned flags = 0;  // kActorReady, kActorBound, kActorHostile
    };
    std::vector<Actor> actors;
    bool starEggStar1 = false;   // GameDataFunction::hasPowerStar("EggStarGalaxy", 1): the loaded file's record
    int powerStars = -1;         // GameDataFunction::calcCurrentPowerStarNum
    bool stageResult = false;    // GameSequenceFunction::hasStageResultSequence: a star was touched this stage
    int playerLife = -1;         // MarioActor health
    bool playerInBind = false;   // MR::isPlayerInBind: held by a launch star, vine, pipe, ...
    bool playerSwinging = false; // MR::isPlayerSwingAction: spinning
    // Mario::mShadowPos (the ground point his shadow ray found) and the gravity
    // Mario itself uses this frame (Mario::getGravityVec), for landing traces.
    float shadowX = 0.0f, shadowY = 0.0f, shadowZ = 0.0f;
    float marioGravityX = 0.0f, marioGravityY = 0.0f, marioGravityZ = 0.0f;
    // For the mechanics checks (smoke_mechanics.cpp), with playerValid: MarioActor::mPlayerMode
    // (0 normal, 1 invincible, 3 ice, 4 bee, 5 spring, 6 boo, 7 red star), Mario's current
    // status (MarioState.hpp: 6 swim, 31 skate, ...), and a bee stuck to a honeycomb wall.
    int playerMode = -1;
    int marioStatus = -1;
    bool beeWallWalk = false;
    // Player locks, with playerValid (the stage script's lock-up probe): MR::isOffPlayerControl,
    // MR::isPlayerInRush (bound to an actor: rides, cannons, race starts) and that actor's name.
    bool playerOffControl = false;
    bool playerInRush = false;
    std::string rushActor;
};

// Actor flags (petari/actor_observe.hpp).
constexpr unsigned kActorReady = 1u;
constexpr unsigned kActorBound = 2u;
constexpr unsigned kActorHostile = 4u;

// A point on the story route, on the ground plane (gravity is -y there).
struct Waypoint {
    float x, z;
};

// Keys for a desired direction in the camera frame: x right, y forward
// (unit-free). 8-way, as digital W/A/S/D keys can express.
struct StickKeys {
    bool up = false, down = false, left = false, right = false;
};
StickKeys stickKeysFor(float x, float y);
// The story script's waypoints: segment 0 to the plaza, 1 to the castle.
const std::vector<Waypoint>& storyRoute(int segment);

// Target flags (petari/ui_observe.hpp).
constexpr unsigned kTargetPointing = 1u;
constexpr unsigned kTargetEmpty = 2u;
constexpr unsigned kTargetSelectable = 4u;

// Observe: load the saved file, then only watch (scene and stage changes, prompts)
// until the frame limit; for long non-interactive sequences such as the ending.
enum class Script { Title, Playable, Gameplay, Reload, Story, Galaxy, Observe };

// Spin (the Shake binding) and CameraLeft/CameraRight (D-pad left/right, the
// camera rotation bindings) are used by the stage script (smoke_stage.hpp).
enum class Button { A, B, StickUp, StickDown, Plus, Minus, StickLeft, StickRight, Spin, CameraLeft, CameraRight, Z };

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
    // Test-only player warp (stage script, PETARI_STAGE_WARP): to warpName (a GeneralPos, via
    // MR::setPlayerPos) when not empty, else to (warpX, warpY, warpZ) via MR::setPlayerPosAndWait.
    bool warp = false;
    std::string warpName;
    float warpX = 0.0f, warpY = 0.0f, warpZ = 0.0f;
};

// Assisted: the script's goal was reached, but with physical gameplay input
// (see "Assisted runs" above), so it is not an unattended PASS.
enum class Result { Running, Pass, Fail, Blocked, Assisted };

// Process exit status for a result: 0 pass, 1 fail, 2 blocked, 3 assisted.
int exitStatus(Result result);
const char* resultName(Result result);

class Driver {
public:
    // PETARI_SMOKE_PLAYER=luigi (read here): on a file with Luigi unlocked, switch to
    // Luigi with FileSelect.Bros after the file is chosen, before Start. Unset or
    // anything else: the file's default player, as before. A missing switch (Luigi
    // not unlocked) is a FAIL, not a silent Mario run.
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
    // Script::Galaxy: Good Egg mission 1 is loaded and ready (the route's
    // last step, before its gameplay checks). Mission scripts
    // (smoke_goodegg.hpp) take over from here.
    bool galaxyMissionReady() const { return mScript == Script::Galaxy && mGalaxyPhase == GalaxyPhase::Complete; }

private:
    enum class Phase {
        Boot, Logo, WaitTitle, TitleReady, Holding, WaitTitleEnd, WaitFileSelect,
        // playable
        ChooseSlot, WaitMiiSelect, ChooseMario, WaitFileConfirm, ChooseLuigi, ChooseStart, WaitDemo, Prologue, Move,
        // gameplay and reload (after the prologue); keep these last before Done
        Ready, Idle, Jump, Forward, Backward, PauseOpen, Paused, PauseClose, Resume,
        // story route
        Calibrate, Route, WaitMovie, MovieEnd, WaitStage,
        Done
    };
    enum class Slot { Any, Empty, NonEmpty };
    // Points at a target and taps A once the game reports the pointer over it.
    // Returns true on the frame A is pressed.
    bool aimAndPress(const Observation& observation, const char* id, Slot slot, Step& step);
    void playable(const Observation& observation, Step& step);
    void gameplay(const Observation& observation, Step& step);
    void story(const Observation& observation, Step& step);
    bool galaxy(const Observation& observation, Step& step);
    bool loadsSave() const {
        return mScript == Script::Reload || mScript == Script::Story || mScript == Script::Galaxy || mScript == Script::Observe;
    }
    // Holds exactly these steering keys (presses for the changes).
    void steer(const StickKeys& keys, Step& step);
    void startSegment(int segment);
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
    std::string mObserved;        // Observe: the scene/stage last logged
    unsigned long mObserveTalkFrames = 0, mObserveTaps = 0;
    bool mIconConfirmed = false;  // System_FileSelect013 answered Yes
    unsigned long mAimFrames = 0;
    unsigned long mAimMissing = 0;
    unsigned long mPointingFrames = 0;
    bool mChooseLuigi = false;         // PETARI_SMOKE_PLAYER=luigi
    int mLuigiPresses = 0;             // FileSelect.Bros presses so far
    unsigned long mLuigiPressFrame = 0;  // mPhaseFrames at the last press
    long mPrologueTapAt = -1;
    unsigned long mDemoTaps = 0;  // A presses advancing an opening demo without a prologue
    unsigned long mSinceProgress = 0;
    unsigned long mMoveFrame = 0;
    float mStartX = 0.0f, mStartY = 0.0f, mStartZ = 0.0f;
    // gameplay
    enum class GalaxyPhase { Observatory, Dome, BlueStar, Galaxy, Reveal, Confirm, Scenario, Mission, Complete };
    GalaxyPhase mGalaxyPhase = GalaxyPhase::Observatory;
    unsigned long mGalaxyFrames = 0;
    unsigned long mGalaxyTapAfter = 0;
    unsigned long mReadyFrames = 0;
    bool mLeftGround = false;
    float mMaxRise = 0.0f;
    float mForwardX = 0.0f, mForwardY = 0.0f, mForwardZ = 0.0f;  // forward displacement
    bool mPausePressed = false;
    unsigned long mPauseOpenCount = 0;   // PauseMenu.Open events before our Plus
    unsigned long mPauseCloseCount = 0;  // PauseMenu.Close events before our second Plus
    std::string mPadDuringHold;          // game buttons while the pause button was held
    // story route
    int mSegment = 0;                    // 0: plaza (PrologueA), 1: castle (PrologueB)
    size_t mWaypoint = 0;
    StickKeys mHeld;                     // steering keys held now
    float mSignForward = 0.0f, mSignRight = 0.0f;  // camera axis signs from calibration
    float mBestDistance = 0.0f;          // closest to the waypoint in the stuck window
    unsigned long mStuckFrames = 0;
    unsigned long mStageReadyFrames = 0;  // consecutive frames HeavensDoorGalaxy is ready
    // physical input
    bool mPhysicalBaseSet = false;
    PhysicalInputs mPhysicalSeen;        // counts at the last frame
    unsigned long mAssistInputs = 0;     // gameplay inputs since the driver's first frame
    unsigned long mPointerInputs = 0, mFocusInputs = 0;
    std::string mFirstAssist;            // the first one: input, frame, phase, position
    void notePhysical(const Observation& observation);
    int mRecoveries = 0;
    unsigned long mRecoverUntil = 0;
    unsigned long mSegmentFrames = 0;
};

// --- Process-wide state for the app (smoke.cpp) ---

// Whether PETARI_SMOKE selects a known script ("title", "playable",
// "gameplay", "reload", "story" or "galaxy"), and which; prints why not otherwise.
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
// What the watchdog records before the stall exit (124), so a hang explains
// itself: unless PETARI_HANG_SAMPLE=0, /usr/bin/sample's stacks of every
// thread for 3 s, written to petari-hang-<pid>.sample.txt in sampleDirectory
// (the temporary directory if empty); then `dump` (the platform state: OS
// threads and the CPU baton, GX sync, pending alarms), if set. A report that
// has not finished 60 s later is abandoned and the process exits anyway.
void setHangReport(void (*dump)(const char* reason), const std::string& sampleDirectory);
void heartbeat(unsigned long frame, const char* phase);
void noteQuitRequested(bool saveSequenceActive);

}  // namespace PetariNative::App::Smoke

namespace PetariNative::App::Smoke {
// smoke_game.cpp (SDK side): what the game shows now, and the milestones
// recorded since the previous call. Main thread, at the seam, while it holds
// the CPU (before the seam releases it).
Observation observeGame(bool wantPlayer);
// Applies a Step's warp (SDK side, main thread at the seam while it holds the CPU). False when
// there is no Mario to move.
bool warpPlayer(const Step& step);
// collision_probe.cpp (SDK side): PETARI_COLLISION_PROBE, a read-only map
// collision survey for offline route planning; stepped by observeGame.
bool collisionProbeActive();
// PETARI_COLLISION_PROBE_NEAR="x,z,r": start only once Mario (playerValid) is within r of (x, z).
void stepCollisionProbe(const std::string& stage, bool sceneReady, bool playerValid, float playerX, float playerZ);
}  // namespace PetariNative::App::Smoke

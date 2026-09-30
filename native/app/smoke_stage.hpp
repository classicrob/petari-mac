#pragma once
// Stage smoke script (PETARI_SMOKE=stage), for the whole-game stage sweep
// (native/tools/stage_sweep.py). Requires --test-fixture stage with
// PETARI_STAGE=<stage> and PETARI_SCENARIO=<n> (app_main.cpp): after the saved
// file loads, the game's own after-loading galaxy move enters that stage and
// scenario instead of the observatory. This is a SYNTHETIC entry: it proves
// nothing about progression, unlocks or how a player reaches the stage.
//
// 1. Boot: the "reload" script (smoke.hpp) through the title, file select, the
//    lowest non-empty file and Start, until the Game scene changes to a stage
//    other than FileSelect. FAIL if that stage is not PETARI_STAGE (the entry
//    was not applied).
// 2. Load: the scene is ready on PETARI_STAGE with the selected scenario
//    PETARI_SCENARIO, within 5400 frames.
// 3. Ready: Mario present, no demo, no talk, pausing permitted for 60 frames
//    in a row, within 5400 frames. An open talk (or a Talk.Advance target) gets
//    a tap of A every 45 frames, as a player reads through it.
// 4. Exercise, each step logged as "stage check <name>: ok|warn|fail ...":
//    idle 120 frames; stick up, down, right, left 30 frames each (each moves
//    Mario at least 40 units across the ground to be "ok"); jump (leaves the
//    ground within 30 frames, rises 60 units, lands within 240); spin (the
//    Shake binding); camera rotation left and right (the angle is reported);
//    pause (hold Plus 18 frames, PauseMenu.Open), 90 frames paused, Plus
//    (PauseMenu.Close); then PETARI_STAGE_IDLE_FRAMES (default 600) more
//    frames with no input.
// PASS: the stage loaded and became ready, the pause menu opened and closed,
// and nothing below ended the run. Movement, jump and camera results are
// reported as "warn" rather than failing: stage starts differ (water, Star
// Ball, slopes, cameras that do not rotate), so they are for triage, not a
// verdict. FAIL: wrong stage or scenario, load or ready timeout, the player
// vanishing, leaving the stage, pause not permitted or not opening/closing,
// Mario dying ("died: ..."), the frame limit. BLOCKED: a system prompt.
// Crashes, heap failures and hangs end the process (crash report, watchdog
// exit 124); the sweep runner classifies those from the log and exit status.
// Physical gameplay input after the boot makes a PASS "ASSISTED".

#include <string>
#include <vector>

#include "smoke.hpp"

namespace PetariNative::App::Smoke {

struct StageConfig {
    std::string stage;
    int scenario = 0;
    unsigned long tailFrames = 600;
};

// PETARI_SMOKE=stage: reads PETARI_STAGE, PETARI_SCENARIO and
// PETARI_STAGE_IDLE_FRAMES. False (with a message) when stage is not selected
// or the variables are missing.
bool stageEnabledFromEnvironment(StageConfig* config);

class StageDriver {
public:
    StageDriver(unsigned long frameLimit, const StageConfig& config);

    // Once per frame, at the seam (same contract as Driver).
    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    const char* phase() const;
    const std::vector<std::string>& log() const { return mLog; }
    bool wantsPlayer() const { return mPhase != Phase::Boot || mBoot.wantsPlayer(); }

private:
    enum class Phase { Boot, Load, Ready, Idle, Walk, Jump, Spin, Camera, PauseOpen, Paused, PauseClose, Tail, Done };
    struct Check {
        std::string name;
        const char* status;  // "ok", "warn" or "fail"
        std::string detail;
    };

    void exercise(const Observation& observation, Step& step);
    void next(Phase phase);
    void tap(Button button, unsigned long holdFrames, Step& step);
    void check(const std::string& name, const char* status, const std::string& detail);
    void finish(Result result, const std::string& reason, Step& step);
    void note(const std::string& line) { mLog.push_back(line); }

    struct Release {
        unsigned long frame;
        Button button;
    };

    Driver mBoot;
    StageConfig mConfig;
    unsigned long mFrameLimit;
    unsigned long mFrame = 0;
    unsigned long mPhaseFrames = 0;
    Phase mPhase = Phase::Boot;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    std::vector<std::string> mSeen;  // milestones
    std::vector<Check> mChecks;
    std::string mLastScene, mLastStage;
    bool mDemoStarted = false;       // FileSelector.DemoStartWait seen
    unsigned long mReadyFrames = 0;
    unsigned long mTalkTapAt = 0;
    unsigned long mTalkFrames = 0;
    unsigned long mLoadStartFrame = 0, mReadyFrame = 0;
    int mStep = 0;                   // walk direction / camera direction index
    float mStartX = 0, mStartY = 0, mStartZ = 0;
    float mCamZx = 0, mCamZy = 0, mCamZz = 1;
    bool mCamTriggerSeen = false;   // the game saw this step's D-pad trigger
    bool mCamRoundAllowed = false;  // the camera allowed rotation when it did
    bool mLeftGround = false;
    float mMaxRise = 0;
    int mWalksOk = 0;
    bool mPausePressed = false;
    unsigned long mPauseOpenCount = 0, mPauseCloseCount = 0;
    unsigned long mPhysicalBase = 0;  // physical gameplay inputs when the stage was entered
    unsigned long mPhysicalSeen = 0;
};

}  // namespace PetariNative::App::Smoke

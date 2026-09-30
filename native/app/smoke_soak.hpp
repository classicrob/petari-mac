#pragma once
// Long-session soak script (PETARI_SMOKE=soak), for native/tools/soak.py.
// Requires --test-fixture stage (as the stage script): each cycle's file load
// enters a stage through the synthetic after-loading galaxy move.
//
// One cycle, all through the game's own menus and scene changes, in process:
// 1. The stage script (smoke_stage.hpp) from the title: file select, load,
//    stage ready, walks, jump, spin, camera, pause/resume, idle tail.
// 2. Pause (hold Plus), "Back to the Comet Observatory" (PauseMenu.Back),
//    confirm Yes: the game's stage exit to the observatory.
// 3. The observatory ready (Mario present, pausing permitted), idle and a
//    short walk.
// 4. Pause, "End Game" (PauseMenu.Back in the observatory): the game's save
//    sequence (Yes to saving, the NAND write), confirm Yes: back to the title
//    in process (requestChangeSceneTitle).
// Then the next cycle with the next stage of PETARI_SOAK_STAGES
// ("Stage:scenario,..."; default PETARI_STAGE:PETARI_SCENARIO), until
// PETARI_SOAK_MINUTES (default 60) have passed at a cycle boundary: PASS.
// Yes/no prompts in steps 2-4 are answered Yes, blocking ones (saving) waited
// out, key prompts tapped. Talks are tapped through. FAIL: any stage cycle
// failing (its reason), a menu or target not appearing in time, the scene not
// changing as the menu requested, Mario dying outside the stage cycle.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "smoke.hpp"
#include "smoke_stage.hpp"

namespace PetariNative::App::Smoke {

struct SoakConfig {
    struct Entry {
        std::string stage;
        int scenario = 1;
    };
    std::vector<Entry> stages;
    double minutes = 60;
    unsigned long stageTailFrames = 600;
    unsigned long observatoryIdleFrames = 300;
    // Sets the synthetic entry the next file load uses (TestFixture::stage).
    void (*setStage)(const std::string& stage, int scenario) = nullptr;
};

// PETARI_SMOKE=soak: reads PETARI_SOAK_STAGES (or PETARI_STAGE and
// PETARI_SCENARIO), PETARI_SOAK_MINUTES, PETARI_STAGE_IDLE_FRAMES. False (with
// a message) when soak is not selected or no stage is given.
bool soakEnabledFromEnvironment(SoakConfig* config);

class SoakDriver {
public:
    SoakDriver(unsigned long cycleFrameLimit, const SoakConfig& config);

    // Once per frame, at the seam (same contract as Driver).
    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    const char* phase() const;
    const std::vector<std::string>& log() const { return mLog; }
    bool wantsPlayer() const { return mPhase != Phase::Stage || mStage->wantsPlayer(); }
    int cycle() const { return mCycle; }

private:
    enum class Phase { Stage, ExitStage, Observatory, EndGame, WaitTitle, Done };
    // Steps of a menu exit (ExitStage, EndGame).
    enum class MenuStep { OpenPause, WaitPause, PressBack, Answer };

    void startCycle(const Observation& observation, Step& step);
    bool menuExit(const Observation& observation, const char* toStage, Step& step);
    bool aimAndPress(const Observation& observation, const char* id, Step& step);
    bool handleTalk(const Observation& observation, Step& step);
    void tap(Button button, unsigned long holdFrames, Step& step);
    void next(Phase phase);
    void finish(Result result, const std::string& reason, Step& step);
    void note(const std::string& line) { mLog.push_back(line); }
    unsigned long seenCount(const char* milestone) const;

    struct Release {
        unsigned long frame;
        Button button;
    };

    SoakConfig mConfig;
    unsigned long mCycleFrameLimit;
    std::unique_ptr<StageDriver> mStage;
    std::chrono::steady_clock::time_point mStart;
    unsigned long mFrame = 0;
    unsigned long mPhaseFrames = 0;
    Phase mPhase = Phase::Stage;
    MenuStep mMenu = MenuStep::OpenPause;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    std::vector<std::string> mSeen;  // milestones since the stage cycle ended
    int mCycle = 1;
    std::size_t mEntry = 0;
    unsigned long mPauseOpenBase = 0;
    bool mAnswering = false;          // a yes/no prompt is up: aim at Prompt.Yes
    unsigned long mAimFrames = 0, mPointingFrames = 0, mAimMissing = 0;
    unsigned long mReadyFrames = 0, mTalkTapAt = 0;
    std::string mLastStage;
    std::string mExitFrom;  // the stage a menu exit started on
    int mKeyPromptTaps = 0;     // a key prompt ("saved") is up: taps left
    unsigned long mKeyTapAt = 0;
};

}  // namespace PetariNative::App::Smoke

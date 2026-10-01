#pragma once

// PETARI_SMOKE=replay: a recorded mission as a regression test.
//
// A person's playtest recording (PETARI_ROUTE_RECORD) is split into one route
// per stage visit by native/tools/recording_segments.py. This script enters the
// recorded stage and scenario directly (PETARI_STAGE, PETARI_SCENARIO, with
// --test-fixture stage, like the stage sweep) and follows the route
// (PETARI_REPLAY_ROUTE, x,y,z,action rows: Walk, Hop, Jump, Spin, Kick, Launch,
// Warp) with the game's own stick mapping (stickKeysForWorld). Talk pages,
// first-time notices and key prompts get A, yes/no prompts Yes, as a player
// would; nothing moves Mario but the pad. After a death it resumes from the
// route point nearest the respawn.
//
// PASS only when the game reports PowerStar.Get (or GrandStar.Get) and then the
// return reaches the observatory (AstroGalaxy or AstroDome) and stays playable;
// save prompts on the way are answered Yes. Anything else is a FAIL with its
// reason: route stuck, frame limit, an unexpected stage, no return.

#include <string>
#include <vector>

#include "smoke.hpp"
#include "smoke_domes.hpp"  // DomeWaypoint: the route row format

namespace PetariNative::App::Smoke {

struct ReplayConfig {
    std::string stage;
    int scenario = 0;
    std::string routePath;
    std::vector<DomeWaypoint> route;
};

// PETARI_SMOKE=replay with PETARI_STAGE, PETARI_SCENARIO and PETARI_REPLAY_ROUTE
// (a readable route with at least two points); false (with a message) otherwise.
bool replayEnabledFromEnvironment(ReplayConfig* config);
// Parses x,y,z,action rows; unknown actions are Walk.
std::vector<DomeWaypoint> parseReplayRoute(const std::string& text);

class ReplayDriver {
public:
    ReplayDriver(unsigned long frameLimit, const ReplayConfig& config);

    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    const char* phase() const;
    const std::vector<std::string>& log() const { return mLog; }
    bool wantsPlayer() const { return mPhase != Phase::Boot || mBoot.wantsPlayer(); }
    size_t waypoint() const { return mWaypoint; }

private:
    enum class Phase { Boot, Route, Launch, Warp, Return, Done };
    struct Release {
        unsigned long frame;
        Button button;
    };

    void route(const Observation& o, Step& step);
    void launchOrWarp(const Observation& o, Step& step);
    void returnToObservatory(const Observation& o, Step& step);
    bool interactions(const Observation& o, Step& step);  // talk, prompts, notices, demos; true when handled
    void steerToward(const Observation& o, float x, float y, float z, Step& step);
    void steer(const StickKeys& keys, Step& step);
    void tap(Button button, unsigned long holdFrames, Step& step);
    void resync(const Observation& o, size_t from, size_t to, const char* why);
    void advance(const char* why, const Observation& o);
    void next(Phase phase);
    void finish(Result result, const std::string& reason, Step& step);
    void note(const std::string& line) { mLog.push_back(line); }

    Driver mBoot;
    ReplayConfig mConfig;
    unsigned long mFrameLimit;
    unsigned long mFrame = 0, mPhaseFrames = 0, mWaypointFrames = 0;
    Phase mPhase = Phase::Boot;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    StickKeys mHeld;
    size_t mWaypoint = 0;
    float mBestDistance = 1e30f;
    unsigned long mStuckFrames = 0;
    int mRecoveries = 0, mResyncs = 0;
    bool mAwaitLanding = false;
    unsigned long mSpinAt = 0, mKickAt = 0, mKickSettle = 0, mKickGrounded = 0;
    int mKickRetries = 0;
    float mLastX = 0, mLastY = 0, mLastZ = 0;
    float mFromX = 0, mFromY = 0, mFromZ = 0;  // where a Launch or Warp began
    bool mWasDead = false;
    int mLastLife = -1;
    unsigned long mLastSpin = 0;
    int mDeaths = 0;
    unsigned long mDemoFrames = 0, mLastA = 0, mTalkTapAt = 0;
    std::string mNotice;
    unsigned long mNoticeFrame = 0, mNoticeTapFrame = 0;
    int mNoticeTaps = 0;
    std::string mPrompt;  // a yes/no prompt's Yes target to point at
    unsigned long mAimFrames = 0, mPointingFrames = 0;
    unsigned long mPromptKeyAt = 0;
    bool mStar = false;
    unsigned long mReadyFrames = 0;
    std::string mLastStage;
};

}  // namespace PetariNative::App::Smoke

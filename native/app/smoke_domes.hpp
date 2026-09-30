#pragma once
// Dome tour (PETARI_SMOKE=domes), for a copy of an unlocked save
// (native/SAVES.md, build/saves/all-missions): every galaxy of one observatory
// dome through the game's real UI, as a player reaches it. No fixture and no
// synthetic entry: the saved file's own progression opens everything.
//
// 1. Boot: the "reload" script (smoke.hpp) through the title, file select, the
//    lowest non-empty file and Start, until the observatory (AstroGalaxy) is
//    playable. FAIL if the file loads anywhere else.
// 2. Walk to dome PETARI_DOME (1 Terrace, 2 Fountain, 3 Kitchen, 4 Bedroom,
//    5 Engine Room, 6 Garden): a calibration (stick up, then right, 20 frames
//    each) learns how the camera axes relate to the stick, then each frame
//    steers with 8-way keys toward the next waypoint of the dome's route
//    (planned offline from the game's own map collision,
//    native/tools/observatory_routes.py). Warp waypoints stop on a warp pod
//    and wait for it to carry Mario to its pair. Stuck (less than 30 units
//    closer in 180 frames): a jump, at most 3 times per waypoint.
// 3. In the dome (AstroDome, scenario = PETARI_DOME): gameplay ready, point at
//    the Blue Star (Dome.BlueStar) and press A; on the galaxy map, record the
//    selectable Galaxy.* targets (the dome's galaxy list), then for each in
//    turn: Galaxy.<name>, Galaxy.Start, the mission star (Scenario.Star,
//    mission 1 unless PETARI_DOME_MISSIONS names others), and wait for that
//    stage and selected scenario to load and become ready (Mario present, no
//    demo, pausing permitted, 60 frames; talks tapped through). A short
//    movement check (stick up 45 frames) is reported, not judged. Then pause,
//    "Back to the Comet Observatory" (PauseMenu.Back), Yes, and back in the
//    dome for the next galaxy.
//    PETARI_DOME_MISSIONS="Galaxy:scenario,..." adds missions after the
//    mission-1 sweep (e.g. a comet or hidden star the file owns).
// PETARI_DOME=7 uses a Grand Finale save and an explicit PETARI_DOME_ROUTE
// CSV: Launch waypoints use jump/spin, Talk uses the Luma's visible dialogue
// controls. It then selects Grand Finale mission 1 and returns to AstroGalaxy.
// PASS: every galaxy on the dome's map (and every extra mission) was selected
// through the UI, loaded with the requested scenario and became ready. Each
// visit is logged as "DOMES VISIT dome D galaxy G scenario S: PASS|FAIL ...".
// FAIL: a route, target, load or ready timeout, a wrong stage or scenario,
// Mario dying, a system prompt other than the exit confirmation, the frame
// limit. Missing layout/sound references fail the run at the seam (as for
// every script); the runner attributes them to visits from the log order.

#include <string>
#include <vector>

#include "smoke.hpp"

namespace PetariNative::App::Smoke {

struct DomesConfig {
    int dome = 1;
    struct Mission {
        std::string galaxy;
        int scenario = 1;
    };
    std::vector<Mission> extras;
};

// PETARI_SMOKE=domes: reads PETARI_DOME (1..6) and PETARI_DOME_MISSIONS. False
// (with a message) when domes is not selected or the dome is invalid.
bool domesEnabledFromEnvironment(DomesConfig* config);

// A route point in AstroGalaxy. Warp: stop here and wait for the pod to carry
// Mario to its pair (the next point). Jump: on arrival, jump toward the next
// point with a spin near the apex (a ledge too high to walk up). Kick: a wall
// kick at this wall contact, after a Hop or Kick; A is pressed when Mario
// clings to the wall there (recorded routes, native/tools/recorded_route.py).
struct DomeWaypoint {
    enum Action { Walk, Warp, Jump, Launch, Talk, Hop, Spin, Kick };
    float x, y, z;
    Action action;
};
// The route from the file-load start to dome `dome` (1..6), or empty: the
// live-verified one (smoke_domes_verified_routes.cpp) when there is one, else
// the generated plan (smoke_domes_routes.cpp, native/tools/observatory_routes.py).
const std::vector<DomeWaypoint>& domeRoute(int dome);
const std::vector<DomeWaypoint>& verifiedDomeRoute(int dome);
const std::vector<DomeWaypoint>& plannedDomeRoute(int dome);

class DomesDriver {
public:
    DomesDriver(unsigned long frameLimit, const DomesConfig& config);

    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    const char* phase() const;
    const std::vector<std::string>& log() const { return mLog; }
    bool wantsPlayer() const { return mPhase != Phase::Boot || mBoot.wantsPlayer(); }

private:
    enum class Phase {
        Boot, Observatory, Calibrate, Route, Warp, Launch, Talk, EnterDome,
        DomeReady, BlueStar, GalaxyMap, Confirm, Scenario, Load, Ready, Move,
        PauseOpen, PauseBack, Answer, ReturnDome, Done
    };
    struct Release {
        unsigned long frame;
        Button button;
    };
    struct Visit {
        std::string galaxy;
        int scenario = 1;
        std::string status;  // "PASS" or "FAIL ..."
        unsigned long loadFrames = 0, readyFrames = 0;
        float moved = 0;
    };

    void next(Phase phase);
    void tap(Button button, unsigned long holdFrames, Step& step);
    void steer(const StickKeys& keys, Step& step);
    bool aimAndPress(const Observation& o, const std::string& id, int index, Step& step);
    bool handleTalk(const Observation& o, Step& step);
    bool gameplayReady(const Observation& o);
    void route(const Observation& o, Step& step);
    void dome(const Observation& o, Step& step);
    void finish(Result result, const std::string& reason, Step& step);
    void note(const std::string& line) { mLog.push_back(line); }
    unsigned long seenCount(const char* milestone) const;
    const Visit* current() const { return mVisit < mVisits.size() ? &mVisits[mVisit] : nullptr; }
    void closeVisit(const std::string& status);

    Driver mBoot;
    DomesConfig mConfig;
    unsigned long mFrameLimit;
    unsigned long mFrame = 0;
    unsigned long mPhaseFrames = 0;
    Phase mPhase = Phase::Boot;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    std::vector<std::string> mSeen;
    std::string mLastScene, mLastStage;
    bool mDemoStarted = false;
    StickKeys mHeld;
    // route
    float mSignForward = 0, mSignRight = 0;
    float mCalX = 0, mCalY = 0, mCalZ = 0;
    size_t mWaypoint = 0;
    float mBestDistance = 1e30f;
    unsigned long mStuckFrames = 0;
    int mRecoveries = 0;
    float mWarpX = 0, mWarpY = 0, mWarpZ = 0;
    bool mAwaitJumpLanding = false;
    unsigned long mSpinAt = 0;  // frame of the spin that follows a route jump
    float mLastX = 0, mLastY = 0, mLastZ = 0;  // Mario's position on the previous route frame
    unsigned long mKickGrounded = 0;           // frames grounded while a wall kick is due
    int mKickRetries = 0;                      // wall-kick chain retries since the last walked point
    // dome
    std::vector<Visit> mVisits;
    bool mMapRecorded = false;
    size_t mVisit = 0;
    unsigned long mReadyFrames = 0;
    unsigned long mTalkTapAt = 0;
    unsigned long mAimFrames = 0, mAimMissing = 0, mPointingFrames = 0;
    unsigned long mPauseOpenBase = 0;
    float mMoveX = 0, mMoveY = 0, mMoveZ = 0;
    bool mAnswering = false;
    bool mTalkWasActive = false;
    unsigned long mPhysicalBase = 0;
};

}  // namespace PetariNative::App::Smoke

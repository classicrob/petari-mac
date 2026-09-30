#pragma once
// Good Egg Galaxy mission 1 ("Dino Piranha") played end to end
// (PETARI_SMOKE=goodegg1), with bound inputs only: stick keys, A, the Shake
// binding (spin) and the pointer for the save prompt.
//
// Entry: the "galaxy" script (smoke.hpp) from the title through the
// observatory, the Terrace and the galaxy/mission selection UI, until Good Egg
// mission 1 is loaded and ready (Driver::galaxyMissionReady). With the stage
// fixture (--test-fixture stage, PETARI_STAGE=EggStarGalaxy PETARI_SCENARIO=1)
// the "reload" boot is used instead and the mission starts from the synthetic
// entry: faster iteration, but the result then says "synthetic entry" and does
// not show the observatory route.
//
// The mission is steered from observations only: Mario's position, gravity
// and the camera axes, and the actors the game publishes
// (Game/Util/NativeActorObserve.hpp). Nothing teleports Mario, sets a flag or
// skips a cutscene. The planet Mario stands on is recognised by position (the
// stage placement, see kPlanets in smoke_goodegg.cpp), and that planet's
// objective is pursued, so a restart or a skipped step replans rather than
// following a fixed script:
//   Disk Garden   walk the surface (the stage's own rail over the rim and
//                 down the stem) to the Luma, talk to it (A, then A per page),
//                 spin at the Sling Star it becomes; the Launch Star below
//                 captures Mario: spin.
//   Peanut        collect the five Star Chips (jump for high ones), then spin
//                 at the Launch Star they form.
//   Bean B        spin the Piranha Plant; spin into the vine it leaves; spin
//                 to climb (the vine throws Mario to the next planet).
//   Fruit Peel    hit the Hammer Head Piranha when its head is down (jump on
//                 it or spin); climb the vine it leaves.
//   Bean C        spin the crystal; spin at the Launch Star inside.
//   Dino Piranha  spin the ball at the end of its tail from behind, four
//                 times (egg, then three levels); touch the Power Star.
// Common rules: A every 45 frames while a talk page is shown; a spin whenever
// a ready or capturing Launch Star is within reach, while Mario hangs on a
// vine, or a hostile enemy is close; no input while Mario is bound, a demo
// runs or he is not in the Game scene.
// After the star: the star-get sequence plays by itself; in the Terrace the
// save prompt System_Save00 gets Prompt.Yes and System_Save02 an A.
// PASS needs, in this order: PowerStar.Get while in EggStarGalaxy scenario 1;
// the AstroDome (or AstroGalaxy) stage; the loaded file recording the star
// (GameDataFunction::hasPowerStar) with one star more than at mission start;
// System_Save00 answered and System_Save02 shown, then the save-data sequence
// idle for 60 frames; Mario controllable there for 60 frames.
// Deaths: the game's miss sequence restarts the mission; the driver waits
// until Mario stands again and plans from scratch, as a player retries. The
// PASS reason counts the deaths.
// FAIL: a third death, a planet objective or the whole mission times out, a
// wrong stage, an unknown prompt (BLOCKED), the frame limit. Physical gameplay
// input from the first frame makes a PASS "ASSISTED".

#include <string>
#include <vector>

#include "smoke.hpp"

namespace PetariNative::App::Smoke {

struct GoodEggConfig {
    bool synthetic = false;  // the stage fixture's entry instead of the galaxy route
};

// PETARI_SMOKE=goodegg1. Synthetic when PETARI_STAGE is set (the app has
// validated --test-fixture stage); it must name EggStarGalaxy scenario 1.
bool goodEggEnabledFromEnvironment(GoodEggConfig* config);

// A point of the mission in world coordinates.
struct Point3 {
    float x, y, z;
};

// The planets of mission 1, for tests and logs.
enum class Planet { None, DiskGarden, Peanut, BeanB, FruitPeel, BeanC, Dino, Other };
const char* planetName(Planet planet);
// The planet whose surface is nearest the point, if within its reach.
Planet planetAt(const Point3& point);
// The walk over the Disk Garden (the stage's rail from the top to the stem's
// bottom), up the Fruit Peel's spiral to the Hammer Head, and from Bean C's
// underside to its crystal.
const std::vector<Point3>& diskGardenRoute();
const std::vector<Point3>& fruitPeelRoute();
const std::vector<Point3>& beanCRoute();

// Keys for a world direction: projected onto the plane across `up`, then onto
// the camera's right and screen-up (camera forward plus camera up, both across
// the ground). Empty when the direction or the axes are degenerate.
StickKeys stickKeysForWorld(const Observation& observation, const Point3& direction);

class GoodEggDriver {
public:
    GoodEggDriver(unsigned long frameLimit, const GoodEggConfig& config);

    // Once per frame, at the seam (same contract as Driver).
    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    const char* phase() const;
    const std::vector<std::string>& log() const { return mLog; }
    bool wantsPlayer() const { return mPhase != Phase::Boot || mBoot.wantsPlayer(); }

private:
    enum class Phase { Boot, Mission, StarGet, Return, Done };

    void mission(const Observation& observation, Step& step);
    void starGet(const Observation& observation, Step& step);
    void returnToDome(const Observation& observation, Step& step);
    // Planet objectives; each steers or acts. Returns nothing: a finish ends it.
    void diskGarden(const Observation& observation, Step& step);
    void peanut(const Observation& observation, Step& step);
    void beanB(const Observation& observation, Step& step);
    void fruitPeel(const Observation& observation, Step& step);
    void beanC(const Observation& observation, Step& step);
    void dino(const Observation& observation, Step& step);

    // Walks toward a point; stuck recovery (jump, then sidestep). True within
    // radius: in 3D, or across the ground only (flat) for things that float
    // above it.
    bool goTo(const Observation& observation, const Point3& target, float radius, const char* what, Step& step,
              bool flat = false);
    // Follows a surface route from the point nearest Mario (chosen again when
    // he is far from it, after a knock-back or a fall); true past its end.
    bool followRoute(const Observation& observation, const std::vector<Point3>& route, const char* name, Step& step);
    void steer(const StickKeys& keys, Step& step);
    void tap(Button button, unsigned long holdFrames, Step& step);
    bool spin(Step& step, const char* why);  // taps the Shake binding unless it did recently
    void finish(Result result, const std::string& reason, Step& step);
    void note(const std::string& line) { mLog.push_back(line); }
    void notePhysical(const Observation& observation);
    void resetStuck();
    bool seen(const char* milestone) const;
    // Boulders (rolling along the Peanut's rails): predicts them and Mario's
    // short candidate moves and braking 90 frames ahead; when the goal path passes
    // within 500 of one, steers the safe move nearest the goal's direction
    // (or waits, or keeps farthest away); true while doing so.
    bool dodgeRocks(const Observation& observation, Step& step);

    struct Release {
        unsigned long frame;
        Button button;
    };

    Driver mBoot;
    GoodEggConfig mConfig;
    unsigned long mFrameLimit;
    unsigned long mFrame = 0;
    unsigned long mPhaseFrames = 0;
    Phase mPhase = Phase::Boot;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    std::vector<std::string> mSeen;  // milestones since the mission started
    StickKeys mHeld;
    std::string mLastScene, mLastStage;
    // mission
    Planet mPlanet = Planet::None;
    unsigned long mPlanetFrames = 0;   // frames on the current planet
    unsigned long mMissionFrames = 0;
    int mStarsAtStart = -1;
    size_t mWaypoint = 0;
    bool mWaypointChosen = false;
    unsigned long mLastSpin = 0;
    unsigned long mLastHammerSpin = 0;
    unsigned long mLastA = 0;
    unsigned long mTalkFrames = 0;     // frames with a talk shown (since asking the Luma)
    bool mLumaAsked = false;           // A pressed next to the Disk Garden Luma
    bool mTalked = false;              // the Disk Garden Luma's talk ended
    unsigned long mAirFrames = 0;
    unsigned long mAirTrace = 0;
    unsigned long mReleaseTrace = 0;          // frames of per-frame trace left after a bind
    Point3 mLastPos{0.0f, 0.0f, 0.0f};
    Point3 mLastMove{0.0f, 0.0f, 0.0f};  // Mario's move over the last frame
    bool mReleased = false;  // was bound (launch star, vine): no steering until Mario lands
    unsigned long mReleasedFrames = 0;
    int mReleaseExperiment = 0;  // PETARI_GOODEGG_RELEASE: 0 none, 1 spin, 2 away
    int mLastLife = -1;
    bool mPeanutToured = false;
    int mDeaths = 0;              // deaths so far (the game restarts; more than kMaxDeaths FAILs)
    bool mDying = false;          // dead, waiting for the restart
    unsigned long mRespawnFrames = 0;
    size_t mChipBase = 0;         // milestones before the current attempt (chips reset on a restart)
    Point3 mGoalPoint{0.0f, 0.0f, 0.0f};  // goTo's latest target (boulder waits look at it)
    bool mHasGoal = false;
    unsigned long mRockLogAt = 0;
    float mBestDistance = 1e30f;
    unsigned long mStuckFrames = 0;
    int mRecoveries = 0;
    unsigned long mSidestepUntil = 0;
    std::string mGoal;                 // what goTo walks to, for logs
    unsigned long mLogAt = 0;
    int mDinoHits = 0;
    int mLastDinoPhase = -1;
    std::vector<Point3> mChipsDone;    // static chip spots seen collected
    // after the star
    bool mStarGot = false;
    bool mSaveAnswered = false;        // System_Save00 answered Yes
    bool mSaveShown = false;           // System_Save00 seen
    bool mSaveDone = false;            // System_Save02 seen
    std::string mPrompt;               // prompt being answered with the pointer
    unsigned long mAimFrames = 0, mPointingFrames = 0;
    unsigned long mIdleSaveFrames = 0;
    unsigned long mReadyFrames = 0;
    // physical input
    bool mPhysicalBaseSet = false;
    PhysicalInputs mPhysicalSeen;
    unsigned long mAssistInputs = 0, mPointerInputs = 0, mFocusInputs = 0;
    std::string mFirstAssist;
};

}  // namespace PetariNative::App::Smoke

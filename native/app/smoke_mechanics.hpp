#pragma once
// Mechanics checks for the stage script (smoke_stage.hpp): PETARI_SMOKE=stage with
// PETARI_MECHANIC=<name> replaces the stage script's generic exercise (idle, walk,
// jump, spin, camera, pause) with one special mechanic, checked by its physically
// expected outcome. Inputs are the bound pad actions only (stick keys, A, the Shake
// binding); the Star Ball's tilt comes from the stick keys through the input layer's
// ride-steering hint. Observations only: nothing moves Mario or sets game state.
//
// Each mechanic names its stage and scenario (the stage fixture's synthetic entry,
// Mario at start 0) and the placement points it uses, taken offline from the stage
// data (build/collision-tools/galaxy.py):
//
//   starball  TamakoroExLv2Galaxy 1. Walk to the Star Ball (the published "StarBall"
//             actor), jump onto it until it reports Mario riding. After the tutorial
//             talk, four 25-frame tilts: forward, back (brake), right, left. Each must
//             push the ball at least 50 units along the tilt beyond coasting at its
//             starting speed (ground friction 0.99/frame; the game adds 0.4 x the tilt
//             rate per frame, ~112 units at full tilt), and Mario must stay on. Short
//             tilts with a brake keep the ball on the start area: a 120-frame full
//             tilt reaches ~28 units/frame and rolls off it (first live run).
//   bee       HoneyBeeExGalaxy 1. Walk into the Bee Mushroom: player mode Bee. Hold A
//             60 frames from the ground: Mario still rises between frames 20 and 60
//             (a plain jump would be falling by then). Fly into the honeycomb wall
//             (wall code Fur, facing +z at z -1257): he sticks to it (bee wall walk);
//             hold the stick 60 frames: he moves at least 60 units while stuck.
//   swim      OceanRingGalaxy 2. Walk toward the shell in the ring's water until
//             Mario's status is Swim. Stroke (A every 20 frames) with the stick held
//             120 frames: at least 150 units while he keeps swimming.
//   ray       SurfingLv1Galaxy 1. Walk the beach (+z, about 6750) to the Ray (the
//             published "Ray" actor) and jump onto it until it reports its rider.
//             After the tutorial: level for 120 frames, it moves at least 300 along
//             its front, on the water; twist right (the right stick key) 90 frames,
//             its heading turns at least 20 degrees to the right; tap A, it leaves
//             the water and is back on it within 150 frames, still ridden.
//   flippanel FlipPanelExGalaxy 1. Walk onto the nearest panels ("FlipPanel" actors)
//             that are not flipped: each one Mario stands on must flip within 60
//             frames; two panels.
//
// FAIL names the step and the measurements. Every step is logged with positions.
#include <string>
#include <vector>
#include "smoke.hpp"

namespace PetariNative::App::Smoke {

struct MechanicPlan {
    const char* name;
    const char* stage;
    int scenario;
};
// The known mechanics, for PETARI_MECHANIC and the run script.
const std::vector<MechanicPlan>& mechanicPlans();
const MechanicPlan* findMechanic(const std::string& name);

// One mechanic's run, stepped by StageDriver once gameplay is ready.
class MechanicRun {
public:
    explicit MechanicRun(const MechanicPlan& plan) : mPlan(plan) {}
    // Adds presses to step. Returns Running until decided; then reason() says why.
    Result step(const Observation& observation, unsigned long frame, Step& step);
    const std::string& reason() const { return mReason; }
    const std::vector<std::string>& log() const { return mLog; }
    void clearLog() { mLog.clear(); }
    // Releases every key this run holds (the stage driver calls it when finishing).
    void releaseAll(Step& step);

    struct V {
        float x, y, z;
    };

private:
    Result starBall(const Observation& o, Step& step);
    Result bee(const Observation& o, Step& step);
    Result swim(const Observation& o, Step& step);
    Result flipPanel(const Observation& o, Step& step);
    Result ray(const Observation& o, Step& step);
    // steerTo with stuck recovery: no 40 units of progress in 120 frames jumps; the
    // third time on one target sidesteps for 45 frames. Returns the flat distance.
    float approach(const Observation& o, V target, Step& step);

    // Holds exactly these keys (presses for the changes).
    void hold(bool up, bool down, bool left, bool right, bool a, Step& step);
    // Steers toward a world point across the ground plane; returns the flat distance.
    float steerTo(const Observation& o, V target, Step& step, bool a = false);
    void tapA(Step& step);
    Result fail(const std::string& why);
    Result pass(const std::string& why);
    void next(int stage);
    void note(const std::string& line) { mLog.push_back(line); }

    const MechanicPlan& mPlan;
    std::string mReason;
    std::vector<std::string> mLog;
    unsigned long mFrame = 0;
    int mStage = 0;
    unsigned long mStageFrames = 0;
    int mAttempts = 0;
    bool mSpinHeld = false;
    bool mHeld[5] = {};        // up, down, left, right, A
    unsigned long mReleaseA = 0;
    // Measurements.
    V mMark{0, 0, 0};
    V mMark2{0, 0, 0};
    V mAxis{0, 0, 0};
    float mValue = 0, mValue2 = 0;
    float mBestDistance = 0;
    unsigned long mBestFrame = 0;
    int mCount = 0;
    int mTarget = -1;
    std::vector<V> mDone;
    float mApproachBest = 0;
    unsigned long mApproachBestFrame = 0;
    int mStuck = 0;
    unsigned long mSidestepUntil = 0;
};

}  // namespace PetariNative::App::Smoke

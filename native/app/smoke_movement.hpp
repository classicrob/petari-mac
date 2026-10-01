#pragma once

// PETARI_SMOKE=movement: measures Mario's movement in the game with scripted
// inputs, for the OdysseyMovement mod (docs/dev/ODYSSEY_MOVEMENT.md, section 6).
//
// Loads a saved file like the reload script (use an observatory test fixture:
// the start is flat ground), waits until Mario stands still, then:
//   1. a standstill jump with A held 15 frames, and a tapped one (A for 1 frame);
//   2. a 50-frame run with the stick held, logging the speed each frame;
//   3. a ground-pound jump: a hop, a ground pound at its peak, then A held 15
//      frames on the 8th frame after landing (height from the landing point);
//   4. three jumps with A held 15 frames, each made on the frame after landing,
//      stick still held (the triple-jump chain at full speed);
//   5. a long jump: run, crouch (Z), and jump 3 frames later;
//   6. a backflip: standing, crouch, and jump 8 frames later;
//   7. a sideflip: run, flick the stick back, and jump 4 frames later;
//   8. a dive: a hop, a ground pound at its peak, Spin 4 frames later (height
//      from the point where Spin was pressed);
//   9. a roll: crouch, Spin 3 frames later, Spin again 30 frames into the roll
//      (a boost); logs "MOVEMENT roll: ..." with the ground speed. At the end of
//      the full sequence on the observatory Mario faces a wall, so measure the
//      roll alone (PETARI_MOVEMENT_ONLY=roll) from the start point;
//  10. a wall jump: run (stick PETARI_MOVEMENT_WALL_DIR, default up) until a wall
//      stops Mario, jump against it, slide 20 frames once he hangs on it (logs
//      "MOVEMENT wall slide: ..." with the fall per frame), then A held 15 frames
//      ("MOVEMENT wall jump: apex ..." from the press). Run it alone
//      (PETARI_MOVEMENT_ONLY=wall) where a wall is in reach;
//  11. landings: a long jump and a dive, each landed with the stick held and
//      released; logs "MOVEMENT <name> landing: speed <v> on the landing frame;
//      every 5th frame: ..." over the 30 frames after it.
// With PETARI_XFB_DUMP, each move marks its name as the dump label (for example
// PETARI_XFB_DUMP_LABELS="long jump,dive").
// PETARI_MOVEMENT_ONLY=<name>[,<name>...] keeps only the tasks whose name
// contains one of them.
// Each jump logs "MOVEMENT <name>: apex <height> after <frames> frames, take-off
// speed <s>"; heights are along -gravity from the take-off point. The run logs
// "MOVEMENT run: max speed <v> u/f, frame <n> reached 95%". Compare with the
// model (petari/odyssey_move.hpp) with PETARI_MODS=OdysseyMovement, or with
// Galaxy's own numbers with mods off. PASS when every step completes; the
// numbers are compared by the caller (native/tools/movement_check.py).

#include <string>
#include <vector>

#include "smoke.hpp"

namespace PetariNative::App::Smoke {

bool movementEnabledFromEnvironment();

class MovementDriver {
public:
    explicit MovementDriver(unsigned long frameLimit);

    Step step(const Observation& observation);

    Result result() const { return mResult; }
    const std::string& reason() const { return mReason; }
    unsigned long frame() const { return mFrame; }
    const char* phase() const;
    const std::vector<std::string>& log() const { return mLog; }
    bool wantsPlayer() const { return mPhase != Phase::Boot || mBoot.wantsPlayer(); }

private:
    enum class Phase { Boot, Settle, Jump, Land, Run, Crouch, Hop, Pound, Reverse, Roll, WallRun, WallClimb, WallSlide, Landed, Done };
    struct Release {
        unsigned long frame;
        Button button;
    };
    struct Task {
        const char* name;
        int holdA;        // frames A is held
        bool stick;       // stick held up during it
        int runFrames;    // run before jumping (0: jump at once)
        bool afterLanding;  // jump on the frame after the previous landing
        int crouch = 0;          // frames Z is held before the jump (long jump)
        bool groundPound = false;  // hop, ground pound, then the measured jump
        int reverse = 0;           // after the run, frames the stick is held back before the jump (sideflip)
        bool dive = false;         // with groundPound: Spin during the pound, measured from there
        int roll = 0;              // with crouch: Spin instead of A, then the ground speed is logged for this many frames
        bool wall = false;         // run into a wall, jump against it, slide, wall-jump (measured from the press)
        int land = 0;              // after landing, log the ground speed for this many frames
        bool releaseStick = false; // release the stick at take-off (lands with it released)
        bool airStick = false;     // hold the stick from take-off (lands with it held)
    };

    void finish(Result result, const std::string& reason, Step& step);
    void tap(Button button, unsigned long holdFrames, Step& step);
    void note(const std::string& line) { mLog.push_back(line); }

    Driver mBoot;
    unsigned long mFrameLimit;
    unsigned long mFrame = 0, mPhaseFrames = 0;
    Phase mPhase = Phase::Boot;
    Result mResult = Result::Running;
    std::string mReason;
    std::vector<std::string> mLog;
    std::vector<Release> mReleases;
    std::vector<Task> mTasks;
    size_t mTask = 0;
    unsigned long mReady = 0;
    bool mStickHeld = false;
    float mStartX = 0, mStartY = 0, mStartZ = 0;  // take-off point
    float mUpX = 0, mUpY = 1, mUpZ = 0;           // -gravity at take-off
    float mApex = 0.0f, mTakeoffSpeed = 0.0f;
    unsigned long mApexFrame = 0;
    bool mLeftGround = false;
    float mLastX = 0, mLastY = 0, mLastZ = 0;
    float mMaxSpeed = 0.0f;
    Button mWallStick = Button::StickUp;  // PETARI_MOVEMENT_WALL_DIR: up, down, left, right
    float mFallLast = 0.0f;
    std::vector<float> mSpeeds;
};

}  // namespace PetariNative::App::Smoke

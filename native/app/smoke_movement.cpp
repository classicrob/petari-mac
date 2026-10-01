#include "smoke_movement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <petari/efb_dump_mark.hpp>

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kTapFrames = 6;
constexpr unsigned long kReadyFrames = 60;
constexpr unsigned long kStandStill = 30;      // grounded, not moving, before a standstill jump
constexpr unsigned long kTaskLimit = 900;      // frames per task
constexpr float kStillSpeed = 0.05f;

struct V {
    float x, y, z;
};
V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(V a) { return std::sqrt(dot(a, a)); }
V scale(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
std::string num(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", v);
    return text;
}

}  // namespace

bool movementEnabledFromEnvironment() {
    const char* smoke = std::getenv("PETARI_SMOKE");
    return smoke != nullptr && std::strcmp(smoke, "movement") == 0;
}

MovementDriver::MovementDriver(unsigned long frameLimit) : mBoot(frameLimit + 1, Script::Reload), mFrameLimit(frameLimit) {
    mTasks = {
        {"standstill held jump", 15, false, 0, false},
        {"standstill tap jump", 1, false, 0, false},
        {"ground-pound jump", 15, false, 0, false, 0, true},
        {"running jump 1", 15, true, 50, false},
        {"chain jump 2", 15, true, 0, true},
        {"chain jump 3", 15, true, 0, true},
        {"long jump", 6, true, 40, false, 3},
        {"dive", 6, false, 0, false, 0, true, 0, true},
        {"backflip", 6, false, 0, false, 8},
        {"sideflip", 6, true, 45, false, 0, false, 2},
        {"roll", 0, false, 0, false, 3, false, 0, false, 120},
        {"wall jump", 15, false, 0, false, 0, false, 0, false, 0, true},
        {"long jump landing (stick held)", 6, true, 40, false, 3, false, 0, false, 0, false, 30},
        {"long jump landing (stick released)", 6, true, 40, false, 3, false, 0, false, 0, false, 30, true},
        {"dive landing (stick held)", 6, false, 0, false, 0, true, 0, true, 0, false, 30, false, true},
        {"dive landing (stick released)", 6, false, 0, false, 0, true, 0, true, 0, false, 30},
        {"roll to the end (stick held)", 0, false, 0, false, 3, false, 0, false, 140, false, 0, false, true, true},
        {"roll to the end (stick released)", 0, false, 0, false, 3, false, 0, false, 140, false, 0, false, false, true},
    };
    if (const char* dir = std::getenv("PETARI_MOVEMENT_WALL_DIR"); dir != nullptr) {
        const std::string d(dir);
        mWallStick = d == "down" ? Button::StickDown : d == "left" ? Button::StickLeft : d == "right" ? Button::StickRight : Button::StickUp;
    }
    // PETARI_MOVEMENT_ONLY=<name>[,<name>...]: only the tasks whose name contains one of them.
    if (const char* only = std::getenv("PETARI_MOVEMENT_ONLY"); only != nullptr && *only != '\0') {
        std::vector<std::string> keep;
        std::string list = only;
        for (size_t start = 0; start <= list.size();) {
            const size_t comma = std::min(list.find(',', start), list.size());
            if (comma > start) keep.push_back(list.substr(start, comma - start));
            start = comma + 1;
        }
        mTasks.erase(std::remove_if(mTasks.begin(), mTasks.end(),
                                    [&](const Task& t) {
                                        return std::none_of(keep.begin(), keep.end(),
                                                            [&](const std::string& k) { return std::strstr(t.name, k.c_str()) != nullptr; });
                                    }),
                     mTasks.end());
    }
}

const char* MovementDriver::phase() const {
    switch (mPhase) {
    case Phase::Boot: return mBoot.phase();
    case Phase::Settle: return "movement: waiting to stand still";
    case Phase::Run: return "movement: running";
    case Phase::Jump: return "movement: jumping";
    case Phase::Crouch: return "movement: crouching before a long jump";
    case Phase::Hop: return "movement: hop before a ground pound";
    case Phase::Pound: return "movement: ground pound";
    case Phase::Reverse: return "movement: stick back before a sideflip";
    case Phase::Roll: return "movement: rolling";
    case Phase::WallRun: return "movement: running to a wall";
    case Phase::WallClimb: return "movement: jumping against the wall";
    case Phase::WallSlide: return "movement: sliding down the wall";
    case Phase::Landed: return "movement: ground speed after landing";
    case Phase::Land: return "movement: landed";
    case Phase::Done: return "movement: done";
    }
    return "movement";
}

void MovementDriver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.presses.push_back({button, true});
    step.assertFocus = true;
    mReleases.push_back({mFrame + holdFrames, button});
}

void MovementDriver::finish(Result result, const std::string& reason, Step& step) {
    mResult = result;
    mReason = reason;
    mPhase = Phase::Done;
    if (mStickHeld) step.presses.push_back({Button::StickUp, false});
    mStickHeld = false;
    note(std::string(result == Result::Pass ? "PASS: " : "FAIL: ") + reason);
    step.requestQuit = true;
}

Step MovementDriver::step(const Observation& o) {
    Step step;
    mLog.clear();
    ++mFrame;
    ++mPhaseFrames;
    for (auto it = mReleases.begin(); it != mReleases.end();) {
        if (it->frame <= mFrame) {
            step.presses.push_back({it->button, false});
            it = mReleases.erase(it);
        } else {
            ++it;
        }
    }
    if (mResult != Result::Running) return step;

    if (mPhase == Phase::Boot) {
        step = mBoot.step(o);
        for (const std::string& line : mBoot.log()) {
            if (line.rfind("physical ", 0) != 0 && line.rfind("window focus", 0) != 0) mLog.push_back(line);
        }
        if (mBoot.result() != Result::Running) {
            mResult = mBoot.result() == Result::Blocked ? Result::Blocked : Result::Fail;
            mReason = std::string("before the measurements: ") + mBoot.reason();
            mPhase = Phase::Done;
            step.requestQuit = true;
            return step;
        }
        const bool ready = o.scene == "Game" && o.sceneReady && o.playerValid && o.playerOnGround && !o.demoActive &&
                           o.pausePermitted && !o.talkActive;
        mReady = ready ? mReady + 1 : 0;
        if (mReady < kReadyFrames) return step;
        step.presses.clear();
        for (Button b : {Button::A, Button::B, Button::StickUp, Button::StickDown, Button::StickLeft, Button::StickRight}) {
            step.presses.push_back({b, false});
        }
        step.requestQuit = false;
        mReleases.clear();
        mPhase = Phase::Settle;
        mPhaseFrames = 0;
        mReady = 0;
        note("movement measurements in " + o.stage + " at (" + num(o.playerX) + ", " + num(o.playerY) + ", " + num(o.playerZ) + ")");
        return step;
    }
    if (mFrame >= mFrameLimit) {
        finish(Result::Fail, std::string("frame limit reached while ") + phase(), step);
        return step;
    }
    if (!o.playerValid) return step;
    if (o.playerDead) {
        finish(Result::Fail, "Mario died during the measurements", step);
        return step;
    }

    const V pos{o.playerX, o.playerY, o.playerZ};
    const V last{mLastX, mLastY, mLastZ};
    mLastX = pos.x; mLastY = pos.y; mLastZ = pos.z;
    V up{-o.gravityX, -o.gravityY, -o.gravityZ};
    const float upLen = len(up);
    up = upLen > 1e-3f ? scale(up, 1.0f / upLen) : V{0, 1, 0};
    const V moved = sub(pos, last);
    const float horizontal = len(sub(moved, scale(up, dot(moved, up))));

    if (mTask >= mTasks.size()) {
        finish(Result::Pass, "movement measured: " + std::to_string(mTasks.size()) + " moves", step);
        return step;
    }
    const Task& task = mTasks[mTask];
    if (mPhaseFrames > kTaskLimit) {
        finish(Result::Fail, std::string(task.name) + " did not finish within " + std::to_string(kTaskLimit) + " frames", step);
        return step;
    }

    const auto startJump = [&](Button button = Button::A) {
        mStartX = pos.x; mStartY = pos.y; mStartZ = pos.z;
        mUpX = up.x; mUpY = up.y; mUpZ = up.z;
        mApex = 0.0f;
        mApexFrame = 0;
        mLeftGround = button != Button::A;  // a dive starts in the air
        mTakeoffSpeed = horizontal;
        tap(button, static_cast< unsigned long >(button == Button::A ? task.holdA : kTapFrames), step);
        PetariNative::EfbDump::mark(task.name);  // opt-in image dump (PETARI_XFB_DUMP); inert otherwise
        if (task.releaseStick && mStickHeld) {
            step.presses.push_back({Button::StickUp, false});
            mStickHeld = false;
        }
        if (task.airStick && !mStickHeld) {
            step.presses.push_back({Button::StickUp, true});
            mStickHeld = true;
        }
        mPhase = Phase::Jump;
        mPhaseFrames = 0;
    };

    switch (mPhase) {
    case Phase::Settle:
    case Phase::Land: {
        if (task.afterLanding && mPhase == Phase::Land) {
            startJump();  // the frame after landing
            break;
        }
        if (task.wall) {
            step.presses.push_back({mWallStick, true});
            step.assertFocus = true;
            mPhase = Phase::WallRun;
            mPhaseFrames = 0;
            mReady = 0;
            break;
        }
        if (task.stick) {
            if (!mStickHeld) {
                step.presses.push_back({Button::StickUp, true});
                step.assertFocus = true;
                mStickHeld = true;
            }
            if (task.runFrames > 0) {
                mPhase = Phase::Run;
                mPhaseFrames = 0;
                mMaxSpeed = 0.0f;
                mSpeeds.clear();
            } else {
                startJump();
            }
            break;
        }
        if (mStickHeld) {
            step.presses.push_back({Button::StickUp, false});
            mStickHeld = false;
        }
        // A standstill jump: grounded and still for a while first.
        mReady = (o.playerOnGround && horizontal < kStillSpeed && std::fabs(dot(moved, up)) < kStillSpeed) ? mReady + 1 : 0;
        if (mReady >= kStandStill) {
            mReady = 0;
            if (task.crouch > 0) {
                // Backflip: crouch, then jump. Roll: crouch, then Spin (Z held longer so
                // the Spin lands inside the crouch whatever the input timing).
                tap(Button::Z, static_cast< unsigned long >(task.crouch + task.holdA + (task.roll > 0 ? 10 : 2)), step);
                mPhase = Phase::Crouch;
                mPhaseFrames = 0;
            } else if (task.groundPound) {
                tap(Button::A, 1, step);  // a hop; the ground pound comes at its peak
                mPhase = Phase::Hop;
                mPhaseFrames = 0;
                mLeftGround = false;
            } else {
                startJump();
            }
        }
        break;
    }
    case Phase::Hop: {
        if (!o.playerOnGround) mLeftGround = true;
        if (mLeftGround && mPhaseFrames > 6 && dot(moved, up) <= 0.0f) {
            tap(Button::Z, kTapFrames, step);
            note("ground pound at the hop's peak");
            mPhase = Phase::Pound;
            mPhaseFrames = 0;
            mReady = 0;
        }
        break;
    }
    case Phase::Pound: {
        if (task.dive) {
            if (mPhaseFrames == 4) startJump(Button::Spin);  // Spin during the pound's wind-up
            break;
        }
        // Count grounded frames after the pound lands; jump on the 8th.
        mReady = o.playerOnGround ? mReady + 1 : 0;
        if (mReady == 8) {
            mReady = 0;
            startJump();
        }
        break;
    }
    case Phase::Crouch: {
        if (static_cast< int >(mPhaseFrames) >= task.crouch) {
            if (task.roll > 0) {
                tap(Button::Spin, 2, step);
                PetariNative::EfbDump::mark(task.name);
                if (task.airStick && !mStickHeld) {
                    step.presses.push_back({Button::StickUp, true});
                    mStickHeld = true;
                }
                mPhase = Phase::Roll;
                mPhaseFrames = 0;
                mSpeeds.clear();
            } else {
                startJump();
            }
        }
        break;
    }
    case Phase::WallRun: {
        // Blocked: the stick is held but Mario barely moves for 10 frames.
        mReady = (mPhaseFrames > 30 && o.playerOnGround && horizontal < 1.0f) ? mReady + 1 : 0;
        if (mReady >= 10) {
            note("wall reached at (" + num(pos.x) + ", " + num(pos.y) + ", " + num(pos.z) + ")");
            tap(Button::A, static_cast< unsigned long >(task.holdA), step);
            mStartX = pos.x; mStartY = pos.y; mStartZ = pos.z;
            mUpX = up.x; mUpY = up.y; mUpZ = up.z;
            mPhase = Phase::WallClimb;
            mPhaseFrames = 0;
            mReady = 0;
            mLeftGround = false;
        } else if (mPhaseFrames > 600) {
            step.presses.push_back({mWallStick, false});
            finish(Result::Fail, "no wall within 600 frames of running", step);
        }
        break;
    }
    case Phase::WallClimb: {
        // On the wall: in the air and falling, then (almost) still along gravity.
        if (!o.playerOnGround) mLeftGround = true;
        const float rise = dot(moved, up);
        if (mLeftGround && o.playerOnGround) {
            step.presses.push_back({mWallStick, false});
            finish(Result::Fail, "landed without hanging on the wall", step);
            break;
        }
        if (dot(sub(pos, V{mStartX, mStartY, mStartZ}), V{mUpX, mUpY, mUpZ}) < -300.0f) {
            step.presses.push_back({mWallStick, false});
            finish(Result::Fail, "fell past the take-off point: no wall to hang on", step);
            break;
        }
        if (mLeftGround && mPhaseFrames > 10 && std::fabs(rise) < 0.05f) {
            PetariNative::EfbDump::mark("wall slide");
            mPhase = Phase::WallSlide;
            mPhaseFrames = 0;
            mSpeeds.clear();
        }
        break;
    }
    case Phase::WallSlide: {
        mSpeeds.push_back(-dot(moved, up));
        if (o.playerOnGround) {
            step.presses.push_back({mWallStick, false});
            finish(Result::Fail, "slid to the ground before the wall jump", step);
            break;
        }
        if (mPhaseFrames >= 20) {
            std::string falls;
            for (float f : mSpeeds) falls += (falls.empty() ? "" : " ") + num(f);
            note("MOVEMENT wall slide: fall per frame " + falls);
            step.presses.push_back({mWallStick, false});
            startJump();
            mLeftGround = true;  // measured from the press, in the air
        }
        break;
    }
    case Phase::Landed: {
        mSpeeds.push_back(horizontal);
        if (static_cast< int >(mPhaseFrames) >= task.land) {
            std::string speeds;
            for (size_t i = 0; i < mSpeeds.size(); i += 5) speeds += (speeds.empty() ? "" : " ") + num(mSpeeds[i]);
            note(std::string("MOVEMENT ") + task.name + ": speed " + num(mSpeeds[0]) + " on the landing frame; every 5th frame: " + speeds);
            ++mTask;
            mPhase = Phase::Land;
            mPhaseFrames = 0;
            mReady = 0;
        }
        break;
    }
    case Phase::Roll: {
        if (mPhaseFrames > 1) mSpeeds.push_back(horizontal);
        // A boost (15 frames apart at least; the emulated shake behind Spin repeats
        // about four times a second, so 30 frames after the start).
        if (mPhaseFrames == 30 && !task.noBoost) tap(Button::Spin, 2, step);
        if (static_cast< int >(mPhaseFrames) >= task.roll) {
            float top = 0.0f;
            std::string speeds;
            for (size_t i = 0; i < mSpeeds.size(); ++i) {
                top = std::max(top, mSpeeds[i]);
                if (i % 5 == 0) speeds += (speeds.empty() ? "" : " ") + num(mSpeeds[i]);
            }
            note(std::string("MOVEMENT ") + task.name + ": max speed " + num(top) + " u/f; every 5th frame: " + speeds);
            ++mTask;
            mPhase = Phase::Land;
            mPhaseFrames = 0;
            mReady = 0;
        }
        break;
    }
    case Phase::Reverse: {
        if (mPhaseFrames == 2) {
            step.presses.push_back({Button::StickDown, true});
            step.assertFocus = true;
        }
        if (static_cast< int >(mPhaseFrames) >= 2 + task.reverse) {
            startJump();
            mReleases.push_back({mFrame + static_cast< unsigned long >(task.holdA), Button::StickDown});
        }
        break;
    }
    case Phase::Run: {
        if (mPhaseFrames > 1) {
            mSpeeds.push_back(horizontal);
            mMaxSpeed = std::max(mMaxSpeed, horizontal);
        }
        if (static_cast< int >(mPhaseFrames) >= task.runFrames) {
            int reached = -1;
            for (size_t i = 0; i < mSpeeds.size(); ++i) {
                if (mSpeeds[i] >= 0.95f * mMaxSpeed) {
                    reached = static_cast< int >(i) + 2;
                    break;
                }
            }
            std::string speeds;
            for (size_t i = 0; i < mSpeeds.size(); i += 5) speeds += (speeds.empty() ? "" : " ") + num(mSpeeds[i]);
            note(std::string("MOVEMENT run") + (task.crouch > 0 ? " before the long jump" : "") + ": max speed " + num(mMaxSpeed) +
                 " u/f, frame " + std::to_string(reached) + " reached 95%; every 5th frame: " + speeds);
            if (task.crouch > 0) {
                tap(Button::Z, static_cast< unsigned long >(task.crouch + task.holdA + 2), step);
                mPhase = Phase::Crouch;
                mPhaseFrames = 0;
            } else if (task.reverse > 0) {
                // Release first: Galaxy arms its turn slide only after the stick
                // passes near neutral (mTurnSlipNeutral) while braking.
                step.presses.push_back({Button::StickUp, false});
                mStickHeld = false;
                mPhase = Phase::Reverse;
                mPhaseFrames = 0;
            } else {
                startJump();
            }
        }
        break;
    }
    case Phase::Jump: {
        if (task.wall && mPhaseFrames == 1) {
            // The press frame still slid down the wall: measure from the take-off.
            mStartX = pos.x; mStartY = pos.y; mStartZ = pos.z;
        }
        if (!mLeftGround && o.playerOnGround && mPhaseFrames % 20 == 0 && mPhaseFrames > 0 && mPhaseFrames <= 60) {
            // A 1-frame tap can fall between two game frames; press again.
            note(std::string(task.name) + ": no take-off after " + std::to_string(mPhaseFrames) + " frames, pressing again");
            tap(Button::A, static_cast< unsigned long >(task.holdA), step);
        }
        if (!mLeftGround && o.playerOnGround) {
            // Measure from the last grounded position before take-off (the press
            // frame can still be in a crouch slide).
            mStartX = pos.x; mStartY = pos.y; mStartZ = pos.z;
        }
        const V start{mStartX, mStartY, mStartZ};
        const V upStart{mUpX, mUpY, mUpZ};
        const float height = dot(sub(pos, start), upStart);
        if (height > mApex) {
            mApex = height;
            mApexFrame = mPhaseFrames;
        }
        if (!o.playerOnGround) mLeftGround = true;
        if (mLeftGround && o.playerOnGround) {
            note(std::string("MOVEMENT ") + task.name + ": apex " + num(mApex) + " after " + std::to_string(mApexFrame) +
                 " frames, take-off speed " + num(mTakeoffSpeed) + ", landed after " + std::to_string(mPhaseFrames) + " frames, from (" +
                 num(mStartX) + ", " + num(mStartY) + ", " + num(mStartZ) + ")");
            if (task.land > 0) {
                mSpeeds.assign(1, horizontal);  // the landing frame
                mPhase = Phase::Landed;
                mPhaseFrames = 0;
                break;
            }
            ++mTask;
            mPhase = Phase::Land;
            mPhaseFrames = 0;
            mReady = 0;
        }
        break;
    }
    default:
        break;
    }
    return step;
}

}  // namespace PetariNative::App::Smoke

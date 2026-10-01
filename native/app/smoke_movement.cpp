#include "smoke_movement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    };
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
        finish(Result::Pass, "movement measured: " + std::to_string(mTasks.size()) + " jumps and a run", step);
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
                tap(Button::Z, static_cast< unsigned long >(task.crouch + task.holdA + 2), step);  // backflip: crouch, then jump
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
        if (static_cast< int >(mPhaseFrames) >= task.crouch) startJump();
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

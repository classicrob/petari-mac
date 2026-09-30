// Mechanics checks (smoke_mechanics.hpp). No SDK or Aurora headers: observations in,
// presses out.
#include "smoke_mechanics.hpp"
#include "smoke_goodegg.hpp"
#include <cmath>
#include <cstdio>
#include <limits>

namespace PetariNative::App::Smoke {
namespace {
using V = MechanicRun::V;

V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V scale(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(V a) { return std::sqrt(dot(a, a)); }
V normalized(V a) {
    const float l = length(a);
    return l > 1e-6f ? scale(a, 1.0f / l) : V{0, 0, 0};
}
V position(const Observation& o) { return {o.playerX, o.playerY, o.playerZ}; }
// Up from the gravity field at Mario.
V up(const Observation& o) { return normalized({-o.gravityX, -o.gravityY, -o.gravityZ}); }
// The part of v across the ground plane at Mario.
V flat(const Observation& o, V v) {
    const V u = up(o);
    return sub(v, scale(u, dot(v, u)));
}
// Camera axes across the ground: right, and forward (screen up).
V cameraRight(const Observation& o) { return normalized(flat(o, {o.camXx, o.camXy, o.camXz})); }
// As stickKeysForWorld: camZ is the view direction; screen-up on the ground is the view
// direction plus the camera's up, both across the ground.
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V cameraForward(const Observation& o) {
    const V camX{o.camXx, o.camXy, o.camXz};
    const V camZ{o.camZx, o.camZy, o.camZz};
    const V camY = cross(scale(camZ, -1.0f), camX);
    return normalized(add(flat(o, camZ), flat(o, camY)));
}
std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}
std::string text(V v) { return "(" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + ")"; }

const Observation::Actor* nearestActor(const Observation& o, const char* kind, V from, bool (*accept)(const Observation::Actor&) = nullptr) {
    const Observation::Actor* best = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    for (const Observation::Actor& actor : o.actors) {
        if (actor.kind != kind || (accept != nullptr && !accept(actor))) continue;
        const float d = length(sub({actor.x, actor.y, actor.z}, from));
        if (d < bestDistance) {
            bestDistance = d;
            best = &actor;
        }
    }
    return best;
}
V at(const Observation::Actor& actor) { return {actor.x, actor.y, actor.z}; }

constexpr int kStatusSwim = 6;
constexpr int kModeBee = 4;
constexpr unsigned long kApproachLimit = 1200;

// Placement points (world space, from the stage data; see smoke_mechanics.hpp).
constexpr V kBeeMushroom{-15750, 12400, -1050};
constexpr V kBeeWall{-15950, 12950, -1257};
constexpr V kSwimShell{-7314, 1295, 11046};
}  // namespace

const std::vector<MechanicPlan>& mechanicPlans() {
    static const std::vector<MechanicPlan> plans = {
        {"starball", "TamakoroExLv2Galaxy", 1},
        {"bee", "HoneyBeeExGalaxy", 1},
        {"swim", "OceanRingGalaxy", 2},
        {"flippanel", "FlipPanelExGalaxy", 1},
        {"ray", "SurfingLv1Galaxy", 1},
    };
    return plans;
}

const MechanicPlan* findMechanic(const std::string& name) {
    for (const MechanicPlan& plan : mechanicPlans()) {
        if (name == plan.name) return &plan;
    }
    return nullptr;
}

void MechanicRun::hold(bool upKey, bool downKey, bool leftKey, bool rightKey, bool a, Step& step) {
    const bool want[5] = {upKey, downKey, leftKey, rightKey, a};
    const Button buttons[5] = {Button::StickUp, Button::StickDown, Button::StickLeft, Button::StickRight, Button::A};
    for (int i = 0; i < 5; i++) {
        if (want[i] != mHeld[i]) {
            step.presses.push_back({buttons[i], want[i]});
            mHeld[i] = want[i];
            step.assertFocus = true;
        }
    }
}

void MechanicRun::releaseAll(Step& step) {
    hold(false, false, false, false, false, step);
}

void MechanicRun::tapA(Step& step) {
    if (!mHeld[4]) {
        step.presses.push_back({Button::A, true});
        mHeld[4] = true;
        step.assertFocus = true;
        mReleaseA = mFrame + 6;
    }
}

float MechanicRun::steerTo(const Observation& o, V target, Step& step, bool a) {
    const V to = flat(o, sub(target, position(o)));
    const StickKeys keys = stickKeysForWorld(o, {to.x, to.y, to.z});
    hold(keys.up, keys.down, keys.left, keys.right, a || (mHeld[4] && mReleaseA > mFrame), step);
    return length(to);
}

float MechanicRun::approach(const Observation& o, V target, Step& step) {
    const V to = flat(o, sub(target, position(o)));
    const float d = length(to);
    if (mStageFrames == 1 || d < mApproachBest - 40.0f) {
        mApproachBest = d;
        mApproachBestFrame = mFrame;
    }
    if (mFrame < mSidestepUntil) {
        const V side = normalized(cross(up(o), to));
        const StickKeys keys = stickKeysForWorld(o, {side.x, side.y, side.z});
        hold(keys.up, keys.down, keys.left, keys.right, false, step);
        return d;
    }
    if (mFrame - mApproachBestFrame > 120) {
        ++mStuck;
        mApproachBestFrame = mFrame;
        if (mStuck % 3 == 0) {
            note("stuck " + number(d) + " from " + text(target) + " at " + text(position(o)) + ": sidestepping");
            mSidestepUntil = mFrame + 45;
        } else {
            note("stuck " + number(d) + " from " + text(target) + " at " + text(position(o)) + ": jumping");
            tapA(step);
        }
    }
    steerTo(o, target, step);
    return d;
}

Result MechanicRun::fail(const std::string& why) {
    mReason = std::string(mPlan.name) + ": " + why;
    note("mechanic " + std::string(mPlan.name) + " FAIL: " + why);
    return Result::Fail;
}

Result MechanicRun::pass(const std::string& why) {
    mReason = std::string(mPlan.name) + ": " + why;
    note("mechanic " + std::string(mPlan.name) + " PASS: " + why);
    return Result::Pass;
}

void MechanicRun::next(int stage) {
    mStage = stage;
    mStageFrames = 0;
}

Result MechanicRun::step(const Observation& o, unsigned long frame, Step& step) {
    mFrame = frame;
    ++mStageFrames;
    if (mHeld[4] && mReleaseA != 0 && mFrame >= mReleaseA) {
        step.presses.push_back({Button::A, false});
        mHeld[4] = false;
        mReleaseA = 0;
    }
    if (o.demoActive) {
        // A demo (power-up, tutorial) owns Mario: send nothing, restart the step's clock.
        hold(false, false, false, false, false, step);
        mStageFrames = 0;
        return Result::Running;
    }
    const std::string name = mPlan.name;
    if (name == "starball") return starBall(o, step);
    if (name == "bee") return bee(o, step);
    if (name == "swim") return swim(o, step);
    if (name == "flippanel") return flipPanel(o, step);
    if (name == "ray") return ray(o, step);
    return fail("unknown mechanic");
}

// Approach, mount, then two tilts. Stages: 0 find, 1 approach, 2 jump on, 3 settle,
// 4 tilt forward, 5 coast, 6 tilt right.
Result MechanicRun::starBall(const Observation& o, Step& step) {
    const Observation::Actor* ball = nearestActor(o, "StarBall", position(o));
    if (ball == nullptr) {
        if (mStageFrames > 300) return fail("no StarBall actor published for 300 frames");
        return Result::Running;
    }
    const bool riding = ball->state == 1;
    const V ballPos = at(*ball);
    switch (mStage) {
    case 0:
        note("Star Ball at " + text(ballPos) + ", Mario at " + text(position(o)) + ", " +
             number(length(sub(ballPos, position(o)))) + " away");
        next(riding ? 3 : 1);
        return Result::Running;
    case 1: {
        if (riding) {
            next(3);
            return Result::Running;
        }
        const float d = approach(o, ballPos, step);
        if (d < 230.0f) {
            note("at the Star Ball (" + number(d) + " across the ground): jumping on, attempt " + std::to_string(mAttempts + 1));
            next(2);
        } else if (mStageFrames > kApproachLimit) {
            return fail("could not reach the Star Ball: still " + number(d) + " away after " + std::to_string(kApproachLimit) + " frames");
        }
        return Result::Running;
    }
    case 2:
        if (mStageFrames == 1) tapA(step);
        steerTo(o, ballPos, step);
        if (riding) {
            hold(false, false, false, false, false, step);
            note("riding the Star Ball at " + text(ballPos) + " after " + std::to_string(mStageFrames) + " frames");
            next(3);
        } else if (mStageFrames > 100) {
            if (++mAttempts >= 6) return fail("Mario did not get onto the Star Ball in 6 jumps (ball at " + text(ballPos) + ", Mario at " + text(position(o)) + ")");
            next(1);
        }
        return Result::Running;
    case 3:
        hold(false, false, false, false, false, step);
        if (!riding) return fail("fell off the Star Ball before the tilts (Mario at " + text(position(o)) + ")");
        if (o.talkActive) mStageFrames = 0;
        mMark2 = ballPos;  // the previous frame's position, for the first tilt's start speed
        if (mStageFrames >= 90) {
            mCount = 0;
            next(4);
        }
        break;
    default: {
        // Tilt phases (stage 4..7): forward, back (brake), right, left; 25 frames each,
        // with 30 level frames before the sideways pair. A phase's effect is the ball's
        // displacement beyond coasting at its starting velocity under the ground
        // friction (0.99 per frame): the game adds 0.4 x the tilt rate per frame, about
        // 112 units over 25 frames at full tilt.
        struct Tilt {
            const char* name;
            bool up, down, left, right;
            bool alongRight;
            float sign;
        };
        static const Tilt kTilts[4] = {{"forward", true, false, false, false, false, 1.0f},
                                       {"back", false, true, false, false, false, -1.0f},
                                       {"right", false, false, false, true, true, 1.0f},
                                       {"left", false, false, true, false, true, -1.0f}};
        const int phase = mStage - 4;
        if (phase >= 4) break;
        const Tilt& tilt = kTilts[phase];
        if (!riding) return fail(std::string("fell off the Star Ball tilting ") + tilt.name + ", " + std::to_string(mStageFrames) + " frames in");
        const unsigned long settle = phase == 2 ? 30 : 0;
        if (mStageFrames <= settle) {
            hold(false, false, false, false, false, step);
            mMark2 = ballPos;
            break;
        }
        if (mStageFrames == settle + 1) {
            mMark = ballPos;
            mAxis = sub(ballPos, mMark2);  // velocity (units per frame) at the phase start
            mAxis = scale(flat(o, mAxis), 1.0f);
        }
        hold(tilt.up, tilt.down, tilt.left, tilt.right, false, step);
        mMark2 = ballPos;
        if (mStageFrames >= settle + 25) {
            hold(false, false, false, false, false, step);
            const V dir = scale(tilt.alongRight ? cameraRight(o) : cameraForward(o), tilt.sign);
            const float coastFactor = (1.0f - std::pow(0.99f, 25.0f)) / 0.01f;
            const V coast = scale(mAxis, coastFactor);
            const float effect = dot(sub(sub(ballPos, mMark), coast), dir);
            note(std::string("tilt ") + tilt.name + ": the ball moved " + number(length(sub(ballPos, mMark))) + " to " + text(ballPos) +
                 ", " + number(effect) + " beyond coasting along the tilt (start speed " + number(length(mAxis)) + ")");
            if (effect < 50.0f) return fail(std::string("tilting ") + tilt.name + " pushed the ball only " + number(effect) + " along the tilt beyond coasting (need 50)");
            mValue = phase == 0 ? effect : mValue;
            if (phase == 3) return pass("rode the Star Ball; each 25-frame tilt (forward, back, right, left) pushed it at least 50 along the tilt, and Mario stayed on");
            next(mStage + 1);
        }
        break;
    }
    }
        return Result::Running;
}

// 0 approach the mushroom, 1 settle as a bee, 2 fly (hold A), 3 fall back, 4 fly into
// the wall, 5 climb.
Result MechanicRun::bee(const Observation& o, Step& step) {
    const V pos = position(o);
    const V u = up(o);
    switch (mStage) {
    case 0: {
        if (o.playerMode == kModeBee) {
            hold(false, false, false, false, false, step);
            note("Bee Mario at " + text(pos) + " after " + std::to_string(mStageFrames) + " frames");
            next(1);
            return Result::Running;
        }
        const float d = approach(o, kBeeMushroom, step);
        if (mStageFrames == 1) note("to the Bee Mushroom at " + text(kBeeMushroom) + ", " + number(d) + " away");
        if (mStageFrames > kApproachLimit) {
            return fail("no Bee Mario: " + number(d) + " from the mushroom after " + std::to_string(kApproachLimit) + " frames (mode " + std::to_string(o.playerMode) + ")");
        }
        return Result::Running;
    }
    case 1:
        hold(false, false, false, false, false, step);
        if (o.playerMode != kModeBee) return fail("lost Bee Mario before flying (mode " + std::to_string(o.playerMode) + ")");
        if (!o.playerOnGround || o.talkActive) mStageFrames = 0;
        if (mStageFrames >= 30) {
            mMark = pos;
            next(2);
        }
        return Result::Running;
    case 2: {
        hold(false, false, false, false, true, step);
        const float rise = dot(sub(pos, mMark), u);
        if (mStageFrames == 20) mValue = rise;
        if (mStageFrames >= 60) {
            mValue2 = rise;
            note("bee flight: A held 60 frames, rise " + number(mValue) + " at frame 20 and " + number(mValue2) + " at frame 60" +
                 (o.playerOnGround ? ", on the ground" : ", in the air"));
            if (mValue2 < mValue + 40.0f || o.playerOnGround) {
                return fail("holding A did not keep Bee Mario rising (rise " + number(mValue) + " at frame 20, " + number(mValue2) + " at frame 60)");
            }
            next(3);
        }
        return Result::Running;
    }
    case 3:
        hold(false, false, false, false, false, step);
        if (o.playerOnGround && mStageFrames > 10) {
            note("landed again at " + text(pos) + "; flying to the honeycomb wall at " + text(kBeeWall));
            next(4);
        } else if (mStageFrames > 600) {
            return fail("Bee Mario did not come down after the flight");
        }
        return Result::Running;
    case 4: {
        if (o.beeWallWalk) {
            hold(false, false, false, false, false, step);
            mMark = pos;
            note("stuck to the honeycomb wall at " + text(pos) + " after " + std::to_string(mStageFrames) + " frames");
            next(5);
            return Result::Running;
        }
        if (o.playerMode != kModeBee) return fail("lost Bee Mario on the way to the wall (mode " + std::to_string(o.playerMode) + ")");
        // Fly toward the wall: hold A (flight), release it every 90 frames so a landing
        // can start a new flight.
        const bool flying = (mStageFrames % 90) < 80;
        const float d = steerTo(o, kBeeWall, step, flying);
        if (mStageFrames % 60 == 0) note("toward the wall: " + number(d) + " across the ground, Mario at " + text(pos));
        if (mStageFrames > 900) return fail("Bee Mario never stuck to the honeycomb wall (" + number(d) + " away across the ground, at " + text(pos) + ")");
        return Result::Running;
    }
    case 5:
        hold(true, false, false, false, false, step);
        if (!o.beeWallWalk) return fail("fell off the honeycomb wall while climbing, " + std::to_string(mStageFrames) + " frames in");
        if (mStageFrames >= 60) {
            hold(false, false, false, false, false, step);
            const float moved = length(sub(pos, mMark));
            note("climbing: moved " + number(moved) + " on the wall to " + text(pos) + ", gravity " + text(scale({o.gravityX, o.gravityY, o.gravityZ}, 100)) + "%");
            if (moved < 60.0f) return fail("Bee Mario stuck to the wall but did not move on it (" + number(moved) + ")");
            return pass("Bee Mario: flight kept rising (" + number(mValue) + " -> " + number(mValue2) + "), stuck to the honeycomb wall and climbed " + number(moved));
        }
        return Result::Running;
    }
    return Result::Running;
}

// 0 walk toward the shell until swimming, 1 stroke.
Result MechanicRun::swim(const Observation& o, Step& step) {
    const V pos = position(o);
    switch (mStage) {
    case 0: {
        if (o.marioStatus == kStatusSwim) {
            hold(false, false, false, false, false, step);
            note("swimming at " + text(pos) + " after " + std::to_string(mStageFrames) + " frames");
            mMark = pos;
            mAxis = normalized(flat(o, sub(kSwimShell, pos)));
            next(1);
            return Result::Running;
        }
        const float d = approach(o, kSwimShell, step);
        if (mStageFrames == 1) note("to the water toward the shell at " + text(kSwimShell) + ", " + number(d) + " away");
        if (d < 120.0f) return fail("reached the shell at " + text(pos) + " without swimming (status " + std::to_string(o.marioStatus) + ")");
        if (mStageFrames > kApproachLimit) return fail("never swam: " + number(d) + " from the shell (status " + std::to_string(o.marioStatus) + ")");
        return Result::Running;
    }
    case 1: {
        if (o.marioStatus != kStatusSwim) return fail("stopped swimming " + std::to_string(mStageFrames) + " frames into the strokes (status " + std::to_string(o.marioStatus) + " at " + text(pos) + ")");
        const bool stroke = (mStageFrames % 20) < 6;
        steerTo(o, add(pos, scale(mAxis, 1000.0f)), step, stroke);
        if (mStageFrames >= 120) {
            hold(false, false, false, false, false, step);
            const float moved = length(sub(pos, mMark));
            note("swim strokes: moved " + number(moved) + " to " + text(pos));
            if (moved < 150.0f) return fail("swimming moved Mario only " + number(moved) + " in 120 frames (need 150)");
            return pass("swam " + number(moved) + " in 120 frames of strokes, status Swim throughout");
        }
        return Result::Running;
    }
    }
    return Result::Running;
}

// Walk onto unflipped panels one at a time; each must flip once Mario stands on it.
Result MechanicRun::flipPanel(const Observation& o, Step& step) {
    const V pos = position(o);
    if (mStage == 0) {
        if (mStageFrames < 30) return Result::Running;
        int panels = 0;
        for (const auto& actor : o.actors) panels += actor.kind == "FlipPanel";
        note(std::to_string(panels) + " flip panels published");
        if (panels == 0) return fail("no FlipPanel actors published");
        next(1);
        return Result::Running;
    }
    if (mStage == 1) {
        // The nearest unflipped panel not tried yet, at least 150 away (not the one underfoot).
        const Observation::Actor* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();
        for (const auto& actor : o.actors) {
            if (actor.kind != "FlipPanel" || actor.state != 0) continue;
            bool tried = false;
            for (V done : mDone) tried = tried || length(sub(done, at(actor))) < 10.0f;
            const float d = length(sub(at(actor), pos));
            if (!tried && d > 150.0f && d < bestDistance) {
                bestDistance = d;
                best = &actor;
            }
        }
        if (best == nullptr) return fail("no unflipped panel left to try");
        mMark = at(*best);
        mDone.push_back(mMark);
        note("to the panel at " + text(mMark) + ", " + number(bestDistance) + " away");
        next(2);
        return Result::Running;
    }
    // Stage 2: walk onto the chosen panel and watch it.
    const Observation::Actor* panel = nearestActor(o, "FlipPanel", mMark);
    if (panel == nullptr) return fail("the chosen panel disappeared");
    const float d = steerTo(o, mMark, step);
    if (panel->state == 1) {
        hold(false, false, false, false, false, step);
        note("the panel at " + text(mMark) + " flipped with Mario at " + text(pos) + " (" + number(d) + " from its centre)");
        if (++mCount >= 2) return pass("two panels flipped as Mario stepped on them");
        next(1);
        return Result::Running;
    }
    if (mStageFrames > 600) return fail("the panel at " + text(mMark) + " did not flip; Mario " + number(d) + " from it, on the ground " + (o.playerOnGround ? "yes" : "no"));
    return Result::Running;
}

// 0 find, 1 approach, 2 jump on, 3 settle, 4 level, 5 twist right, 6 jump.
Result MechanicRun::ray(const Observation& o, Step& step) {
    const Observation::Actor* rayActor = nearestActor(o, "Ray", position(o));
    if (rayActor == nullptr) {
        if (mStageFrames > 300) return fail("no Ray actor published for 300 frames");
        return Result::Running;
    }
    const bool ridden = (rayActor->state & 1) != 0;
    const bool onWater = (rayActor->state & 2) != 0;
    const V rayPos = at(*rayActor);
    const V front = normalized({rayActor->dx, rayActor->dy, rayActor->dz});
    switch (mStage) {
    case 0:
        note("Ray at " + text(rayPos) + ", Mario at " + text(position(o)) + ", " + number(length(sub(rayPos, position(o)))) + " away");
        next(ridden ? 3 : 1);
        return Result::Running;
    case 1: {
        if (ridden) {
            next(3);
            return Result::Running;
        }
        const float d = approach(o, rayPos, step);
        if (mStageFrames % 120 == 0) note("to the Ray: " + number(d) + " across the ground, Mario at " + text(position(o)));
        if (d < 200.0f) {
            note("at the Ray (" + number(d) + "): jumping on, attempt " + std::to_string(mAttempts + 1));
            next(2);
        } else if (mStageFrames > 3000) {
            return fail("could not reach the Ray: still " + number(d) + " away after 3000 frames");
        }
        return Result::Running;
    }
    case 2:
        if (mStageFrames == 1) tapA(step);
        steerTo(o, rayPos, step);
        if (ridden) {
            hold(false, false, false, false, false, step);
            note("riding the Ray at " + text(rayPos) + (onWater ? ", on the water" : ", not on the water"));
            next(3);
        } else if (mStageFrames > 100) {
            if (++mAttempts >= 6) return fail("Mario did not get onto the Ray in 6 jumps (Ray at " + text(rayPos) + ", Mario at " + text(position(o)) + ")");
            next(1);
        }
        return Result::Running;
    case 3:
        hold(false, false, false, false, false, step);
        if (!ridden) return fail("fell off the Ray before the checks");
        if (o.talkActive) mStageFrames = 0;
        if (mStageFrames >= 60) {
            mMark = rayPos;
            mAxis = front;
            mCount = 0;
            next(4);
        }
        return Result::Running;
    case 4:
        hold(false, false, false, false, false, step);
        if (!ridden) return fail("fell off the Ray riding level, " + std::to_string(mStageFrames) + " frames in");
        mCount += onWater ? 1 : 0;
        if (mStageFrames >= 120) {
            mValue = dot(sub(rayPos, mMark), mAxis);
            note("level: the Ray moved " + number(length(sub(rayPos, mMark))) + " (" + number(mValue) + " along its front) to " + text(rayPos) + ", on the water " + std::to_string(mCount) + "/120 frames");
            if (mValue < 300.0f) return fail("riding level moved the Ray only " + number(mValue) + " along its front in 120 frames (need 300)");
            if (mCount < 100) return fail("the Ray was on the water only " + std::to_string(mCount) + " of 120 frames riding level");
            mAxis = front;
            next(5);
        }
        return Result::Running;
    case 5:
        hold(false, false, false, true, false, step);
        if (!ridden) return fail("fell off the Ray while turning, " + std::to_string(mStageFrames) + " frames in");
        if (mStageFrames >= 90) {
            hold(false, false, false, false, false, step);
            const V u = up(o);
            const float cosine = std::fmax(-1.0f, std::fmin(1.0f, dot(normalized(flat(o, front)), normalized(flat(o, mAxis)))));
            const float degrees = std::acos(cosine) * 57.29578f;
            // Right of the old heading: the old front crossed with up points left, so right turns give a negative dot.
            const float side = dot(cross(u, mAxis), front);
            note("twist right: heading turned " + number(degrees) + " degrees (" + (side < 0 ? "right" : "left") + ")");
            if (degrees < 20.0f || side >= 0) return fail("twisting right turned the Ray " + number(degrees) + " degrees " + (side < 0 ? "right" : "left") + " (need 20 right)");
            mValue2 = degrees;
            mMark = rayPos;
            next(6);
        }
        return Result::Running;
    case 6: {
        if (mStageFrames == 1) tapA(step);
        if (!ridden) return fail("fell off the Ray jumping, " + std::to_string(mStageFrames) + " frames in");
        const float rise = dot(sub(rayPos, mMark), up(o));
        if (!onWater) mCount = -1;
        mBestDistance = std::fmax(mBestDistance, rise);
        if (mCount == -1 && onWater && mStageFrames > 5) {
            note("jump: the Ray left the water, rose " + number(mBestDistance) + ", and landed back on it after " + std::to_string(mStageFrames) + " frames");
            if (mBestDistance < 30.0f) return fail("the Ray's jump rose only " + number(mBestDistance));
            return pass("rode the Ray: level " + number(mValue) + " forward on the water, right twist turned " + number(mValue2) + " degrees, jump rose " + number(mBestDistance) + " and landed on the water");
        }
        if (mStageFrames > 150) return fail(mCount == -1 ? "the Ray jumped but was not back on the water within 150 frames" : "tapping A did not make the Ray leave the water");
        return Result::Running;
    }
    }
    return Result::Running;
}

}  // namespace PetariNative::App::Smoke

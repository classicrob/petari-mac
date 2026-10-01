#pragma once
// Super Mario Odyssey movement model for the OdysseyMovement mod
// (docs/dev/ODYSSEY_MOVEMENT.md). Plain functions over gravity-relative speeds
// (front, side, up) and SMO's PlayerConst values (OdysseyDecomp, v1.0.0);
// no game types, so the rules are unit-tested on their own
// (native/tests/odyssey_move_tests.cpp) and Galaxy's Mario applies the result.
//
// Units are SMO's: units/frame and units/frame² at 60 fps. Scale 1.0 (Galaxy
// Mario is SMO-sized). Header-only so game code and tests share it.

#include <algorithm>
#include <cmath>

namespace PetariNative::Odyssey {

// PlayerConst defaults used by the mod (names as in OdysseyDecomp).
namespace Const {
constexpr float NormalMinSpeed = 3.0f;
constexpr float NormalMaxSpeed = 14.0f;
constexpr float NormalAccelFrame = 40.0f;
constexpr float NormalBrakeFrame = 10.0f;
constexpr float StickOnBrakeFrame = 120.0f;
constexpr float FallSpeedMax = 35.0f;
constexpr int ExtendFrame = 10;
constexpr float JumpPowerMin = 17.0f;
constexpr float JumpPowerMax = 19.5f;
constexpr float JumpGravity = 1.5f;
constexpr float ContinuousJumpPowerMin = 19.5f;
constexpr float JumpPowerMax2nd = 21.0f;
constexpr float JumpGravity2nd = 1.5f;
constexpr float JumpPowerMax3rd = 25.0f;
constexpr float JumpGravity3rd = 1.0f;
constexpr int ContinuousJumpCount = 3;
constexpr int ContinuousJumpTimer = 10;
constexpr int ContinuousJumpPreInputFrame = 5;
constexpr float JumpBaseSpeedMax = 24.0f;
constexpr float JumpAccelFront = 0.5f;
constexpr float JumpAccelBack = 1.0f;
constexpr float JumpAccelTurn = 0.3f;
constexpr float SquatJumpPower = 32.0f;  // backflip
constexpr float SquatJumpGravity = 1.0f;
constexpr float SquatJumpBackPower = 5.0f;
constexpr float SquatJumpMovePower = 0.2f;
constexpr float SquatJumpMoveSpeedMax = 9.0f;
constexpr float TurnJumpPower = 32.0f;  // sideflip
constexpr float TurnJumpGravity = 1.0f;
constexpr float TurnJumpVelH = 9.0f;
constexpr float TurnJumpBrake = 0.5f;
constexpr float TurnJumpAccel = 0.25f;
constexpr float TurnJumpSideAccel = 0.075f;
constexpr float LongJumpAccel = 0.25f;
constexpr float LongJumpBrake = 0.5f;
constexpr float LongJumpSideAccel = 0.25f;
constexpr float LongJumpGravity = 0.48f;
constexpr float LongJumpJumpPow = 12.0f;
constexpr float LongJumpMovePow = 4.0f;
constexpr float LongJumpInitSpeed = 14.0f;
constexpr float LongJumpSpeed = 23.0f;
constexpr float LongJumpSpeedMin = 2.5f;
constexpr float HeadSlidingSpeed = 20.0f;  // dive
constexpr float HeadSlidingSpeedMin = 2.5f;
constexpr float HeadSlidingBrake = 0.5f;
constexpr float HeadSlidingSideAccel = 0.125f;
constexpr float HeadSlidingJump = 28.0f;
constexpr float HeadSlidingGravityAir = 2.0f;
constexpr float HipDropSpeed = 45.0f;
constexpr float HipDropSpeedMax = 45.0f;
constexpr int JumpHipDropPermitBeginFrame = 5;
constexpr int JumpHipDropPermitEndFrame = 30;
constexpr float JumpHipDropPower = 40.0f;
constexpr float WallJumpHSpeed = 8.6f;
constexpr float WallJumpPower = 23.0f;
constexpr float WallJumpGravity = 0.95f;
constexpr int WallJumpInvalidateInputFrame = 25;
constexpr float GravityWallSlide = 0.5f;
constexpr float SquatAccelRate = 1.2f;
constexpr float SquatBrakeRate = 0.95f;
constexpr float SquatBrakeEndSpeed = 3.5f;
constexpr float SlopeRollingSpeedStart = 20.0f;
constexpr float SlopeRollingMaxSpeed = 35.0f;
constexpr float SlopeRollingSpeedEnd = 17.0f;
constexpr float SlopeRollingBrake = 0.998f;
constexpr int SlopeRollingReStartInterval = 15;
}  // namespace Const

// What an airborne move is; it selects the gravity, the hold-extend rule and the
// horizontal controller.
enum class Air : int {
    None,
    Jump1, Jump2, Jump3,  // the jump chain
    Backflip, Sideflip, LongJump, Dive, GroundPoundJump, WallJump,
    Fall,
};

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// PlayerActionFunction::calcStickPow: 0.1 dead zone, rescaled to 0..1.
inline float stickPow(float r) {
    const float a = std::fabs(r);
    return std::clamp((a - 0.1f) / 0.9f, 0.0f, 1.0f);
}

// calcJumpSpeed: power from forward speed, NormalMin..MaxSpeed (3..14).
inline float jumpPower(Air kind, float frontSpeed) {
    const float t = std::clamp((frontSpeed - Const::NormalMinSpeed) / (Const::NormalMaxSpeed - Const::NormalMinSpeed), 0.0f, 1.0f);
    switch (kind) {
    case Air::Jump1: return lerp(Const::JumpPowerMin, Const::JumpPowerMax, t);
    case Air::Jump2: return lerp(Const::ContinuousJumpPowerMin, Const::JumpPowerMax2nd, t);
    case Air::Jump3: return lerp(Const::ContinuousJumpPowerMin, Const::JumpPowerMax3rd, t);
    default: return 0.0f;
    }
}

inline float gravityOf(Air kind) {
    switch (kind) {
    case Air::Jump1: return Const::JumpGravity;
    case Air::Jump2: return Const::JumpGravity2nd;
    case Air::Jump3: return Const::JumpGravity3rd;
    case Air::Backflip: return Const::SquatJumpGravity;
    case Air::Sideflip: return Const::TurnJumpGravity;
    case Air::LongJump: return Const::LongJumpGravity;
    case Air::Dive: return Const::HeadSlidingGravityAir;
    case Air::GroundPoundJump: return Const::JumpGravity;
    case Air::WallJump: return Const::WallJumpGravity;
    default: return Const::JumpGravity;  // falls (GravityAir 1.5)
    }
}

// Only the chain jumps (and the cap-return jump, not modelled) hold their speed
// while the button is held, for up to ExtendFrame frames.
inline bool extends(Air kind) { return kind == Air::Jump1 || kind == Air::Jump2 || kind == Air::Jump3; }

// Speeds relative to gravity and the move's reference direction (SMO measures
// vectoring against the airborne action's initial direction).
struct AirState {
    Air kind = Air::None;
    float up = 0.0f;     // along -gravity, units/frame
    float front = 0.0f;  // along the reference direction
    float side = 0.0f;   // perpendicular to it, across gravity
    float cap = 0.0f;    // horizontal limit for front and side
    int frame = 0;       // frames since the move started
    bool extending = false;
    int inputLockout = 0;  // frames the stick is ignored (wall jump)
};

// Starts an airborne move. frontSpeed is the ground speed along the new
// reference direction at take-off.
inline AirState startAir(Air kind, float frontSpeed) {
    AirState s;
    s.kind = kind;
    s.extending = extends(kind);
    switch (kind) {
    case Air::Jump1:
    case Air::Jump2:
    case Air::Jump3:
        s.up = jumpPower(kind, frontSpeed);
        s.front = std::min(frontSpeed, Const::JumpBaseSpeedMax);
        s.cap = std::max(s.front, Const::NormalMaxSpeed);
        break;
    case Air::Backflip:
        s.up = Const::SquatJumpPower;
        s.front = -Const::SquatJumpBackPower;
        s.cap = Const::SquatJumpMoveSpeedMax;
        break;
    case Air::Sideflip:  // reference direction: the new facing
        s.up = Const::TurnJumpPower;
        s.front = Const::TurnJumpVelH;
        s.cap = Const::TurnJumpVelH;
        break;
    case Air::LongJump:
        s.up = Const::LongJumpJumpPow;
        s.front = std::min(frontSpeed + Const::LongJumpMovePow, Const::LongJumpInitSpeed);
        s.cap = Const::LongJumpSpeed;
        break;
    case Air::Dive:
        s.up = Const::HeadSlidingJump;
        s.front = Const::HeadSlidingSpeed;
        s.cap = Const::HeadSlidingSpeed;
        break;
    case Air::GroundPoundJump:
        s.up = Const::JumpHipDropPower;
        s.front = 0.0f;
        s.cap = Const::NormalMaxSpeed;
        break;
    case Air::WallJump:  // reference direction: away from the wall
        s.up = Const::WallJumpPower;
        s.front = Const::WallJumpHSpeed;
        s.cap = std::max(Const::WallJumpHSpeed, Const::NormalMaxSpeed);
        s.inputLockout = Const::WallJumpInvalidateInputFrame;
        break;
    case Air::Fall:
    case Air::None:
        s.front = frontSpeed;
        s.cap = std::max(frontSpeed, 11.0f);  // JumpMoveSpeedMin
        break;
    }
    return s;
}

// One frame in the air. jumpHeld: the jump button; stickFront/stickSide: the
// stick projected onto the reference direction (-1..1, dead zone applied by the
// caller). Returns the distance moved along up this frame (SMO's order: the
// chain jumps move before gravity on their first frame; other moves take
// gravity first).
inline float stepAir(AirState& s, bool jumpHeld, float stickFront, float stickSide) {
    const float g = gravityOf(s.kind);
    float rise;
    if (s.extending && (s.frame == 0 || (jumpHeld && s.frame < Const::ExtendFrame))) {
        // Chain jumps keep their speed on the first frame and then while the
        // button stays held, ExtendFrame frames at most (SMO: "velocity only
        // begins decreasing after A/B is released or after 10 frames").
        rise = s.up;
    } else {
        s.extending = false;
        s.up -= g;
        rise = s.up;
    }
    s.up = std::max(s.up, -Const::FallSpeedMax);

    if (s.inputLockout > 0) {
        --s.inputLockout;
        stickFront = stickSide = 0.0f;
    }
    float accelFront = Const::JumpAccelFront, accelBack = Const::JumpAccelBack, accelSide = Const::JumpAccelTurn, minFront = -s.cap;
    switch (s.kind) {
    case Air::LongJump:
        accelFront = Const::LongJumpAccel; accelBack = Const::LongJumpBrake; accelSide = Const::LongJumpSideAccel;
        minFront = Const::LongJumpSpeedMin;
        break;
    case Air::Dive:
        accelFront = 0.0f; accelBack = Const::HeadSlidingBrake; accelSide = Const::HeadSlidingSideAccel;
        minFront = Const::HeadSlidingSpeedMin;
        break;
    case Air::Sideflip:
        accelFront = Const::TurnJumpAccel; accelBack = Const::TurnJumpBrake; accelSide = Const::TurnJumpSideAccel;
        minFront = 0.0f;
        break;
    case Air::Backflip:
        accelFront = accelBack = accelSide = Const::SquatJumpMovePower;
        break;
    default:
        break;
    }
    if (stickFront > 0.0f) s.front = std::min(s.front + accelFront * stickFront, s.cap);
    else if (stickFront < 0.0f) s.front = std::max(s.front + accelBack * stickFront, minFront);
    s.side += accelSide * stickSide;
    if (s.kind != Air::Sideflip) s.side = std::clamp(s.side, -s.cap, s.cap);  // the sideflip's side speed is uncapped
    ++s.frame;
    return rise;
}

// Ground run speed (flat ground): the cap is 3 + 11·stick; below it Mario
// accelerates by NormalMaxSpeed/NormalAccelFrame, above it decays by
// NormalMaxSpeed/StickOnBrakeFrame, and with the stick released brakes by
// NormalMaxSpeed/NormalBrakeFrame.
inline float stepRun(float speed, float stick) {
    const float r = stickPow(stick);
    if (r <= 0.0f) return std::max(speed - Const::NormalMaxSpeed / Const::NormalBrakeFrame, 0.0f);
    const float cap = Const::NormalMinSpeed + (Const::NormalMaxSpeed - Const::NormalMinSpeed) * r;
    if (speed < cap) return std::min(speed + Const::NormalMaxSpeed / Const::NormalAccelFrame, cap);
    return std::max(speed - Const::NormalMaxSpeed / Const::StickOnBrakeFrame, cap);
}

// Crouching: ×1.2 on entry (at or below the run cap), then ×0.95 per frame
// while above 3.5.
inline float squatEntrySpeed(float speed) {
    return speed <= Const::NormalMaxSpeed ? speed * Const::SquatAccelRate : speed;
}
inline float stepSquat(float speed) {
    return speed > Const::SquatBrakeEndSpeed ? std::max(speed * Const::SquatBrakeRate, Const::SquatBrakeEndSpeed) : speed;
}

// The triple-jump chain (PlayerContinuousJump): the next jump continues the
// chain when it comes within ContinuousJumpTimer grounded frames of the last
// landing, Mario runs at (nearly) full speed (jump power ≥ 0.99·JumpPowerMax)
// and the direction is within 45° of the previous jump's. Returns the index
// (0, 1, 2) of the jump to make.
inline int nextChainIndex(int previous, int groundedFrames, float frontSpeed, float directionCos) {
    if (previous < 0 || groundedFrames > Const::ContinuousJumpTimer) return 0;
    if (jumpPower(Air::Jump1, frontSpeed) < 0.99f * Const::JumpPowerMax) return 0;
    if (directionCos < 0.70710678f) return 0;
    return (previous + 1) % Const::ContinuousJumpCount;
}

inline Air chainAir(int index) { return index == 2 ? Air::Jump3 : index == 1 ? Air::Jump2 : Air::Jump1; }

// The ground-pound jump: allowed on landing frames JumpHipDropPermitBegin..End.
inline bool groundPoundJumpAllowed(int landingFrame) {
    return landingFrame >= Const::JumpHipDropPermitBeginFrame && landingFrame <= Const::JumpHipDropPermitEndFrame;
}

// Ground-pound fall speed: HipDropSpeed added on the first frame, capped.
inline float groundPoundFallSpeed() { return std::min(Const::HipDropSpeed, Const::HipDropSpeedMax); }

// Rolling: starts at least SlopeRollingSpeedStart, decays ×0.998 per frame on
// flat ground, ends below SlopeRollingSpeedEnd; a boost (Spin, at least
// SlopeRollingReStartInterval frames apart) raises it to at least `bump`.
struct RollState {
    float speed = 0.0f;
    int sinceBoost = 0;
    bool rolling = false;
};
inline RollState startRoll(float speed, bool fromGroundPound) {
    RollState r;
    r.rolling = true;
    r.speed = std::max(speed, fromGroundPound ? 30.0f : Const::SlopeRollingSpeedStart);
    r.sinceBoost = 0;
    return r;
}
inline void stepRoll(RollState& r, bool boostPressed, float bump = 23.0f) {
    if (!r.rolling) return;
    ++r.sinceBoost;
    if (boostPressed && r.sinceBoost >= Const::SlopeRollingReStartInterval) {
        r.speed = std::max(r.speed, bump);
        r.sinceBoost = 0;
    }
    r.speed = std::min(r.speed * Const::SlopeRollingBrake, Const::SlopeRollingMaxSpeed);
    if (r.speed < Const::SlopeRollingSpeedEnd) r.rolling = false;
}

}  // namespace PetariNative::Odyssey

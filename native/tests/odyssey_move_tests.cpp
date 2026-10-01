// OdysseyMovement model (native/include/petari/odyssey_move.hpp) against the
// numbers that follow from OdysseyDecomp's PlayerConst and that the SMO
// community measured (docs/dev/ODYSSEY_MOVEMENT.md, section 6).
#include "petari/odyssey_move.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace PetariNative::Odyssey;

static int checks;
static void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        std::exit(1);
    }
}
static bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

struct Apex {
    float height = 0.0f;
    int frame = 0;  // frames until the peak
};
// Peak height along up for a move, holding the jump button for holdFrames.
static Apex apex(Air kind, float frontSpeed, int holdFrames) {
    AirState s = startAir(kind, frontSpeed);
    float y = 0.0f;
    Apex best;
    for (int f = 0; f < 600; ++f) {
        y += stepAir(s, f < holdFrames, 0.0f, 0.0f);
        if (y > best.height) {
            best.height = y;
            best.frame = f + 1;
        }
        if (s.up < 0.0f && y < best.height) break;
    }
    return best;
}

static void testJumpArcs() {
    check(near(apex(Air::Jump1, 0.0f, 0).height, 105.0f), "single jump from standstill, tap: 105");
    check(near(apex(Air::Jump1, 0.0f, 100).height, 105.0f + 9.0f * 17.0f), "single jump from standstill, held: 258");
    check(near(apex(Air::Jump1, 14.0f, 0).height, 136.5f), "single jump at full speed, tap: 136.5");
    check(near(apex(Air::Jump1, 14.0f, 100).height, 312.0f), "single jump at full speed, held: 312");
    check(near(apex(Air::Jump1, 14.0f, 4).height, 136.5f + 3.0f * 19.5f), "a 4-frame hold extends 3 frames");
    check(near(apex(Air::Jump2, 14.0f, 0).height, 157.5f), "double jump, tap: 157.5");
    check(near(apex(Air::Jump2, 14.0f, 100).height, 346.5f), "double jump, held: 346.5");
    check(near(apex(Air::Jump3, 14.0f, 0).height, 325.0f), "triple jump, tap: 325");
    check(near(apex(Air::Jump3, 14.0f, 100).height, 550.0f), "triple jump, held: 550");
    check(near(apex(Air::Backflip, 0.0f, 100).height, 496.0f), "backflip: 496 (holding changes nothing)");
    check(near(apex(Air::Sideflip, 0.0f, 0).height, 496.0f), "sideflip: 496");
    check(near(apex(Air::LongJump, 14.0f, 0).height, 144.0f), "long jump: 144");
    check(near(apex(Air::Dive, 14.0f, 0).height, 182.0f), "dive: +182");
    // Community measurement: 513 (v0 38.5); the per-frame sum of 38.5, 37, ... 1
    // is 513.5, so the measurement is within half a unit.
    check(near(apex(Air::GroundPoundJump, 0.0f, 0).height, 513.5f), "ground-pound jump: 513.5 (measured 513)");
    check(near(apex(Air::WallJump, 0.0f, 0).height, 267.0f), "wall jump: 267");
}

static void testJumpPower() {
    check(near(jumpPower(Air::Jump1, 0.0f), 17.0f) && near(jumpPower(Air::Jump1, 3.0f), 17.0f), "power 17 at or below speed 3");
    check(near(jumpPower(Air::Jump1, 8.5f), 18.25f), "power interpolates by speed");
    check(near(jumpPower(Air::Jump1, 30.0f), 19.5f), "power 19.5 at or above speed 14");
    check(near(jumpPower(Air::Jump3, 14.0f), 25.0f) && near(jumpPower(Air::Jump2, 0.0f), 19.5f), "2nd/3rd jump ranges");
}

static void testChain() {
    check(nextChainIndex(-1, 0, 14.0f, 1.0f) == 0, "no previous jump: first jump");
    check(nextChainIndex(0, 5, 14.0f, 1.0f) == 1 && nextChainIndex(1, 5, 14.0f, 1.0f) == 2, "chain advances");
    check(nextChainIndex(2, 5, 14.0f, 1.0f) == 0, "after the triple, back to the first");
    check(nextChainIndex(0, 11, 14.0f, 1.0f) == 0, "more than 10 grounded frames break the chain");
    check(nextChainIndex(0, 10, 14.0f, 1.0f) == 1, "10 grounded frames still chain");
    check(nextChainIndex(0, 5, 13.0f, 1.0f) == 0, "below (nearly) full speed the chain breaks");
    check(nextChainIndex(0, 5, 14.0f, 0.70f) == 0 && nextChainIndex(0, 5, 14.0f, 0.71f) == 1, "45 degree turn limit");
}

static void testAirControl() {
    AirState s = startAir(Air::Jump1, 14.0f);
    check(near(s.front, 14.0f) && near(s.cap, 14.0f), "jump keeps take-off speed, cap max(v0,14)");
    stepAir(s, false, -1.0f, 0.0f);
    check(near(s.front, 13.0f), "back: 1.0 per frame");
    stepAir(s, false, 1.0f, 0.0f);
    check(near(s.front, 13.5f), "forward: 0.5 per frame");
    for (int i = 0; i < 10; ++i) stepAir(s, false, 1.0f, 1.0f);
    check(near(s.front, 14.0f) && near(s.side, 3.0f), "forward capped at 14, side 0.3 per frame");
    AirState fast = startAir(Air::Jump1, 30.0f);
    check(near(fast.front, 24.0f), "take-off speed limited to 24");

    AirState lj = startAir(Air::LongJump, 13.0f);
    check(near(lj.front, 14.0f), "long jump starts at min(v+4, 14)");
    for (int i = 0; i < 100; ++i) stepAir(lj, false, 1.0f, 0.0f);
    check(near(lj.front, 23.0f), "long jump accelerates to 23");
    for (int i = 0; i < 100; ++i) stepAir(lj, false, -1.0f, 0.0f);
    check(near(lj.front, 2.5f), "long jump brakes no lower than 2.5");

    AirState dive = startAir(Air::Dive, 5.0f);
    stepAir(dive, false, 1.0f, 0.0f);
    check(near(dive.front, 20.0f), "dive: 20, no forward acceleration");

    AirState flip = startAir(Air::Sideflip, 0.0f);
    for (int i = 0; i < 200; ++i) stepAir(flip, false, 0.0f, 1.0f);
    check(flip.side > 14.0f, "sideflip side speed is uncapped (speedflip)");

    AirState wall = startAir(Air::WallJump, 0.0f);
    for (int i = 0; i < 25; ++i) stepAir(wall, false, -1.0f, 0.0f);
    check(near(wall.front, 8.6f), "wall jump ignores the stick for 25 frames");
    stepAir(wall, false, -1.0f, 0.0f);
    check(near(wall.front, 7.6f), "then air control returns");

    AirState fall = startAir(Air::Fall, 0.0f);
    for (int i = 0; i < 200; ++i) stepAir(fall, false, 0.0f, 0.0f);
    check(near(fall.up, -35.0f), "falling is capped at 35");
}

static void testGround() {
    float v = 0.0f;
    int frames = 0;
    while (v < 14.0f && frames < 200) {
        v = stepRun(v, 1.0f);
        ++frames;
    }
    check(near(v, 14.0f) && frames == 40, "full speed 14 after 40 frames");
    check(near(stepRun(17.0f, 1.0f), 17.0f - 14.0f / 120.0f), "above the cap: -0.1167 per frame");
    check(near(stepRun(14.0f, 0.0f), 12.6f), "stick released: brake 1.4");
    check(near(stepRun(0.0f, 0.05f), 0.0f), "inside the 0.1 dead zone: no movement");
    check(near(stepRun(14.0f, 0.55f), 14.0f - 14.0f / 120.0f) && near(stepRun(0.0f, 0.55f), 0.35f), "half stick caps at 8.5");
    check(near(squatEntrySpeed(10.0f), 12.0f) && near(squatEntrySpeed(20.0f), 20.0f), "crouch entry x1.2 up to the run cap");
    check(near(stepSquat(10.0f), 9.5f) && near(stepSquat(3.6f), 3.5f) && near(stepSquat(3.0f), 3.0f), "crouch slide x0.95 to 3.5");
}

static void testGroundPoundAndRoll() {
    check(!groundPoundJumpAllowed(4) && groundPoundJumpAllowed(5) && groundPoundJumpAllowed(30) && !groundPoundJumpAllowed(31),
          "ground-pound jump window: landing frames 5-30");
    check(near(groundPoundFallSpeed(), 45.0f), "ground pound falls at 45");
    RollState r = startRoll(5.0f, false);
    check(r.rolling && near(r.speed, 20.0f), "roll starts at 20");
    check(near(startRoll(5.0f, true).speed, 30.0f), "ground-pound roll starts at 30");
    stepRoll(r, true);
    check(r.speed < 20.0f, "no boost within 15 frames");
    for (int i = 0; i < 14; ++i) stepRoll(r, false);
    stepRoll(r, true);
    check(near(r.speed, 23.0f * 0.998f, 0.01f), "boost to 23 after 15 frames");
    RollState slow = startRoll(0.0f, false);
    int frames = 0;
    while (slow.rolling && frames < 1000) {
        stepRoll(slow, false);
        ++frames;
    }
    check(!slow.rolling && frames > 50, "an unboosted roll decays below 17 and ends");
}

int main() {
    testJumpPower();
    testJumpArcs();
    testChain();
    testAirControl();
    testGround();
    testGroundPoundAndRoll();
    std::printf("%d Odyssey movement model checks passed\n", checks);
}

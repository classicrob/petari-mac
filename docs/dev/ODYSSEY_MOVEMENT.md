# Odyssey movement mod (design)

An optional mod, **`OdysseyMovement`** (off by default), that makes Mario move
like he does in *Super Mario Odyssey* (SMO): its run, jump arcs, triple-jump
chain, long jump, ground-pound jump, backflip, sideflip, dive, roll and wall
slide, with SMO's own numbers. With the mod off the game is unchanged.

Status: design (2026-10-01). Implementation follows in the order at the end.
Decisions below were made without the user (they were away); each says why, so
they can be revisited.

## Sources

- **SMO constants:** `PlayerConst.cpp` in
  [MonsterDruide1/OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp)
  (all 615 player constants, v1.0.0), and the decompiled move states that use
  them (long jump, dive, wall jump, ground pound, jump chain, jump buffer).
- **Measurements:** [smo.wiki](https://smo.wiki/) and the "Odyssey Movement Data
  v2" sheet (frame-by-frame heights, speeds, timings). Every measured value
  checked so far follows from the constants by plain per-frame integration, so
  the constants are treated as ground truth. The research notes and the full
  cross-check table: `build/mods-research/odyssey-physics.md` (local).
- **Galaxy side:** `MarioConstTable` (`include/Game/Player/MarioConst.hpp`,
  defaults in `src/Game/Player/MarioConst.cpp`), `MarioJump.cpp` (`tryJump`,
  `procJump`, `trySquatJump`, `tryBackJump`, `tryTurnJump`, `tryWallJump`,
  `procHipDrop`, `doLanding`), `MarioMove.cpp`, `MarioWall.cpp`.

SMO's core air and ground controllers (`PlayerActionAirMoveControl`,
`PlayerActionGroundMoveControl`) are not decompiled yet. For those parts the mod
follows the community's quantitative description (vectoring, midair turning,
run turning), which matches the constants wherever it was checked.

## 1. Scale: 1.0 (SMO units used as they are)

| | SMO | Galaxy | Ratio |
|---|---|---|---|
| Collision height | `Tall` 160 | ceiling checks at 160 (`Mario::calcDistToCeil(...) < 160`) | 1.00 |
| Collision radius | `CollisionRadius` 55 | binder radius 60 (`MarioActor::initBinder(60, ...)`) | 0.92 |
| Full run speed | `NormalMaxSpeed` 14 u/f | `mWalkSpeed` 13 u/f | 0.93 |
| Frame rate | 60 | 60 | 1 |
| First jump | 17–19.5 u/f up, gravity 1.5 (105–312 u high) | `mJumpHeight[0]` 22 up, gravity 1.8 | similar |

Measured in the game (`PETARI_SMOKE=movement` on the observatory's flat start,
2026-10-01; heights along gravity from the take-off point):

| | Galaxy (mod off) | Odyssey mod on | SMO (decomp/community) |
|---|---|---|---|
| Standstill jump, A held / tapped | 260.0 / 156.4 | 258.000 / 105.000 | 258 / 105 |
| Run top speed, frames to 95% | 11.97 u/f, 46 | 13.998 u/f, 41 | 14, 40 |
| Running jump, held | 260.0 | 312.000 | 312 |
| Double jump (chained) | 345.2 | 346.500 | 346.5 |
| Triple jump (chained) | 738.0 | 550.000 | 550 |
| Ground-pound jump | 260.0 (a normal jump) | 513.500 | 513.5 (measured 513) |
| Long jump | 240.4 | 144.000 | 144 |
| Dive (from a ground pound) | none | 182.000 | 182 |
| Backflip | 585.0 | 496.000 | 496 |
| Sideflip | 284 (Galaxy's turn jump) | 496.000 | 496 |
| Roll (crouch + Spin), boost at frame 30 | none | 20 → ×0.998, boost 22.95 | 20, ×0.998, boost 23 |
| Wall slide (fall per frame after touching) | Galaxy's wall stick | 0 while held, then 0.5, 1.0, ... (+0.5 a frame) | held 3 frames, gravity 0.5 |
| Wall jump (from the slide) | Galaxy's (13 away, 30 up) | 267.000, 8.6 away | 267 |
| Long jump landing, stick held (ground speed on the frames after) | 7.85, then back up to 11.6 by frame 30 | 22.60, slowing 0.117/frame to 19.68 at frame 30 | keeps its speed |
| Dive landing, stick held | stop (belly flop), then a walk from 0 | 18.16 at frame 5, slowing to 15.25 at frame 30 (landed at 19.91) | see below |
| Long jump / dive landing, stick released | stop | stop (Galaxy's landing) | |
| Roll end (below 17, about frame 81, no boost) | none | stick held: 17.0, then 16.5, 15.9, 15.3, ... to 14; released: 11.4, 4.4, 0 (brake skid) | un-roll below 17 |

The long jump and dive values are from the per-frame trace
(`PETARI_ODYSSEY_TRACE=1`: take-off at y 392.21, peak 536.21; dive start
493.71, peak 675.71); the harness now measures from the last grounded position
(the press frame can be mid crouch-slide), and the dive ran into higher ground
after its peak on the observatory terrace. The roll is measured alone
(`PETARI_MOVEMENT_ONLY=roll`) from the start point; at the end of the full
sequence Mario faces a wall. The wall slide and wall jump are measured alone
(`PETARI_MOVEMENT_ONLY=wall`) against an `InvisibleWallJump10x20` added to the
observatory with `native/tools/stage_edit.py` (at 2800, 1700, -3100, `dir_x=-90`:
the object's collision is a flat 1000 × 1990 plane that must be turned upright).

Landings (user feedback, 2026-10-01): with a direction held, a long jump or a
dive lands straight into a run at its landing speed. The run then slows to its
cap at SMO's 14/120 per frame, and the dive's get-up is the failed dive's
forward roll. This is a design decision. The decomp's dive state
(`PlayerStateHeadSliding`) ends when Mario touches the ground, and the code that
picks the next state (`PlayerActorHakoniwa`) is not decompiled. smo.wiki only
says that holding ZL on landing turns the dive into a roll, and the movement
sheet has no landing data. So SMO's belly-slide speed, friction and get-up timing
are unknown. With the stick released, Galaxy's landing stays (a stop).
Measured with the harness's landing tasks
(`PETARI_MOVEMENT_ONLY="dive landing (stick held)"` and so on), each run alone
from the start point.

On Good Egg (EggStarGalaxy, planet gravity) with the mod on, the stage smoke
passes and the same harness measured the standstill jumps 258.000 / 105.000,
the ground-pound jump 513.500, the running jump 312.000 and the triple jump
550.000 (double 341.5: chained from 13.85 u/f, below full speed). The run before
the long jump then ran into terrain, so the later moves are not measured there.

Both games use the same unit size for Mario (about 160 units tall) and run at
60 frames per second, so distances, speeds (units/frame) and accelerations
(units/frame²) carry over directly. **Decision: scale factor 1.0.** Scaling
SMO down by Galaxy's 0.92–0.93 radius/speed ratio would make SMO's jumps miss
Galaxy ledges that Galaxy's own jumps reach; at 1.0, SMO's single jump (up to
312 u) is a bit higher than Galaxy's, its triple jump (550 u) comparable to
Galaxy's (`mJumpHeight[2]` 36), and its long jump flatter. The measurement
harness (section 6) records Galaxy's own heights with the mod off, so this
table can be completed with measured Galaxy numbers.

## 2. How it hooks into Galaxy

- **One switch.** `petari_mod_enabled(OdysseyMovement)`, read once per frame
  into a flag on `Mario`. Every hook is `if (odyssey) { ... }` around new code,
  or chooses between the original statement and the mod's: with the flag off
  the original code runs unchanged, so the game is the same with the mod off.
  The toggle takes effect on the next landing (never mid-air), so a jump is never
  half one model and half the other.
- **A separate, testable model.** The SMO rules live in
  `native/mods/odyssey_move.{hpp,cpp}`: plain functions over a small state
  (front/side/up speeds relative to gravity, extend timer, jump-chain counter,
  per-move timers) and SMO's constants, with no game types. Galaxy's `Mario`
  code calls it at a few points and applies the result to its own velocity
  (`mJumpVec` in the air, the walking speed on the ground). Unit tests drive
  the model frame by frame and compare it with the decomp's numbers (section 6).
- **Gravity-relative, like SMO.** SMO splits velocity into front, side and up
  relative to `al::getGravity(actor)`. The model does the same against Galaxy's
  `getAirGravityVec()` and Mario's front/side vectors, recomputing the frame
  every frame, so arcs follow planet gravity. Galaxy already rotates `mJumpVec`
  with gravity (`cutGravityElementFromJumpVec`); the mod re-derives the up
  component from the current gravity each frame instead of integrating it in
  world space, so a jump around a small planet keeps its SMO height relative to
  the ground under Mario. Gravity-field specifics (point, cube, cylinder, zero
  gravity, gravity flips) stay Galaxy's: the mod only changes *how Mario moves
  within* the gravity Galaxy gives him.

## 3. Moveset map

D = OdysseyDecomp constant/code, C = community measurement. "Hook" is where the
mod changes Galaxy.

| SMO move | SMO numbers | Galaxy today | Hook (mod on) | Phase |
|---|---|---|---|---|
| Run: accel/decel/brake | cap `3 + 11·stick` (14), accel over 40 f, above-cap decay 14/120, brake 1.4 u/f² (D=C) | `mWalkSpeed` 13, inertia constants, Galaxy turning | Ground speed update in `MarioMove.cpp`: SMO cap, accel, decay, brake | 1 |
| Run turning | 6.5–8.5 °/f cap, accel 1/20 (1/5 past 45°) (D+C) | Galaxy `mTurnAngleSpeed*` | Same place: SMO turn-rate limits | 1 |
| Jump, held arc | power 17→19.5 by speed 3→14, gravity 1.5, speed held while A held ≤10 f (D+C); 105–312 u | `mJumpHeight[0]` 22, gravity 1.8, Galaxy hold model | `tryJump` (initial up speed) and `procJump` (extend + gravity) | 1 |
| Double jump | 19.5→21, gravity 1.5 (D) | `mJumpHeight[1]` 26 | same, chain index 1 | 1 |
| Triple jump + chain rules | 19.5→25, gravity 1.0; chain within 10 grounded f, ≤45° direction change, power ≥ 0.99·max (D); 325–550 u | `mJumpHeight[2]` 36; chain counter `_430` with `mJumpConnectTime` 7 | `tryJump`: SMO chain rules decide `_430` 0/1/2 | 1 |
| Air control (vectoring) | accel 0.5 forward / 1.0 back / 0.3 side relative to the jump's start direction; cap max(v₀,14), start min(v₁,24) (C) | Galaxy air walk (`doAirWalk`) | `procJump` horizontal update | 1 |
| Midair turning | soft cap 1.5–6 °/f, quickturn 25 °/f at ≥135° (D+C) | Galaxy air turning | `procJump` facing update | 1 |
| Fall speed cap | 35 (D) | `mMaxDropSpeed[0]` 35 | none needed (already equal) | — |
| Long jump | up 12, gravity 0.48, start min(v+4, 14), cap 23, accel 0.25/0.5/0.25 (D=C); 144 u high | Galaxy long jump (crouch + jump while moving: `trySquatJump`, `mSquatJumpFrontSpeed` 16.5) | `trySquatJump` (moving branch) + its air update | 2 |
| Ground-pound jump | jump within landing frames 5–30, up 40, gravity 1.5 (D); 513 u | not in Galaxy | `procHipDrop` landing / `doLanding`: A in the window starts a jump with power 40 | 2 |
| Ground pound | stall, then 45 u/f, cap 45 (D) | `procHipDrop`, `mGravityHipDrop` 12, `mLimitSpeedHipDrop` 150 | fall speed to SMO 45 | 2 |
| Backflip | up 32, gravity 1.0, back 5, horizontal cap 9 (D); 496 u | `tryBackJump` (crouch + jump standing) | `tryBackJump` initial velocity + air update | 3 |
| Sideflip | up 32, gravity 1.0, horizontal 9, accel 0.25/0.5/0.075 (D); 496 u | `tryTurnJump` | `tryTurnJump` initial velocity + air update | 3 |
| Dive | horizontal 20, up 28, gravity 2.0, brake 0.5, side 0.125 (D); +182 u | not in Galaxy | new: in the air, during a ground pound's stall, Spin starts a dive (SMO: GP then Y). Uses the hip-drop state's start, then the mod's dive air update; land into a belly slide (Galaxy's slide animation) | 4 |
| Roll | start ≥20 (GP roll 30), max 35, decay ×0.998, ends <17, boosts to 23/26 with ≥15 f between (D+C) | not in Galaxy (closest: Galaxy's slide on slopes) | new: crouching + Spin on the ground starts a roll (SMO: crouch + Y); Spin while rolling boosts; A while rolling is a long jump. The speed is set in `updateWalkSpeed` and Galaxy's walking steers it; the start and each boost play Galaxy's dive-landing forward roll | 5 |
| Wall slide / wall jump | slide gravity 0.5; jump up 23, gravity 0.95, horizontal 8.6, input lockout 25 f (D) | `MarioWall`, `tryWallJump` (`mWallJumpPowerXZ/Y` 13/30), Galaxy wall stick | `tryWallJump` velocities and lockout; `moveWallSlide` speed | 6 |
| Spin | Galaxy's Star Spin stays | Spin (shake / F): attacks, Launch Stars, Pull Stars, Spin Drills | **unchanged.** It is essential to Galaxy's levels | — |
| Spin jump | SMO's comes from a stick spin (rotate twice) | Galaxy spin jump (Spin in the air) | unchanged | — |
| Crouch | ×1.2 on entry, ×0.95/f slide to 3.5, crouch-walk 3.5 (D) | Galaxy squat | ground speed hook while crouching | 1 |
| Swim | `Swim*` constants (D) | Galaxy swimming | **unchanged in v1.** Galaxy's water levels are tuned around its swim | — |
| Cap throw / Cappy | Cappy actor, cap bounce, vault, return jump | — | **left out of v1** (see below) | — |

### Cappy

Left out of v1. SMO's cap throw needs a Cappy actor with its own flight and hit
detection (`HackCap`, 189 functions, not decompiled, and its throw parameters are
unpublished), plus captures, which have no meaning in Galaxy's levels. Galaxy's
Star Spin already fills the "Y button" role (attack, collect, Launch Stars), and
Galaxy's levels depend on it. A cap-bounce stand-in (a spin in the air after a
dive giving the dive cap-bounce's 22 u/f, gravity 1.0, once per airtime) is a
possible v2, kept out of v1 so the spin keeps behaving as Galaxy's levels expect.

## 4. Inputs

No new buttons. SMO's actions map onto Galaxy's existing bindings, which keeps
every Galaxy control working and needs no remapping:

| SMO input | Galaxy binding (keyboard / controller default) | Used for |
|---|---|---|
| A/B (jump) | Space / A | all jumps, ground-pound jump, wall jump |
| ZL/ZR (crouch) | Shift / ZL·ZR-equivalent (Galaxy's Z) | crouch, long jump (run + crouch + jump), backflip, ground pound, roll start |
| Y/X (cap) | F / the Spin binding (Wii shake) | dive (during a ground pound), roll (while crouching), roll boost; otherwise Galaxy's Star Spin |
| Left stick | WASD / left stick | movement; stick-back + jump for the sideflip |

The pointer (mouse / right stick-style pointer) and Star Bits are untouched:
they use the pointer and its buttons, which the mod does not read. Galaxy's
Spin keeps every meaning it has in Galaxy unless one of the two specific
combinations (ground pound + Spin, crouch + Spin) is pressed, and those are
inputs that do nothing useful in Galaxy today.

## 5. Gravity and planets

SMO's code is written against a gravity vector, not world up, so it maps
directly: every speed is kept as front/side/up relative to the current gravity
and Mario's facing, and turned back into a world velocity each frame. On a
small planet the up axis turns as Mario moves, so a long jump around a
Good Egg planet curves with it, as Galaxy's own jumps do. Two Galaxy-specific
rules stay Galaxy's: the gravity change when Mario crosses into another field
(the mod re-derives its frame next frame), and Galaxy's terminal speed toward
very strong gravity fields (both games cap falling at 35).

## 6. Verification plan

1. **Model tests (no game).** A unit test integrates the model frame by frame on
   flat gravity with scripted inputs and checks it against the decomp-derived
   values (all of these follow from the constants; the research notes show the
   community measured the same numbers):

   | Input | Expected |
   |---|---|
   | Jump from standstill, tap / hold | 105 u / 105 + 9·17 = 258 u |
   | Jump at full speed, tap / hold | 136.5 u / 312 u |
   | Double, held, full speed | 346.5 u |
   | Triple, tap / held | 325 u / 550 u |
   | Backflip, sideflip | 496 u |
   | Long jump | 144 u high, start min(v+4,14), cap 23 |
   | Ground-pound jump | 513 u |
   | Dive | +182 u |
   | Run | cap 14, decay 0.1167, brake 1.4 |
   | Chain | broken after 10 grounded frames or a >45° turn |

   Tolerance: exact to 0.01 u (the integration is deterministic).
2. **In-game measurement harness.** A smoke script (`PETARI_SMOKE=movement`)
   in a flat area (the observatory's flat terrace, gravity straight down) plays
   scripted inputs through the normal input layer and records Mario's height
   above the start (along gravity), peak frame and speed per frame, with the mod
   **on** (must match the model within 1%, the difference coming from Galaxy's
   collision and ground snap) and **off** (Galaxy's baseline, recorded for the
   scale table, and a regression that the mod-off numbers never change).
3. **Playability.** Background runs with the mod on: the Good Egg stage smoke
   (planet gravity, walking, jumping, Launch Stars), the observatory, and a
   small-planet stage. Pass criteria are the smoke's usual ones (loaded, moved,
   no crash/hang/missing references) plus no fall-outs during the scripted moves.
4. **Mod off stays the same.** Every existing smoke and dome-tour test runs with
   the mod off as before.

## 7. Order of work

1. Mod plumbing (`OdysseyMovement` in `mods.hpp`, Mods page, `mods.txt`) and the
   model with run, jump arcs, chain, air control, gravity, with model tests.
2. Long jump and ground-pound jump (and SMO's ground-pound fall).
3. Backflip and sideflip.
4. Dive.
5. Roll and roll boosts.
6. Wall slide and wall jump.

Each step: model tests, then the in-game measurement for that move, then a
playability run, before the next step.

## Known limits (v1)

- SMO's run-turn and air-control code is not decompiled; the mod follows the
  community description, so small differences from real SMO are possible.
- Slope walking in SMO is not well measured; the mod keeps Galaxy's slope
  behaviour except for the speed cap.
- Animations are Galaxy's; some SMO moves (dive, roll) reuse the nearest Galaxy
  animation, checked with XFB dumps (`PETARI_XFB_DUMP`; the harness marks each
  move's name as the dump label): the dive uses Galaxy's high dive into water
  (upright while rising, head-first on the way down, belly landing); the roll
  restarts the failed-dive landing's forward roll every 12 frames (back to back),
  and when it ends runs on with a direction held (Galaxy's run target is kept
  following the stick during the roll) or plays Galaxy's brake skid; the long jump, backflip, sideflip, ground-pound jump, wall
  slide and wall jump use Galaxy's matching poses. No T-poses or wrong facing.
- No ground-pound roll (SMO: Y on a ground-pound landing, 30 u/f) and no roll
  speed gain on downhill slopes; the roll ends on a turn slide (stick reversed
  after release) like Galaxy's run.
- The wall slide replaces Galaxy's wall stick speed only: where Mario can cling
  is still Galaxy's rule (falling, stick toward the wall, a wall Galaxy allows).
  Holding the stick away lets go after 15 frames; Galaxy's 180-frame release is
  not used. SMO's slide state itself is not decompiled; the slide uses its
  PlayerConst values (WallKeepFrame 3, GravityWallSlide 0.5, FallSpeedMax 35).
- No Cappy, cap bounce or captures; swimming is Galaxy's.

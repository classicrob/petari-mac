# Odyssey-style camera (mod `OdysseyCamera`)

Status: implemented, 2026-10-01. Off by default; with the mod off no code path below
runs and the game is byte-identical.

## Goal

A player-controlled orbit camera like Super Mario Odyssey's, wherever Galaxy
uses an ordinary follow camera:

- right stick, or mouse with the middle button held, orbits around Mario;
  the scroll wheel (or arrow up/down) zooms; the camera stays where it was put;
- it eases back behind Mario's movement after a few seconds without camera input,
  and C recentres it at once;
- on planets the orbit is relative to Mario's gravity (it rides around the
  sphere with him);
- walls and terrain pull it in instead of clipping through;
- sensitivity and invert X/Y options.

Scripted cameras stay scripted: cutscenes, boss intros, talk cameras, launch
star and cannon flights, 2D/2.5D sections, fixed and rail cameras, first person.

## Galaxy's camera, as it matters here

Per frame (`SceneExecutor`: camera movement runs before the player):

1. `CameraDirector::movement` (`src/Game/Camera/CameraDirector.cpp:115`)
   - `updateCameraMan`: the current manager (`CameraManGame` in normal play,
     `CameraManEvent` for scripted events, `CameraManSubjective` for first
     person) calls its camera type's `calc()`.
   - `calcPose`: `OnlyCamera` smooths toward the ideal pose and copies it to
     `mPoseParam1` (eye `mPos`, target `mWatchPos`, `mUpVec`).
   - `createViewMtx`: builds the view matrix from `mPoseParam1`, then
     `CameraViewInterpolator::updateCameraMtx` applies blending, repulsion areas
     and the map collision sweep (`calcCollision`), and sets the scene view.
2. The renderer (`MR::loadViewMtx`) and Mario (`MarioActor::updateCameraInfo`,
   `Mario::calcMoveDir`: stick relative to the camera, projected on the gravity
   plane) read that view.

Camera types (`CameraHolder.cpp` `sCameraTable`) are chosen per area by
`CameraManGame::selectCameraChunk` (cube camera areas, the ground triangle's
camera ID, start/zoom cameras). C and the D-pad are polled by each type
(`CameraLocalUtil` `tryCameraReset`, `testCameraPadTriggerRoundLeft/Right`).

## Where the orbit goes in

**Hook:** `CameraDirector::movement`, between `calcPose()` and `createViewMtx()`.
When the orbit is active it overwrites `mPoseParam1`'s eye, target and up.
Everything downstream is unchanged and consistent: the view matrix, collision in
`CameraViewInterpolator`, `MR::getCamPos`/`getCameraWatchPos`, the renderer and
Mario's camera-relative stick. The game's own camera keeps running underneath
(`CameraManGame`/`OnlyCamera` state), so when a scripted camera takes over the
handoff is to the game's camera as if the orbit had never been there.

**Active only when all hold** (checked each frame):

- mod on, scene in normal play: current manager is `CameraManGame` (not event or
  subjective: `!MR::isEventCameraActive()`, not first person), `!MR::isDemoActive()`,
  player alive and not bound (launch stars, cannons, pipes and Pull Star rides are
  binds or events);
- the active `CameraManGame` type is a follow-style camera:
  FOLLOW, TOWER, WONDER_PLANET, MEDIAN_PLANET, MEDIAN_TOWER, CUBE_PLANET,
  XZ_PARA, SLIDER, WATER_FOLLOW, WATER_PLANET, WATER_PLANET_BOSS, INWARD_TOWER,
  INWARD_SPHERE, TWISTED_PASSAGE, INNER_CYLINDER, RACE_FOLLOW;
- not 2D/2.5D (`CAM_TYPE_2D_SLIDE`, or Mario's 2D movement flags).

Everything else (fixed, rail, tripod, demo, talk, black hole, Foo Fighter, anim,
DPD, subjective) stays the game's camera. On entering the orbit, its yaw, pitch
and distance are seeded from the current game camera pose, so there is no jump.

## Orbit model

State: a unit direction `d` from target to eye, a distance `r`, both in world
space, and the previous up vector.

- Target: Mario's feet plus `up * 180`, minus the jump absorb (see Values).
- Up: `-gravity` at Mario (`MarioActor` gravity vector), smoothed.
- **Gravity-relative:** each frame `d` is rotated by the minimal rotation from the
  previous up to the new up (parallel transport), so walking around a planet
  carries the camera with Mario instead of leaving it fixed in world space.
- **Input:** yaw rotates `d` about `up`; pitch rotates it about the camera's
  right axis, clamped to -60..+70 degrees from the tangent plane. Rates: right
  stick up to 180 deg/s (times sensitivity), mouse 0.25 deg/point (times
  sensitivity); turns are limited to 20 deg per frame so Mario's camera-turn
  snap (`updateCameraInfo`, more than 30 deg) never fires.
- **Zoom:** `r` in 400..2400, default seeded from the game camera; wheel/arrows
  step it, eased.
- **Auto-follow:** after 2 s without camera input, while Mario moves, the yaw
  eases toward behind his velocity at up to 45 deg/s (never while he moves
  toward the camera, to avoid fighting the stick).
- **Recentre (C):** ease yaw to behind Mario's facing and pitch to 15 deg over
  ~0.3 s. The game's own C reset still runs underneath, which is harmless.
- **Collision:** a line from target to the desired eye
  (`MR::getFirstPolyOnLineToMap` with the `MR::isCameraCodeThrough` filter, as
  `CameraFollow` uses, so grates, foliage and glass don't block) shortens `r` to
  the hit minus 60 units; it eases back out when clear. If the hit is the floor
  below the target and would bring the eye closer than 320 units (into Mario),
  the pitch is raised in 5 degree steps (up to 60) until the floor no longer cuts
  the view, and kept there. The game's own `CameraViewInterpolator` collision
  then runs on the result as usual.

## Values: SMO-derived vs tuned

Super Mario Odyssey's own follow camera (`CameraPoserFollowLimit`,
`CameraAngleCtrlInfo`, `CameraArrowCollider`) is not decompiled yet in
MonsterDruide1/OdysseyDecomp (headers only, checked 2026-09-30), and its per-stage
distances and angles live in game data files that are not publicly documented. So:

| Value | Here | Source |
| --- | --- | --- |
| Look-at height above Mario's feet | 180 | SMO: `CameraOffsetPreset.cpp` "Default" (0, 180, 0) |
| Jump absorb: target holds its height in the air within -200..+480 of Mario, follows high jumps (> 35 units/frame up), leftover fades x0.8 per frame on landing | as SMO | SMO: `CameraVerticalAbsorber.h/.cpp` defaults (interpretation of the screen-position band as world units is ours) |
| Speed levels 1-5 | x0.44, 0.72, 1.0, 1.27, 1.55 | SMO: `getStickSensitivityScale` levels -2..+2 (its level 0 reads 1.6 in the decomp; we use 1.0 at the middle) |
| Invert X/Y default | off | SMO: `GameConfigData` `IsCameraReverseInputH/V` |
| Base stick rate (speed 3) | 3.6 deg/frame horizontal, 2.4 vertical | tuned |
| Distance range, default | 400-2400, seeded from Galaxy's camera | tuned (SMO's is per-stage data) |
| Pitch limits | -60..+70 deg | tuned |
| Auto-follow | after 2 s idle, 0.75 deg/frame | tuned |
| Recentre | 18 frames (0.3 s) to behind Mario, 15 deg up | tuned (matches the 0.3 s fan re-creations use; not measured) |
| Collision | line of sight, 60 margin, pull in at once, ease out 10%/frame | tuned (SMO's `CameraArrowCollider` not decompiled) |
| Floor lift | pitch raised in 5 deg steps when the floor would pull the eye within 320 | tuned (live sweeps 2026-10-01: without it the eye ended inside Mario in Good Egg) |
| Turn cap | 20 deg/frame | Galaxy: Mario re-frames his stick above 30 deg/frame |

## Input

| Input (mod on) | Action (remappable in controls.txt) |
| --- | --- |
| Right stick | orbit (x yaw, y pitch). The stick stops moving the Star Pointer; the mouse still does |
| J / L, I / K | orbit left/right, pitch up/down (`CameraOrbitLeft/Right`, `CameraPitchUp/Down`) |
| Q / E, arrow left/right | orbit left/right (the game's D-pad step rotation keys) |
| Middle mouse or Command held + mouse motion | orbit; the pointer stays put (`CameraOrbitHold`) |
| Two-finger trackpad / Magic Mouse scroll | orbit (horizontal yaw, vertical pitch) |
| Mouse wheel clicks, trackpad pinch, Z / X | zoom (`CameraZoomIn/Out`; camera.txt `ScrollMode=auto|orbit|zoom` decides what scrolling does) |
| C | recentre |

Host side: the input layer accumulates orbit/zoom deltas (`Input::takeCameraInput`),
consumed once per frame by the game hook. With the mod off nothing is
accumulated and the right stick drives the pointer as before.

Settings (Mods page -> Odyssey camera...): on/off, speed 1-5, invert horizontal,
invert vertical; saved in `camera.txt` next to `mods.txt` (`OdysseyCamera`,
`Speed`, `InvertX`, `InvertY`, `ScrollMode`). `PETARI_ODYSSEY_CAMERA=1|0`
overrides on/off for a run. The camera and Odyssey movement mods are independent:
either works alone, or both together.

## Live checks

`PETARI_CAMERA_TEST=1` (with the mod on) drives the camera while the orbit is
active: a collision sweep first (farthest zoom, pitch to the floor, one full
turn; XFB label `camera-sweep`), then orbit, pitch, zoom, drag and recentre.
`PETARI_CAMERA_TEST_DELAY=N` waits N orbit frames first, so a smoke's
camera-relative movement checks finish before the camera turns.
`PETARI_CAMERA_TRACE=1` logs every 60 frames: distance, wanted distance, how many
frames the line of sight was blocked, the nearest allowed distance, elevation.
Script: `build/camera/run3.sh` (stage smoke, 1200-frame delay, 1500-frame tail).

## Risks

- Areas whose follow camera is designed as a fixed framing (some towers) will now
  orbit freely; a level designer's view is one C press away.
- Mario reads the camera for his stick: the orbit changes "forward" exactly as the
  player sees it, which is the point, but the game's automatic camera turns no
  longer steer it while the orbit is active.
- Handoffs to scripted cameras snap to the game's camera (the game's own blend
  starts from its own pose, not ours); acceptable for a first version.

## Photo mode

A detached free camera over the frozen game (player docs: native/MODS.md
"Photo mode"; setting `PhotoMode` in `camera.txt`, independent of the orbit).

- **Freeze:** a native `GameScene` nerve, `PhotoMode`, entered from
  `GameScene::update` only in `GameSceneAction` with `isPermitToPauseMenu()`
  (no demo, talk, wipe or death). Like `exePauseMenu`, it runs no movement list.
  So nothing moves: Mario, enemies, timers, the camera director, clipping. Audio
  and rumble pause as for the pause menu (`AudSystem::enterPauseMenu`,
  `onPauseBeginAllRumble`). `calcAnim`/`calcViewAndEntry` still run every frame, so
  the scene is drawn from the new view. Leaving goes through
  `setNerveAfterPauseMenu`, as the pause menu does.
- **View:** `PhotoCamera` (src/Game/Camera/PhotoCamera.cpp) saves the camera
  context's view matrix and fovy on entry. Each frame it sets its own with
  `MR::setCameraViewMtx`/`MR::setFovy`. On leaving it puts the saved ones back
  before any movement runs, so the game's next frame starts from its own view.
  Yaw turns about the camera's up on entry (planets keep their horizon), and pitch
  stops short of straight up or down.
- **HUD:** `GameScene::draw` skips `draw2D` and `GameSystemObjHolder::drawStarPointer`
  skips the Star Pointer while active. Screenshots therefore have no HUD and no
  cursor.
- **Input:** host side (wpad_host.cpp). From the `PhotoMode` press until the game
  leaves, every input goes to the free camera and none to the game. Entering
  releases what the game saw held (as on a focus loss). A press the game cannot
  take is answered at once (`petari_photo_set_active(0)`). A press no scene
  answers (file select, movies) lapses after 0.3 s and is not carried over.
- **Screenshot:** `petari/screenshot.hpp` is a one-shot request. The presenter
  reads the next XFB back (the same path as `PETARI_XFB_DUMP`) and writes a PNG at
  the renderer's resolution: `[photo] screenshot saved: <path> (WxH)`.
- **Known limit:** objects the game's clipping had hidden from its own camera stay
  hidden. Running the clipping director would change actor state (some actors
  reset when they come back into view), which leaving must not do.
- **Check:** `PETARI_PHOTO_TEST=N` (app_main.cpp) enters after N gameplay frames,
  flies, looks, zooms, takes two shots and leaves. `[photo] enter:` and
  `[photo] exit:` log the scene frame count, Mario's position and velocity,
  coins, Star Bits, a hash of the view matrix and fovy; they must match. Script:
  `build/photo/run.sh`.

# Native gameplay playtest — 2026-09-29

These are full-app tests on the extracted RMGE01 disc, using the public native
input layer, game UI targeting, and read-only game-state observations. They do
not teleport Mario or change the game's state to advance the test. Desktop
computer control was unavailable for app27/app28, so those runs were not visual
inspection. The subsequent rendering investigation below uses desktop captures.

## Existing save: app27

`PETARI_SMOKE=reload PETARI_SMOKE_FRAMES=36000`, isolated user directory
`build/playtest-reload-27`, log `build/native-boot-27.log`.

- Loaded a previously created Mario slot without creating another file.
- Advanced the five prologue pages and Peach's letter with normal A presses.
- Idle: no displacement in 120 frames, on the ground.
- Jump: rose 203.5 units and landed after 36 frames.
- Up/down: moved about 373 and 362 units; direction cosine -0.999365.
- Pause opened on a Plus hold. Movement input caused zero displacement while paused.
- Resume closed the menu; movement then covered 360.48 units.
- PASS at frame 4699; normal power-off returned exit status 0.
- SHA-256 hashes of GameData.bin and banner.bin matched the pre-run copies.

## Fresh save: app28

`PETARI_SMOKE=gameplay PETARI_SMOKE_FRAMES=36000`, isolated user directory
`build/playtest-fresh-28`, log `build/native-boot-28.log`.

Created a Mario file, completed both saves, advanced all five prologue pages and
the letter, then passed idle, jump/landing, opposite movement, pause/freeze, and
resume/movement checks. Jump height was 203.5 units; movement after resuming was
360.38 units. PASS at frame 5411, followed by normal power-off and exit status 0.

Component verification: `native_app_smoke`, `native_app_seam`, and `native_input`
pass in the root build. The 137 smoke checks and 27 seam checks also pass
under ASan/UBSan.

## Bug reproduced and fixed

App26 performed the fresh-file path, idle, jump and opposite movement checks,
then failed to pause. The default Escape binding sent Plus and B together. The
game's pause checker requires Plus held for 12 frames while A/B are up, so B
prevented pausing. Its sampled state confirmed Plus held, B held, operating yes.

Escape now sends only Plus; Backspace sends B for backing out of menus. Hold
Escape briefly to open pause, then tap it to close. The real KPAD input regression
checks that Escape keeps A/B up throughout a hold and that Backspace sends B
without Plus. App27 verified the fix in the game.

## Limits

The opening and these controls are covered. Later stages, camera controls, spin,
motion-controlled activities, movie presentation, and overall visual fidelity
have not been established by these runs. The next route toward the Bowser attack
has been researched from stage placement data but has not been playtested.

## Rendering and device-audio investigation

The manual playtest exposed defects that the input/state smoke tests could not
see: a solid-blue title background, a white/pink logo reaction, missing animated
text, and irregular sound.

- **Depth clears:** Aurora's `GXSetZTexture` is unimplemented. The game's clear
  quads therefore wrote their vertex depth (near) instead of the constant
  `0xffffff` texture depth (far), rejecting later geometry. The native clear
  routines now place these quads on the orthographic far plane. Wii paths are
  unchanged. Desktop captures confirmed the starfield and planet reappeared
  with ordinary depth testing enabled.
- **Animation conversion:** `__OSf32tou8` and `__OSf32tos16` had no native body,
  returning uninitialized values. Layout and particle animations use these for
  opacity and color. Native implementations saturate and truncate, with scalar
  regression coverage for negative values, fractions, and both range limits.
- **Device audio pacing:** SDL requests can span multiple AI DMA blocks. Pulling
  the whole request immediately could relatch a block before the game's audio
  thread prepared its successor. A paced producer now feeds a bounded stereo
  ring; SDL only drains it. The DMA-spacing regression passes at 32/48 kHz and
  fails against the old sink (successive interrupts 0.000 ms apart).

These fixes do not establish fidelity of every effect, level, or sound. Device
playback still needs listening confirmation; the timing test does not measure
perceived noise.

Verification after the fixes:
- Desktop inspection: title background, animated A+B prompt and copyright text
  are visible; clicking B produces a soft blue glow rather than the opaque
  jagged white/pink patch.
- `native_core`, `native_audio_sdl`, `native_app_smoke`, `native_app_seam`: 4/4 pass.
  The conversion regression fails against the previous SDK header.
- Fresh full-app `PETARI_SMOKE=gameplay`: PASS at frame 5398, normal shutdown
  exit 0. Log: `build/visual-fix-gameplay.log`. This includes file creation,
  prologue, idle, jump/landing, opposite movement, pause/freeze, and resume.

## Follow-up fidelity checks

- Generated collision now has a typed initialization path, preserving its mutable
  descriptor and explicit triangle count. Its octree leaf is written in host
  order. The root generated-collision and asset tests pass, including 22,293
  triangle queries over 150 disc collision files. This addresses the manual
  playtest crash that treated runtime-generated collision as unconverted disc data.
- Actor lights previously left their direction uninitialized. A live debugger
  read found directions around 2.3e20 in lights 0 and 1, large enough to overflow
  the spotlight calculation. Native light objects now start initialized. The
  optimized regression passes. Desktop inspection confirms the white wash is
  gone. A second bug kept the eyes closed: `getMaterialAnm()` rejected all host
  pointers above 4 GiB using a Wii address test. The native accessor now rejects
  only the original 32-bit sentinel range. Desktop inspection confirms open
  blue irises. Both root material-animation tests pass, covering texture swaps,
  TEV colors, material colors and texture transforms with real disc resources.
- The normal 1280x720-point Retina window already renders at 2560x1440. GPU
  tests show a hard edge at its native texel and a 1:1 presentation scale. The
  prologue illustrations are 608x224 source textures, and many UI icons are
  24–64 pixels; raising render resolution does not add detail to those assets.
  Presentation now fits the render target to the displayed aspect when the
  window has a different shape, avoiding an extra resampling step. The GPU
  regression covers 4:3 and 16:9, resizing, and replacement of stale copies.

The integrated reload run (`build/fidelity-reload.log`) passed at frame 4702
and exited normally with status 0. Idle, jump/landing, opposite movement, pause
freeze and resumed movement passed. The isolated NAND files remained identical
by SHA-256. Across 83 device-audio reports there were zero output underrun
frames, but 17 AI block replays; audio fidelity is not yet established. Replays
cluster around slow frames, so scheduling diagnostics are the next check.


## Audio scheduling follow-up

The first pacing revision eliminated output-ring starvation but still repeated
DMA blocks. CPU-wait samples identified two separate causes:

- A lower-priority thread could be assigned the simulated CPU while its host
  thread was still asleep. An audio interrupt then waited for that sleeper to
  wake before it could yield. The scheduler now immediately redirects an
  unstarted dispatch to the highest-priority ready thread. A deterministic
  dispatch regression fails without this change; OS tests pass in Release,
  ASan and TSan.
- After that fix, `build/fidelity-scheduler-reload.log` passed the gameplay
  checks with zero underruns over 79 reports, but five replays remained during
  a sampled 78.2 ms `FileRipper::decompressSzsSub` call. The native decoder now
  offers higher-priority preemption between groups every 16 KiB of output,
  preserving Wii code and decode state. Root game-data checks pass on 311
  archives, 55 uncompressed files and 16 streamed archives requiring refills.

The sink now refills to a level based on whole device requests, with catch-up
bounded by elapsed time; it never discards pulled samples. The producer and AI
interrupt thread use real-time scheduling, with higher QoS for the DSP and
high-priority OS threads. Buffer latency is roughly 64 ms plus the device's
largest request at 32 kHz. Diagnostics distinguish underruns from repeated AI
blocks (`PETARI_AUDIO_DIAG=1`); optional CPU-wait sampling uses
`PETARI_BATON_DIAG=1`. The diagnostic mutex has process lifetime so its detached
reporter remains safe during normal exit.


Final integrated run: `build/fidelity-final-reload.log`, rebuilt with both
scheduler and decoder fixes. The existing-save gameplay sequence passed and
exited 0. All 79 audio reports had zero underrun frames; one AI block replay
occurred during startup, with none subsequently reported through title, file
load, prologue and gameplay. No CPU-wait report exceeded the diagnostic's 5 ms
reporting threshold. The isolated NAND hashes remained unchanged. This shows
the measured sustained distortion mechanisms are corrected on this route; it
does not establish perceptual audio fidelity or zero stalls in untested levels.

Desktop capture during the arrival sequence confirms white Toad caps with
red/green/blue/yellow spots and distinct clothing colors, without the previous
gold wash. Mario has his red cap and blue overalls. The earlier file-select
capture confirmed open blue irises. These are visual spot checks, not a
pixel-level comparison against Wii output.


The one startup replay is expected silent JAudio2 initialization, not a late
production block: `JASDriver::initAI` clears all DAC buffers and starts buffer 2;
`lastRspMadep` is initially null, so the first `updateDac` interrupt registers
no successor. The zeroed initial buffer therefore plays twice, as on the Wii.
Subsequent interrupts register the prepared buffer. The diagnostic intentionally
counts this event rather than masking it. No unexpected replay was measured
in the final route; listening remains a separate fidelity check.

## Longer-route diagnostics

For timing measurements, use `PETARI_BATON_DIAG=1 PETARI_BATON_SAMPLE=0`
with `PETARI_AUDIO_DIAG=1`. Disabling the stack sampler avoids suspending the
thread being measured. Reports include holder CPU time, interrupt-disable
counts, and a mid-wait host run-state sample. The sample describes one instant,
not the entire wait. Mach CPU accounting may lag a scheduling slice.

With sampling enabled, the diagnostic also reports the measured sampler pause;
its intervention can lengthen the wait. Early timing magnitudes were collected
while two stale TSan test processes were consuming CPU. Those processes were
stopped; do not use those magnitudes as clean-machine benchmarks.

See `RELIABILITY.md` for the expanding story-route coverage and open findings.

### Manual assistance in automated runs

Physical presses and releases of keys or mouse buttons bound to game controls
are now recorded separately from the driver's injected input. A completed run
with such input reports `ASSISTED` and exits 3; it is not an unattended PASS.
Failures and blocked runs retain exit 1 and 2 and note any assistance. The log
records the frame, phase, position, event count and latest event in the first
observed input batch. Pointer motion and focus changes are logged separately;
they do not steer the story route and do not alone change its result.

A playtest observer supplied a jump and steered around a wall in story run 6
(screen recording). That run establishes HeavensDoorGalaxy loading and
heap headroom, but not autonomous traversal. The revised castle route avoids
the curb and building using a collision-derived path with stricter step and
clearance limits. Story run 7 then completed the revised route with no physical gameplay input,
reached HeavensDoorGalaxy for 60 consecutive ready frames at frame 21006, and
exited 0. It recorded 64 pointer movements and 10 focus changes. Three automatic
sidestep recoveries succeeded; no manual jump or steering was needed. Both NAND
files remained byte-identical to the isolated fixture before the runs.


### Latest synchronization run

Story run 9 (`build/proactive-story-9.log`) passed at frame 20242 with no physical
gameplay input and no stuck recoveries. Both movies played, HeavensDoorGalaxy
stayed ready for 60 frames, and both isolated NAND hashes were unchanged.
Its 342 audio reports contained zero underrun frames, one expected silent
startup replay and two unexpected replays. Audio therefore remains an open
reliability item despite the route pass. See `RELIABILITY.md` for timing evidence
and coverage limits.

The AI registration lock now uses owner-aware blocking instead of busy spinning.
The scheduler temporarily requests higher host QoS for a running baton holder
when a higher-priority game thread waits; it ends the override after unlocking.
Audio diagnostic formatting runs on a joined reporter thread rather than the
real-time producer. `[audio-host]` records producer and callback interval stats;
these help localize future failures without asserting their cause.


### Observatory and Good Egg fixture

The `galaxy` smoke loads an isolated post-tutorial fixture, walks from the
observatory to the Terrace, points at the Pull Star, selects Good Egg Galaxy
and mission 1 through the normal UI, then checks movement, jumping and
pause/resume. The native UI hooks only publish visible targets; the driver
uses the normal input bindings. This is not a full mission completion test.
The route has simulated coverage; a live unattended pass is still pending.

Create a fresh fixture from an existing saved-file seed (source is read only):

```sh
python3 native/tools/create_observatory_fixture.py --source PATH_TO_SEED_USER --output NEW_TEST_USER
PETARI_SMOKE=galaxy PETARI_SMOKE_FRAMES=108000 PETARI_TRACE_BOOT=1 \
  build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari \
  --disc build/game-data/RMGE01 --user NEW_TEST_USER --test-fixture observatory
```

The explicit fixture flag requires a marker in that isolated directory and
refuses the normal user directory. At reload it supplies the tutorial Grand
Star and completed observatory introduction on both game-data snapshots.
This bootstrap is not earned progression and does not validate those skipped
sequences. Good Egg progress is otherwise unchanged. Physical bound input
still makes an otherwise successful run `ASSISTED` (exit 3).

The first observatory attempt entered the Terrace but later aborted while
compiling a shader whose indirect texture order referenced a disabled texture
coordinate. The native Aurora patch maps that indirect coordinate to coordinate
0, matching Dolphin's handling, in both shader analysis and generation. Valid
coordinates remain unchanged. The exact failing configuration and all 454
captured GX configurations now pass source generation; this alone does not
validate their Metal compilation or rendered appearance.

Known cached pipelines now finish compiling before the game starts. The window
title reports remaining work and closing the window cancels startup. New
configurations encountered during play can still compile on demand. An optional
`-DPETARI_PIPELINE_SEED=/absolute/path/to/seed.db` CMake setting bundles a curated
Aurora pipeline-configuration database as `Resources/initial_pipeline_cache.db`.
Export a live cache with SQLite's backup API, not a raw copy of its database
file while WAL writes may be active. Do not substitute the device-specific Dawn
cache. No seed is required by default.

### Unlocked saves

`build/saves/all-missions` (Mario, 120 stars) opens every dome, galaxy and
mission through the normal observatory UI. `complete-luigi` and `grand-finale`
add Luigi and the Grand Finale Galaxy. Launch with `--user build/saves/VARIANT`,
or use a copy of it. It needs no fixture flag. The game's own code makes and
checks these saves. See [SAVES.md](SAVES.md) for what each save contains, how
it is made and verified, and how to copy one into your normal save directory.
The observatory fixture's bootstrap now only raises story progress. With
`--test-fixture stage`, a copy of an unlocked save keeps its full progression.

### Human-style macOS window playtest

Use the normal keyboard/mouse route, with no `PETARI_SMOKE` driver. Prepare a new
isolated fixture once, then launch each bounded session through the helper:

```sh
python3 native/tools/cu_playtest.py --prepare
# Freeze the current app (with no build running):
ditto build/macos-gx/native/app/Petari.app build/cu-playtest/frozen/Petari.app
shasum -a 256 build/cu-playtest/frozen/Petari.app/Contents/MacOS/Petari
python3 native/tools/cu_playtest.py --session 1 --app build/cu-playtest/frozen/Petari.app
# After clean quit, use the same isolated save to check persistence:
python3 native/tools/cu_playtest.py --session 2 --app build/cu-playtest/frozen/Petari.app
```

The helper owns `build/cu-playtest/user`,
launches only the app binary with that directory and the observatory marker,
and limits each actual app session to 25 minutes (20 minutes by default).
Run timing measurements on an otherwise idle machine, one app at a time, with
no concurrent builds. It records binary/save hashes,
PID, UTC times, exit status, boot/audio logs and frame CSV. It enables baton
diagnostics with `PETARI_BATON_SAMPLE=0` to avoid sampling-induced pauses. A forced deadline exit
is reported as failure, not a clean quit. `--prepare` refuses to overwrite an
existing fixture; preserve prior evidence before selecting a new output location.

After `build/cu-playtest/active-session.json` appears, confirm that PID is still
running. Paste `native/tools/cu_playtest_repl.js` into `cua_repl` after reading its
first-use documentation, then `await playtest.bind()`. Bind again after **every**
relaunch; do not ask CUA to open the app while waiting for the launcher. CUA's
`getApp` can launch an app if none exists. Do not use `Play Petari.command`.

The helper's `tap(key,count)`, `click(observedTarget,button)`, `drag(from,to)` and
`capture(label)` send macOS UI events, refresh accessibility state, and save UTC
input records/screenshots under `build/cu-playtest/`. Use coordinates only from
a fresh screenshot. `note(text)` records a judgment or reproduction detail.
The CUA interface has no explicit key-down/hold-duration or mouse-move-only API;
pulses and clicks cannot establish the feel of sustained key or Pull Star holds.
Do not mislabel that interface limit as a game-input failure.

Repeatable scenario script (observe between steps; do not run blind coordinates):

1. Capture title; press Return once to send A+B at title (plain A elsewhere).
   Capture save selection. Select the existing Mario file and enter the
   observatory through normal UI; this fixture supplies post-tutorial progression.
2. Exercise WASD, Space, F, Q/E and C. Compare pointer placement before/after window
   resize; test fullscreen followed by at least 20 seconds of gameplay, focus loss
   and focus regain. Save `build/list-app-windows <own-petari-pid>` output before
   fullscreen, during fullscreen, after restoring windowed mode, and immediately
   after any failed click, using distinct timestamped files beside the screenshots.
   This helper is read-only. Compare window bounds/order with the screenshot before
   attributing a click failure to SDL or CUA. Record whether a game controller is
   attached and the evidence used; silence in device logs does not prove absence.
   Test a single Escape/Minus tap to pause and another to resume. Check that inputs release.
3. Follow visible paths to the Terrace. Interact with its Pull Star, select Good
   Egg, wait for any first-time reveal, and select mission 1.
4. Try movement/jump/spin, pointer/Star Bit collection and shooting, NPC dialogue,
   launch stars and pause/resume. Attempt the mission and return-to-observatory UI.
5. Return to the observatory through the pause menu, then select Quit and confirm
   “Save your progress and quit?” Capture the save result before closing the app
   with Cmd+Q or F1 → Quit. Allow at least two minutes before the session deadline
   for these steps. Check a zero exit and save writes. Relaunch as the next numbered
   session and verify the same file and any earned progress; do not infer save
   persistence merely from fixture state or a changed save-file hash.
6. Quit the final session. Run `python3 native/tools/cu_playtest.py --summarize`
   to refresh `build/cu-playtest/analysis.json` without launching anything.
   Write a prioritized issue/coverage report at
   `build/cu-playtest/REPORT.md`, linking screenshots and timestamped log context.

Good Egg mission 1 landmark route, verified with real held input in session 12:
walk off the first disk's rim to its underside, then go toward the green pipe.
Approach the central stem on the pipe-facing side, approximately a quarter turn
around from the question block. The question-block face is a wall. Enter the
narrow curved brick ramp with sidewalls and keep walking up the stem to its end
cap. Talk to the Luma there to create a Sling Star. Center under it and spin;
spin again when caught by the Launch Star to fly to Peanut. Short repeated F taps
completed this sequence in the playtest; a long screenshot gap between the two
spins can miss the activation window. The stone bridge with the Star Bit crystal
is a dead end and is not required for this route. Observe after every movement
batch rather than replaying fixed coordinates through rotating gravity/camera.

Report observed facts separately from likely causes and untested scenarios. A
screenshot is not continuous video, and device/audio counters are not a listening
assessment. Do not modify game code during this diagnostic task.

For this playtest the optional macOS CGEvent keyboard-hold helper was
authorized, because CUA has no held-key API. Compile it **before**
launching the app:

```sh
swiftc native/tools/cu_hold_input.swift -o build/cu-playtest/cu-hold-input
```

After binding the running app with CUA and making it frontmost, a bounded forward
hold is:

```sh
build/cu-playtest/cu-hold-input build/cu-playtest/active-session.json w 1.0
```

Use fresh CUA accessibility/screenshot observations after each hold. Supported
keys are W/A/S/D, Space, F, Q/E, C and Shift (lowercase argument names); up to three
may be combined with commas. Holds are capped at three seconds. The helper checks
the recorded PID and bundle and the frontmost application.
It logs every key event to `actions.jsonl` and sends cleanup key-up events only
to the original PID on normal return, focus loss, SIGTERM, SIGINT or SIGHUP.
It does not open a macOS permission prompt if event-post access is unavailable.
Continue using CUA for screenshots, pointer operations, window control and all
other interactions. This helper sends OS events; it never injects game inputs or
changes game state directly. SIGKILL cannot run cleanup; prefer the handled
signals when stopping it.

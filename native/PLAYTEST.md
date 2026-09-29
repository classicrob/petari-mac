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
pass in the root build. The smoke worker also ran its 137 checks and 27 seam
checks under ASan/UBSan.

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

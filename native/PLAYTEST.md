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

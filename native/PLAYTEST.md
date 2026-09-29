# Native gameplay playtest — 2026-09-29

These are full-app tests on the extracted RMGE01 disc, using the public native
input layer, game UI targeting, and read-only game-state observations. They do
not teleport Mario or change the game's state to advance the test. Desktop
computer control was unavailable, so this is not visual inspection.

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

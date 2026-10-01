# Petari native macOS project handoff

Updated 2026-09-29. Implementation and playtests are paused while handing off, following the user's request for this document. No playtest or build is running. The most recent run exited normally with a smoke-driver failure; the next correction is understood but not implemented.

## What we are trying to deliver

A dependable, playable native Apple Silicon version of Super Mario Galaxy, with sensible keyboard/mouse controls, working saves, correct rendering, stable audio and smooth frame pacing. This is more than getting the opening scene to run.

The user wants us to anticipate problems rather than wait for them to discover and report every issue. Their request was explicit: “I don’t want to have to come back to you with everything I want it all to just work!” Treat that as responsibility to investigate adjacent failure modes, build useful regression coverage, and validate actual player journeys. It is not evidence that the whole game already works.

The current concrete expansion is: be in the Comet Observatory, enter the Terrace, use the normal galaxy/mission selection interface, and play the first real non-tutorial level, Good Egg Galaxy mission 1. The user also noticed substantial frame drops. Functional progression and performance both matter.

The user can interrupt, manually play, or ask us to pause for a meeting. Stop playtests when asked; resume when invited. Do not interpret their accidental input or interruption as loss of authorization for the project. They explicitly authorized the team to resume and said Astra agents may be used.

## How the user wants the lead agent to work

Own the outcome. Make reasonable implementation decisions, coordinate the team, review its conclusions, integrate changes, and keep going through real failures. Do not make the user act as project manager or ask them to approve routine reversible fixes.

Use child agents/threads for concrete, bounded parallel work. The user has explicitly welcomed team work and Astra agents. Useful ownership splits have been game/route behavior, rendering and libraries, platform/audio, and app/input/testing. Assign exact files or modules, explain that the checkout is shared, and forbid reverting another worker's changes. Reuse agents for follow-ups instead of starting redundant investigations.

The root agent owns integration, shared CMake changes, the main app build directory, live playtests, interpretation of results, and the final account to the user. Workers should report what changed, exact evidence, limitations, and whether their sources are stable. “Source-ready” is not an integration pass. Check that interrupted workers actually resumed; do not merely assume they did.

Keep the machine quiet during performance measurements. Earlier abandoned test loops consumed CPU and invalidated timing conclusions. Use bounded tests, check their exit status, and verify no background jobs remain before declaring a measurement clean. Workers must not edit sources during the integration build or start heavy probes during a measured playtest.

Communicate plainly and concisely, with meaningful progress updates rather than a stream of implementation trivia. Start user-facing messages with a kaomoji, as requested. Admit and correct mistaken conclusions. Separate verified outcomes, plausible hypotheses, and untested work.

## Where the files are

Repository:

```text
petari/ (this repository; paths below are relative to its root)
```

Branch: `port/macos-arm64`.

App bundle, relative to the repository:

```text
build/macos-gx/native/app/Petari.app
```

Executable: `build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari`.

Normal launcher: `Play Petari.command`. Normal user data is under `~/Library/Application Support/Petari`. Automated tests use separate copies; never overwrite the user's actual save.

Extracted disc: `build/game-data/RMGE01`. The sibling `../aurora-reference` is the upstream renderer reference; do not edit it. Native renderer changes use generated-source patches in `native/gx`.

This handoff is `docs/dev/HANDOFF.md`. More detailed evidence is in `native/RELIABILITY.md` and `native/PLAYTEST.md`; build/control documentation is in `native/README.md` and `native/CONTROLS.md`.

## What is already working

The native app boots, creates and reloads saves, accepts keyboard/mouse controls, and runs the opening gameplay. Automated story coverage now walks Peach's Castle Garden, plays both full prologue movies, and reaches a ready HeavensDoorGalaxy. Story run 9 completed without physical gameplay input or stuck recovery. This establishes that journey, not completion of the tutorial or the entire game.

Significant fixes already checkpointed include:

- Atomic safe-save replacement, with a concurrent-reader regression.
- Native material-animation binding, tested across 524 archives and 1,287 animations.
- Generated collision, lighting, and native pointer/resource issues encountered on the story route.
- A companion heap for native model/animation copies, preventing the real HeavensDoor load overflow while preserving original archive bytes and the Wii archive-placement budget.
- Movie decoding that releases the emulated CPU during pure host computation; frame and audio comparisons cover all nine movie files.
- More reliable keyboard jump-then-spin, bounded queued shakes when tapping rapidly, and a remappable walk modifier.
- Audio synchronization improvements: an owner-aware registration lock, scoped host priority inheritance, and moving diagnostic output off the real-time threads.
- Real-input tracking: successful runs helped by bound physical key/button input report ASSISTED, exit 3, rather than PASS.

The user supplied a jump and steering around a wall in story run 6. We originally overstated that run and corrected the record. Its heap/load evidence remains valid; it is not an unattended pass. Later revised routes passed without that help.

Audio remains an open reliability item. Story run 9 had zero device underruns, one expected silent startup replay, and two unexpected replays. Do not call the whole audio subsystem clean based on a successful route.

## Current observatory work: implemented, not yet checkpointed

There are substantial uncommitted changes in the working tree. Preserve them. The latest existing commit is `74599b96d`, following synchronization commit `89d435837`. Run `git status` before doing anything; do not reset or clean the checkout.

The current changes add:

1. An explicit isolated observatory fixture. `--test-fixture observatory` requires an explicitly supplied, marked test user directory and rejects the normal user directory. At reload it supplies the tutorial Grand Star and completed observatory introduction. This is synthetic test progression, not proof of earning those events. The helper `native/tools/create_observatory_fixture.py` copies only NAND into a new directory and records source hashes.
2. `PETARI_SMOKE=galaxy`: an input-driven observatory walk, Pull Star interaction, galaxy/mission selection, and planned movement/jump/pause/resume checks in Good Egg mission 1. Native hooks publish visible UI targets; they do not directly select a galaxy or teleport Mario.
3. A fix for an actual renderer abort: an indirect texture order referenced disabled coordinate 1. Shader analysis and generation now use the coordinate-0 fallback. Valid coordinates remain unchanged. All 454 captured GX configurations pass source generation, and the app completed startup compilation of the seeded configurations.
4. Startup preparation of known shader pipelines before gameplay. An optional CMake pipeline-configuration seed is bundled as `Resources/initial_pipeline_cache.db`. The local build uses `build/observatory-pipeline-seed.db`; it is a build artifact, not a committed asset. New unseen configurations still compile during play.
5. A fix for a real galaxy-nameplate crash: two source strings said `TxtGaxyName` rather than the actual layout pane `TxtGalaxyName`, including the `U` variant. The asset regression passes and rejects the original typo. The following app run displayed the nameplate and got past the prior crash.

Root CTests for the driver, frame seam, fixture isolation, shader regression and nameplate asset check passed. The standalone driver suite currently passes 258 checks. These do not substitute for the unfinished live mission-selection test.

## Latest run and exact next step

`build/observatory-4.log` is the latest run, using `build/observatory-user-2`.

It walked from observatory spawn into the Terrace without manual gameplay input or recovery, activated the Pull Star, advanced the three visible introductory dialogue pages, displayed the galaxy nameplate, and clicked Good Egg. It then failed at frame 3820: `no selectable Galaxy.Start shown for 600 frames`. No app is left running.

The app/route agent identified the cause from source:

- A newly available galaxy's first click **unlocks/reveals** it; it does not select it.
- `MiniatureGalaxy::receiveOtherMsg` enters the Open animation and returns false for the New state.
- The reveal lasts about 150 frames; a later click on the open galaxy starts confirmation.
- The observer currently labels both New and Open states `Galaxy.EggStarGalaxy`, so the driver advances to waiting for Start after the unlock click.

**Next correction:** publish a distinct `Galaxy.UnlockEggStarGalaxy` target for New, keep `Galaxy.EggStarGalaxy` for the genuinely selectable Open/Wait state, and have the driver click unlock, wait through the reveal, then click the open galaxy before waiting for Start. Preserve the already-open path. Add a regression for New → reveal/no input → Open → confirmation. This correction has not been made; the agent stopped for this handoff without additional edits or tests.

After that, rebuild and rerun through Good Egg mission 1. Fix whatever actually fails rather than weakening pass criteria. PASS should require the correct stage and mission, stable readiness, and the gameplay checks. This is still only an initial gameplay smoke, not a complete Good Egg mission or star collection.

The current galaxy driver uses pointer targets. Physical pointer motion is logged separately and does not mark ASSISTED; unlike the story walk, pointer motion could influence selection. Report pointer/focus counts explicitly and avoid claiming a run was completely untouched merely because no bound key/button edges were recorded.

## Frame drops and remaining uncertainty

First-use shader compilation is a proven contributor. Known pipelines prepared in 30.85 seconds on a fresh cache and 1.32 seconds on the next launch. Observatory run 3 nevertheless encountered 18 new blocking pipeline resolves totaling 27.27 seconds, largest 4.61 seconds, in newly exercised Terrace content.

Run 4, with that content cached, had three additional blocking resolves totaling 1.23 seconds, largest 510 ms. It was much better, but still showed smaller frame-time spikes. This is evidence that caching helps; it is not a claim that frame pacing is fixed. The next new stage will introduce more uncached shaders. Compare cold and warm runs honestly and avoid concurrent compilation/test load.

Run 4 had zero device underrun frames and two AI replayed blocks. One startup replay is expected and silent; the remaining replay is not yet explained. Prior runs had more substantial audio failures, documented in the reliability ledger.

Still unverified: successful first-mission selection and gameplay, complete tutorial progression, collecting a star and returning to the observatory, subsequent galaxies, rides/tilt-control usability, broad visual fidelity, and full-game stability/performance.

## Practical resume notes

Current collaboration agents:

- `galaxy_route_resume`: owns smoke driver/observer tests and the observational UI hooks; latest diagnosis above, sources frozen.
- `renderer_resume`: implemented shader fallback and nameplate fix/regressions; completed.
- `shader_stalls`: provided startup-cache analysis and the collision-derived Terrace route; completed, no product edits.

They share the checkout. Root owns `native/app/frame_seam.cpp`, `app_main.cpp`, shared CMake, fixture bootstrap and the integration build. Check their actual status when resuming; these names may not survive a new session. Older BB child-thread reports in the conversation are historical, not evidence those workers are still running.

Build and targeted tests from the repository:

```sh
cmake --build build/macos-gx --target petari petari_app_smoke_tests -j8
ctest --test-dir build/macos-gx \
  -R '^(native_app_smoke|native_app_seam|native_gx_shader|native_observatory_fixture|native_galaxy_name_plate)$' \
  --output-on-failure
```

The current marked fixture can be reused for the next attempt:

```sh
PETARI_SMOKE=galaxy PETARI_SMOKE_FRAMES=108000 PETARI_TRACE_BOOT=1 PETARI_AUDIO_DIAG=1 \
  build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari \
  --disc build/game-data/RMGE01 --user build/observatory-user-2 --test-fixture observatory \
  > build/observatory-5.log 2>&1
```

Use a new log name. Await build completion before launch. Keep tests/probes stopped during the run. Rebind the computer-use app handle after relaunch before taking screenshots; stale bindings caused timeouts. Do not inadvertently launch a second bare app when obtaining an app handle.

The next checkpoint should include the uncommitted observatory work with honest validation notes. There is no need to redo the completed story route simply to make the handoff feel complete; focus on the remaining level-selection path and the user's observed performance problems.

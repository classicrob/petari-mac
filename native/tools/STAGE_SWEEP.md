# Whole-game stage sweep

Unattended crash/hang/heap/shader/perf coverage for every galaxy and mission.

**Synthetic entry.** A run loads a saved file and the game's own after-loading
galaxy move (`GalaxyMoveArgument` type 6, `StorySequenceExecutor::
overwriteGalaxyNameAfterLoading`) goes to the requested stage and scenario
instead of the observatory, with the scenario set as `ScenarioSelectScene::
trySetCurrentScenarioNo` would (hidden stars placed in their host scenario).
Only the observatory fixture's post-tutorial flags are applied; other galaxies'
stars, comets and unlock flags are unchanged. A PASS says the stage loads and
runs under basic input. It makes no claim about progression, how a player
reaches the stage, or completing the mission.

## Pieces

- `--test-fixture stage` (native/app/app_main.cpp) with `PETARI_STAGE=<Stage>`
  and `PETARI_SCENARIO=<n>`: requires an explicit `--user` whose
  `.petari-test-fixture` contains `stage` (never the normal user directory),
  and a `StageData/<Stage>/<Stage>Scenario.arc` on the disc. Create one with
  `native/tools/create_observatory_fixture.py --kind stage`.
- `PETARI_SMOKE=stage` (native/app/smoke_stage.hpp): the reload script through
  title and file select, then load → ready → idle, four walks, jump, spin,
  camera left/right, pause/paused-stick/resume, `PETARI_STAGE_IDLE_FRAMES`
  (600) idle frames. Movement and camera checks are warnings, not verdicts.
  Requires `--test-fixture stage`. Tests: `native_app_smoke_stage`.
- `native/tools/stage_sweep.py`: enumerates stages and scenarios from the disc
  (scenariodata.bcsv; domes from AstroDome placement layers A–F and the hidden
  galaxies' `Appear<Galaxy>` flags), runs each through the single-app lock `build/locked-app.sh` (`--lock-script`)
  with a per-run in-lock alarm, and classifies the log and exit status.

## Running

```sh
python3 native/tools/stage_sweep.py list --domes 1,2 --scenarios 1-3
python3 native/tools/stage_sweep.py run --domes 1,2 --scenarios 1-3 --name batch1
python3 native/tools/stage_sweep.py run --stages HoneyBeeKingdomGalaxy --scenarios 2 --name repro --rerun
python3 native/tools/stage_sweep.py reanalyze --name batch1   # re-parse logs with the current parser
```

Each sweep freezes a private copy of the app bundle (`<sweep>/app/`, sha256 in
`app.json` and every result) so concurrent rebuilds of `build/macos-gx` cannot
mix binaries into one sweep; `--refreeze-app` takes a new copy. Every run
starts from a fresh fixture user directory (NAND copied from `--source`) plus a
one-time SQLite snapshot of `--pipeline-cache-from`'s pipeline/Dawn caches
(`--cold` for none), so first-use shader counts are comparable across runs.
Bundled seeds come from the frozen app. `--seed-dir` snapshots an explicit alternative.

Output: `build/stage-sweep/<name>/runs/<Stage>-s<n>/{app.log,frames.csv,
result.json,command.txt,user/Crashes}`, and `results.json`, `results.csv`,
`summary.md` for the sweep. Existing results are skipped unless `--rerun`. A
run whose app could not be executed writes `infra.json` (no result) and stops
the sweep.

## Outcomes

| Outcome | Meaning |
| --- | --- |
| PASS / PASS_WARN | driver PASS, exit 0; PASS_WARN when a movement/camera check warned |
| PASS_RENDER_ERRORS | PASS, but renderer error lines or failed pipeline compiles in the log |
| CRASH / HEAP | crash report (signal, last panic, top frames); HEAP when a native heap allocation failed |
| HANG / SHUTDOWN_HANG | watchdog exit 124 (no frame for the stall limit; stacks sampled into `user/Crashes`) / 125 |
| TIMEOUT | the per-run alarm fired |
| NOT_READY, DIED, LEFT_STAGE, PAUSE_FAIL, ENTRY_FAIL, BOOT_FAIL, PLAYER_VANISHED, FRAME_LIMIT, FAIL | driver FAIL, by reason |
| MISSING_ASSET | the seam failed a PASS because layout/sound lookups missed; the `[layout]`/`[sound] missing` pairs are listed |
| NON_GAMEPLAY | a stage with no gameplay by design (EpilogueDemoStage, the ending movie) loaded and ran to the ready limit without a crash; not a pass |
| BLOCKED | a system prompt appeared |
| ASSISTED | physical gameplay input during the run |

Per run the summary also reports load-to-ready seconds, gameplay frame times
(ready → end: p50/p95/p99/max, frames over 20.85 ms), first-use pipeline
configs attributed to the stage (and how many the seed manifest did not
cover), blocking pipeline resolves (count and worst ms), and heap headroom
after the stage's scene initialization (`[heap]` reports).

## Placement warps and race probes

`--env PETARI_STAGE_WARP=x,y,z` requests one synthetic warp after readiness.
The native hook also accepts `name:<GeneralPos>` for a loaded named position.
These runs do not validate walking to the destination.

`--warp-placement 'NAME[:FIELD=integer][@index]'` resolves the position from
placement or GeneralPos data in the root zone, filtering layers by the selected scenario.
For GeneralPos, the selector may be its PosName (for example `ゴーストデモマリオ位置`).
It rejects missing or ambiguous matches unless a zero-based index is given.
It records the archive, table path, row, scenario mask and coordinates in
`runs/<stage>-s<n>/warp-placement.json`. Subzone transforms are not supported;
subzone-local coordinates are never silently used as world coordinates.
Explicit warp/environment runs cannot reuse an ordinary baseline result.

Cosmic Mario appears when a SwitchCube sets the GhostPlayer's SW_APPEAR.
The active trigger selectors for scenario 4 are listed below. Honeyhive and
Gold Leaf additionally require Mario to be grounded (Obj_arg2=0), so an area
origin alone does not provide a safe grounded spawn. The Honeyhive origin
probe fell below terrain and died before triggering the race. `ゴーストデモマリオ位置` resolves the authored
Mario demo position in all three stages; this is also a candidate warp target.
Honeyhive, Freezeflame and Gold Leaf race activation was observed in the rows-v1/v2
probes (full 663-frame intro followed by control release). All idle race probes
ended DIED and are excluded from PASS-only cache merging. Each new run must still establish its own activation evidence.


| Stage | Selector |
| --- | --- |
| HoneyBeeKingdomGalaxy | `SwitchCube:SW_A=12@0` |
| IceVolcanoGalaxy | `SwitchCube:SW_A=4@0` |
| ReverseKingdomGalaxy | `SwitchCube:SW_A=7@0` |

Example (synthetic race-trigger entry, live verification required):

```sh
python3 native/tools/stage_sweep.py run --stages HoneyBeeKingdomGalaxy --scenarios 4 --name honey-race --warp-placement 'ゴーストデモマリオ位置' --idle-frames 3600 --env PETARI_STAGE_PROBE=1
```

A stage PASS before the race starts is not race coverage. A DIED result after
losing a race remains DIED, even if it demonstrates survival beyond an earlier
renderer crash. Record actual race start/end evidence and elapsed duration.
Every sweep enables `PETARI_AUDIO_DIAG=1`; summaries retain underrun frames,
replayed blocks and DSP holds, with AUDIO-CHOPPY above one hold per second.

## Background automation

Smoke scripts and explicit test fixtures default to `PETARI_SMOKE_BACKGROUND=1`.
The app requests no activation, uses an accessory application with a nonfocusable background window, ignores
physical keyboard/mouse/controller/touch input before Aurora sees it, and keeps
driver input logically focused. VI and audio continue normally; startup pacing
uses the precise delay even without focus. The ASSISTED check remains unchanged.
Frame CSV still records actual lack of window focus; it is not forged as focused.
`result.json` records `background_smoke` from the app's startup marker. Background
performance still requires absence of competing apps/builds and live validation.

Set `PETARI_SMOKE_BACKGROUND=0` explicitly for human-controlled fixture playtests
or visible foreground diagnostics; physical input and ASSISTED detection then
behave as before. An ordinary non-fixture, non-smoke launch stays interactive.

Background validation: `build/stage-sweep/background-live-v3` passed EggStar s1
with all 1,457 measured frames unfocused, 0 physical assistance and fault-free
audio. The external System Events monitor never reported Petari frontmost.
LaunchServices (and a fresh NSWorkspace query) can nevertheless report Petari as
frontmost while System Events reports the user's app; this is a known observer
discrepancy. Use System Events together with actual window-focus and input
telemetry for this check, not LaunchServices alone. The extra monitor makes this
run instrumentation evidence rather than a timing baseline.

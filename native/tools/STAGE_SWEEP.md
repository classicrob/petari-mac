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
  galaxies' `Appear<Galaxy>` flags), runs each through the sweep app lane `build/locked-sweep-lane.sh` (`--lock-script`)
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
`PETARI_PIPELINE_SEED_DIR` is `build/pipeline-seeds` when present.

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

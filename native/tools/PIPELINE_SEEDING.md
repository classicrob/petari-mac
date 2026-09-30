# Pipeline preparation and diagnostics

## Default startup (2026-09-30): no preparation screen

The game starts at once. Nothing waits for the shader backlog:

- **Background global preparation is the default.** Unset (or `background`)
  `PETARI_PIPELINE_GLOBAL_PRECOMPILE` queues the bundled global seed at background
  priority before the game thread starts. `1` restores the blocking, skippable
  full-preparation screen (six startup workers); `0` disables global work.
- **Scene first.** A stage/overlay begin moves that scene's manifest ahead of the
  global backlog; a draw that needs a pending pipeline jumps to the front of all
  queued work and is never throttled (user-initiated QoS). During active gameplay
  (frame phase Gameplay and the pause menu closed) at most
  `PETARI_PIPELINE_GAMEPLAY_SPECULATIVE` (default 1) speculative compiles (stage
  manifest first, then the global backlog) run at once, at utility QoS: Metal
  compiles in MTLCompilerService, a separate process our thread QoS does not cap,
  so the in-flight count itself is limited. Menus, file select, loading and pause
  run the backlog on every worker. Game threads are user-interactive, VI/audio real-time.
- **Bounded draws.** A GX draw whose pipeline is still compiling waits only within a
  per-frame budget (`PETARI_PIPELINE_DRAW_BUDGET_MS`, default 6, reset each frame),
  then the draw is skipped until the compile lands (pop-in instead of a freeze).
  Clear and UI pipelines always block. `PETARI_PIPELINE_POLICY=blocking` restores
  unbounded GX waits; `async` skips without waiting. Shutdown reports
  `skipped_draws` (encoded draws dropped) and `deferred_draws` (draw requests whose
  budget ran out) in `[gx pipeline summary]`, plus per-config `[gx pipeline miss]`.
- **Short stage gate.** Scene start waits for the stage manifest at most
  `PETARI_STAGE_GATE_MS` (default 1500, max 10000); nothing needs the gate for
  correctness, it only trims pop-in when compiles are nearly done.
- **Per-machine cache.** A normal launch keeps `pipeline_cache.db` and `dawn_cache.db`
  in `~/Library/Caches/Petari`, not the `--user` directory, so copying or switching
  saves stays warm (Metal's own cache was already per machine). The first launch
  adopts an existing `<user>` cache once. A second concurrent normal launch, and every
  test run (fixtures, `PETARI_SMOKE`, unlocked-save, marked user dirs), keeps the old
  `<user>` location; background smoke runs keep `<user>/cache`. `PETARI_CACHE_DIR`
  overrides all of these. The chosen directory is logged as `[gx pipeline cache]`.

Tools that pin `PETARI_PIPELINE_GLOBAL_PRECOMPILE=0` (stage_sweep, soak, cu_playtest)
still get bounded draws and the short gate; set `PETARI_PIPELINE_POLICY=blocking`
where a run must reproduce the old stall-instead-of-skip behavior.

The rest of this document records the earlier blocking-default history; statements
there about the default policy, the ten-second gate and the preparation screen
describe the builds they measured.

`PETARI_PIPELINE_POLICY=async` opts into Aurora's existing skip-draw path while a
pipeline is pending, with no wait at all. This can hide geometry or effects for an
unbounded number of frames under load.
`PETARI_PIPELINE_THREADS=1..4` sets the compile pool. The automatic size is performance cores minus four,
clamped to 1–4 workers (four on the measurement M4 Max). Full startup preparation
uses `clamp(performance_cores / 2, 4, 6)` workers: six on the 12-performance-core
M4 Max. `PETARI_PIPELINE_STARTUP_THREADS` overrides that startup count (from the
normal pool size through 32); invalid values use the automatic startup count.
After preparation or skip, extra workers finish their current job and retire to
the normal pool. Background-only and stage-only modes use the normal pool.
The isolated 4/6/8/10-worker comparison found six and eight effectively tied at
160 seconds, versus 197 seconds for four, with better progress p99 at six.
These are single trials with differing host loads, not a universal scaling law.
Requested jobs
precede background preparation; Blocking draws also move ahead of speculative
stage jobs already in the requested queue. An in-flight compile is not cancelled.
The pool overlaps known work, but cannot parallelize serial discovery of new
configs by a command processor blocked on each first use. Dawn/Metal may impose
additional internal serialization; measure actual overlap before claiming gains.

## Stage ABI

`petari_gx_pipeline_stage_begin(const char* stage, const char* seedPath)` loads a
manifest and queues its scene/offscreen variants plus all eight clear masks.
It returns after submission; it does not wait for compilation when the normal
Aurora worker subsystem is active. Call after renderer initialization and before
scene loading. Repeated stage/layout begins reuse the targets and promote pending
work without duplicate compilation. A different real stage demotes the previous
stage's queued speculative work; jobs already requested by an actual draw keep
priority. ScenarioSelect and GalaxyMap are additive overlays: they neither replace
the real stage nor demote its jobs. Overlay tags persist until the next real stage.

A null seedPath selects `$PETARI_PIPELINE_SEED_DIR/<stage>.db`, or
`Resources/pipeline-seeds/<stage>.db` when the variable is unset. Absent, invalid,
or empty manifests fall back to configs already known in memory, logged as
`shared-observed-memory`. Such fallback cannot invent unseen material states.
`extern "C" uint64_t petari_gx_pipeline_manifest_failure_count()` returns a
process-lifetime monotonic atomic count of failed stage-manifest load attempts.
It allocates nothing and is safe to sample at the frame seam. Open/schema, query,
row-read, invalid-row and empty-manifest failures increment once per attempted
load and emit `[gx stage prep] ... manifest read failed` with source and count.
Missing optional default seeds do not count; explicit paths that fail do count.
Same-stage idempotent begins do not retry or increment again. The count survives
stage/renderer resets so smoke validation cannot lose a recorded failure.

Files use Aurora's existing SQLite config cache schema/version checks; they
contain raw versioned config structs, not Metal binaries. Re-export when the
Aurora config ABI changes. Use trusted locally generated caches.

`petari_gx_pipeline_stage_wait()` waits for the current stage plus overlay targets
for at most `PETARI_STAGE_GATE_MS` (default 1.5 s; ten seconds before 2026-09-30),
prints residual wait/pending/failed counts, and proceeds.
The caller must release the emulated CPU baton around this host wait. After a
stage-gate timeout, later Blocking draws can still wait for pending compiles.
The timeout therefore prevents an infinite stage gate, not every gameplay stall.

First-use records include config hash, stage, loading/after_prep phase and coverage.
Each config is recorded once per stage, with separate additive overlay tags.
Overlay tagging means “seen while overlay active”, not proof of draw ownership.
Coverage uses the active config manifest; runtime layout variants may differ.
Records are buffered and emitted at frame boundaries or shutdown rather than
performing stderr writes while holding the scheduling mutex. Stage submission
still reads a SQLite manifest on the calling game thread; measure enqueue_ms.
Seeds have a first-use ordering index; the read-only VFS reader also sets
`temp_store=MEMORY`. Without these, large manifest sorts attempted a writable
temporary database and failed, silently reducing preparation to the observed
fallback. A CPU-only VFS regression reproduces the old failure and both fixes.

## Offline archive replay and bundled stage seeds

The collector and replay are separate steps. The collector follows scenario
ZoneList and all placement layers, resolves static NameObjFactory archive aliases,
and includes shared player resources and layouts. `--all-stages` discovers every
scenario root on the supplied disc. Unresolved actor names remain in its report;
some have no model, while dynamic factory callbacks can add dependencies.

```sh
python3 native/tools/collect_pipeline_seed_inputs.py \
  --files build/game-data/RMGE01/files --all-stages \
  --output build/pipeline-all-stage-inputs.json
build/locked-build.sh pipeline cmake --build build/macos-gx \
  --target petari_pipeline_replay -j2
build/locked-build.sh pipeline python3 native/tools/build_pipeline_stage_seeds.py \
  --files build/game-data/RMGE01/files \
  --inputs build/pipeline-all-stage-inputs.json \
  --baseline build/observatory-user-2/pipeline_cache.db
```

The CPU-only replay scans every archive under ObjectData, StageData and LayoutData,
including archives not selected by the static dependency resolver. It creates no
window or GPU device. Each archive runs in a separate process with a timeout;
errors and aborts are retained with resource/material/shape/hash context.

For BDL/BMD it uses the native J3D loader and ResourceHolder's loader flags,
material display-list generation, shape VCD/VAT and draw lists. Source-extracted
ModelUtil preprocessing inserts the same environment/projection-map matrix
attributes and fractional position changes as the runtime loader. The replay
exercises both current-texture-matrix modes. It records real GX default state,
flushes deferred GX registers, and decodes FIFO through Aurora's actual register
handlers and `populate_pipeline_config`. Vertex spans are bounds checked; indexed
geometry and matrix values need not be fetched to construct a pipeline config.

Same-archive BMT material alternatives are joined to shapes by material name.
MarioAnime's shared BMT is conservatively joined to Mario and Luigi models, as
both use that shared resource archive. The cache signature includes those external
model archives. BRLYT replay uses the native normalizer, Material constructor,
SetupGX and vertex-format setup, with actual pane texcoord counts, opaque/faded
alpha and vertex-color modulation. Text/font CharWriter specialization, particles,
procedural draws and dynamic game material mutations are not exhaustively replayed.

Every emitted GX config is passed through the generated production shader source
builder for five legal output variants: None/Replace destination alpha with and
without the normal attachment, plus DualSource without it. Both RGB8 and RGBA6
pixel states are seeded. This checks source-generator aborts and unsupported GX
states reached by these inputs; it does not run Tint, compile Metal, or prove
visual correctness or complete runtime state coverage.

The driver snapshots the replay executable by SHA-256, resumes only matching
archive/executable signatures, and merges replay results into each stage's mapped
archive set plus the unchanged shared observed baseline. UI overlays include all
layout archives conservatively. It publishes no new manifests when any archive
fails. Per-archive SQLite `replay_sources` records preserve material/shape origins;
JSON sidecars preserve baseline/input hashes, archive signatures and limitations.
The default local baseline has 602 rows (599 GX and three clear configs).

Outputs are `build/pipeline-seeds/<stage>.db` plus provenance JSON;
`build/pipeline-replay-corpus/corpus.json` and `stages.json` hold detailed reports.
These are versioned raw pipeline configurations, not Metal executable binaries.
Keep the disc-derived artifacts local rather than committing them as source.

The app CMake cache path `PETARI_PIPELINE_STAGE_SEEDS` defaults to
`build/pipeline-seeds`. An ordinary subsequent configure/build copies its DB/JSON
files into `Petari.app/Contents/Resources/pipeline-seeds`; normal launches then use
the bundled seeds without environment variables. An explicit path can select a
separate generated corpus. Fresh checkouts must generate the artifacts first.

For observed-only fallback, `export_pipeline_stage_seeds.py --cache CACHE --output
DIRECTORY` remains available. Its shared-observed provenance distinguishes it from
actual replay; do not overwrite a replay directory unintentionally.

## Coverage accounting

```sh
python3 native/tools/pipeline_coverage.py --seeds build/pipeline-seeds \
  --output build/pipeline-coverage.json
python3 native/tools/pipeline_replay_tests.py
```

The coverage tool reads observatory logs and stage-sweep logs, deduplicates config
hashes per stage/run, excludes additive overlay tag duplicates and distinguishes
original runtime coverage from missing hashes in newly supplied seeds. The latter
is a retrospective comparison, not a new playtest or evidence of reduced latency.
Logs without stage instrumentation cannot establish coverage. Logs still being
written provide partial results; retain exact paths and snapshot time in reports.

## Original corpus result, 2026-09-29

The local RMGE01 run completed all **2,215 archives with zero failures**:
1,853 model/layout/replacement-model replay instances, 9,325 material/shape cases,
3,053,860 draw visits and 11,818 per-archive distinct configs.
Correction: the original 59,090 source calls used a boolean normal-attachment
argument where the API requires UINT32_MAX for absent and 1 for present. The
collector is fixed; config bytes and hashes were unaffected. The serialized union
of 8,267 GX configs was independently checked across all five legal variants
(41,335 source builds, zero invalid results), including observed-only configs. The corpus contains 7,806 globally unique config hashes; 1,850
archives emit configs. Counts include conservative alternative states and repeated
geometry visits, not that many distinct runtime draws or shaders.

48 scenario roots plus ScenarioSelect/GalaxyMap produce 50 manifests. Every one
merges all 602 baseline rows. AstroGalaxy has 1,658 configs from 165 mapped archives;
EggStarGalaxy has 1,385 from 202; each overlay has 631 from all 92 layout archives.
The seed range is 631–1,658 configs. A locked `petari` build passed; all 100
bundled DB/JSON files match generated sources byte-for-byte (212.1 MiB total). Larger seeds increase cold compilation work;
Blocking remains default, the ten-second stage gate can time out, and no claim of
locked 60 fps or eliminated stalls follows from this corpus check. Observed
first-use ordering is preserved ahead of speculative replay-only alternatives.

Coverage snapshot: **2026-09-29 21:48:04 UTC**, 36 logs, saved in
`build/pipeline-coverage-replay-final.json`. Logs may still be growing. These are
unique post-prep first uses; the last column compares logged hashes against the
new stage manifest only (additive overlay seeds may cover more). This snapshot predates the subsequent integration measurements.

| Log under build | Stage | Logged uncovered / first uses | Missing from new stage seed |
| --- | --- | ---: | ---: |
| observatory-7.log | EggStarGalaxy | 1 / 191 | 0 |
| stage-sweep/check-1/runs/EggStarGalaxy-s1/app.log | EggStarGalaxy | 6 / 200 | 5 |
| stage-sweep/batch1/runs/BattleShipGalaxy-s1/app.log | BattleShipGalaxy | 79 / 183 | 62 |
| stage-sweep/batch1/runs/BattleShipGalaxy-s2/app.log | BattleShipGalaxy | 75 / 168 | 65 |
| stage-sweep/batch1/runs/BattleShipGalaxy-s3/app.log | BattleShipGalaxy | 79 / 183 | 61 |
| stage-sweep/batch1/runs/BeltConveyerExGalaxy-s1/app.log | BeltConveyerExGalaxy | 42 / 128 | 35 |
| stage-sweep/batch1/runs/BreakDownPlanetGalaxy-s1/app.log | BreakDownPlanetGalaxy | 12 / 130 | 9 |
| stage-sweep/batch1/runs/CocoonExGalaxy-s1/app.log | CocoonExGalaxy | 33 / 132 | 23 |

Good Egg's remaining five hashes are `785b74e1dfbb5e47`, `9b2682d426454ad7`,
`cae885e1e40a0dfa`, `eb99a8a0e41c4d2d`, `ffc0fdf9c98e7635`. Their causes are not
established by the first-use logs; inspect the corresponding captured config
structs and draw owners before attributing them to a material or subsystem.

Validation: full fixed-binary corpus, identical repeated ScenarioSelect config
bytes/hashes, two replay/publication/coverage Python tests, two existing seed tests,
CPU stage scheduling harness, worker harness in Blocking and async modes, and
`native_gx_shader` CTest all pass. ScenarioSelect reproduces five captured baseline
hashes and Mario reproduces three, providing exact runtime matches beyond source
inspection. No new generator abort was found, so no additional shader workaround
was added during this replay phase.

## Timings and cache interpretation

`PETARI_PIPELINE_DIAG=1` (or PETARI_TRACE_BOOT) emits queue, requested-queue, total
build, WGSL generation, CreateShaderModule and CreateRenderPipeline durations;
logs include config/runtime hashes, bytes/lines, TEV/indirect counts and status.
Timing accumulators are thread-local and copied under the scheduler mutex.
Async skip totals and per-config pending lifetimes are printed at shutdown.
Optional `PETARI_PIPELINE_DUMP=<directory>` writes WGSL, with I/O time separate.
Use dumps for diagnosis, not performance acceptance runs.

`pipeline_cache_inspect` generates WGSL for observed configs without Metal. All
599 captured GX configs passed; maximum source was 18,046 bytes/522 lines with
three TEV stages and one indirect stage. The current inspector checks all five legal output variants; CSV source size
refers to no normal attachment and DstAlphaMode=None. The initial global union
maximum was 18,312 bytes/532 lines. These are source checks, not Metal compiles.
Old resolve logs lack hashes, so their 230 ms–4.6 s worst cases cannot be matched
reliably to source complexity. WGSL size is not proof of Metal compile cost.

Aurora's config DB stores serialized configs; Dawn's blob cache stores MSL
translation/metadata in the pinned backend. It does not eliminate
newLibraryWithSource/newRenderPipelineState calls. The optional
`patch_pipeline_dawn_timings.py` instruments those individual calls plus
Tint-or-blob-cache translation in source-built Dawn revision
1155e0ed531126f33a1279afa029349651ca1c93. Its anchors have been checked, but it has
not been compiled or wired into the current prebuilt Dawn. The normal app build
therefore reports WebGPU API durations, not those deeper Metal subtimings.
No persistent Metal binary archive implementation is included in this change.

## Validation

CPU worker tests exercise the actual worker body, requested/background priority,
two gated concurrent jobs, thread-local timing isolation, completion and shutdown.
Stage tests include the actual stage body with CPU scheduling/device stubs and
check idempotence, additive overlays, attribution, ready gates and old-stage
demotion. Seed tests compile the generated production promotion function and
check shared/attributed export, signed hashes and input preservation.

With `BUILD_TESTING=ON`, the default build includes the CPU worker, stage and
seed-sort executables. CTest registers all seven pipeline checks: ImGui,
worker default/async policy, stage scheduling, seed export, replay bookkeeping,
and read-only SQLite sorting. Each has a 60-second timeout and uses synthetic
inputs; no disc-dependent test is added by this group. The seed-sort wrapper
creates and removes its own indexed/unindexed fixture databases. The seed test
uses CMake's configured Aurora source directory rather than requiring a sibling
reference checkout.

```sh
build/locked-build.sh pipeline-build cmake --build build/macos-gx -j2
build/locked-build.sh pipeline-build ctest --test-dir build/macos-gx \
  -R '^native_pipeline_' --output-on-failure
```

The registration integration run passed the default build and all 83 CTests
in 141.78 seconds (`build/pipeline-prep-measure/resume-background/full-ctest.log`).
The seven pipeline tests took 0.04–5.24 seconds each. This is a shared-build
suite result, not a separate clean-build or live-play verification.

These are component and source checks. Cold/warm route measurements are separate
and must hold app, sweep-lane and build locks in that order. All heavy CPU work,
including standalone compilations and sanitizer/stress runs, holds the build lock.


## Default startup preparation and retained observations

An unset `PETARI_PIPELINE_GLOBAL_PRECOMPILE` (or `1`) imports bundled `__global__.db`
before startup. The full union can require minutes on first use. The union
includes all replayed archives, including
ones absent from static stage maps, plus the observed baseline and explicitly
attributed additions. Global jobs use the background queue and utility QoS;
requested draws retain priority. The native startup loop pumps window events
and presents an ImGui progress/ETA screen at a target of 60 Hz after two seconds.
Preparations completing sooner do not flash the screen. Return, keypad Enter,
or controller A/Start leaves unfinished work in the utility-QoS background queue;
stage and draw requests still promote their jobs. Startup button presses are
not forwarded into the game, and the final focus state is handed to native input.
Resizes use the current window dimensions. The window title provides progress
when the overlay cannot draw. `[gx startup prep]` records screen visibility,
completion/skip, pending/failure counts, elapsed time and presentation-loop timing.

`0` disables global preparation and keeps observed startup plus stage lookahead.
`background` keeps observed startup, then queues the global manifest without
waiting for it. Never-drawn speculative rows are omitted from observed startup
in those two modes, even if a previous full-preparation run imported them.

Configs persist in the user's Aurora SQLite DB; Dawn translation blobs persist
separately. Neither stores ready Metal pipeline objects. A warm launch still
creates Metal libraries/pipelines; the measured warm comparison below took
1.56 s. `[gx global prep]` reports progress every five seconds. Shader API
timing includes Tint-to-MSL and Metal work; tiny `module_ms` does not mean Tint is
cheap, because the backend can defer translation until pipeline creation.

`PETARI_PIPELINE_OWNERS=1` is an opt-in diagnostic: GX producer stacks travel with
FIFO markers, and first-use records include the producer ID. It adds overhead
and is excluded from timing runs. `pipeline_owner_report.py` joins captured
stacks to config hashes; shared configs can have more than one owner. The family
classification is inferred from stack symbols, not proof of material identity.
`merge_pipeline_observations.py` adds real-stage first-use configs from a retained
cache to the stage and global seeds, preserving cache/log hashes and labeling the
additions as observed rather than offline replay. Reapply after corpus regeneration.

The first retained BattleShipGalaxy scenarios 1/2 supplied 88 distinct new configs
(62 from scenario 1, another 26 from scenario 2). These extend the working stage
and global seeds. Pre-pause frozen bundles retain the original 8,270-row union;
the resumed 8c4 measurement bundle contains the enriched 8,358-row union.
All 8,355 GX configs in the enriched union passed five-variant source generation
(41,775 checks, zero invalid results). The merge snapshots/provenance are under `build/pipeline-prep-measure/observations`.
Historical logs without retained caches or owner stacks cannot reconstruct owners.

`measure_pipeline_prep.py` records raw logs, timestamped events, load samples,
cache inventory, frozen binary hash, per-config queue/build/API distributions,
stage gates and blocking resolves. Cold fixtures contain only isolated NAND and
the fixture marker. OS Metal caches are not cleared; “cold” means empty app caches.
Warm runs require a successful preceding cold route. The first corrected run was
aborted under host load above 200; it is explicitly rejected as a performance
comparison. Requeued runs acquire all three locks before their timers start.


## Controlled app-cache cold/warm route, checkpoint 8c4c107cf

Artifacts: `build/pipeline-prep-measure/resume-8c4/{stage-cold,stage-warm}`.
Both runs passed the galaxy smoke through Good Egg mission 1 using the same
frozen app/seeds, automatic four-worker pool, Blocking policy, global preparation
off and owner tracing off. Each held app, sweep-lane and build locks. Cold means
empty **app** caches; the OS Metal cache was not cleared. All comparisons below
are this pair, not the earlier load-contaminated aborted run.

| Measurement | Cold | Warm |
| --- | ---: | ---: |
| Whole route, seconds | 183.84 | 112.23 |
| Startup preparation, seconds | 40.21 (454 jobs) | 0.12 (481 jobs) |
| Runtime pipeline builds | 2,142 | 2,142 |
| Build p50 / p95 / p99 / max, ms | 105.023 / 686.134 / 843.821 / 1012.819 | 0.626 / 70.752 / 145.591 / 206.631 |
| API p50 / p95, ms | 101.863 / 677.250 | 0.470 / 69.379 |
| Module p50 / p95, ms | 2.609 / 4.617 | 0.093 / 3.965 |
| Queue p50 / p95 / p99 / max, ms | 8262.370 / 29521.143 / 37240.777 / 39526.374 | 286.433 / 1503.620 / 1602.624 / 1672.026 |
| Host load p50 / p95 | 17.13 / 22.34 | 14.64 / 15.46 |
| Gate expiries | 2 | 0 |
| Individual blocking resolves >=10 ms | 0 | 0 |
| Telemetry waits >=50 us / total wait ms | 18 / 2.439 | 26 / 4.603 |
| Audio underrun frames / startup replay blocks | 0 / 1 | 0 / 1 |

WGSL generation was tiny (cold p50 0.060 ms, p95 0.098 ms). The pipeline API
includes deferred Tint-to-MSL, Metal library compilation and PSO construction;
it does not isolate Metal alone. The API accounts for 98.3% of cold build time.
The warm result demonstrates substantial caching benefit, but its remaining
70.75 ms build p95 prevents claiming that every cached pipeline is instant.

| Stage | Cold gate wait / pending at exit | Warm gate wait / pending at exit |
| --- | --- | --- |
| FileSelect | 10.002 s / 83 (expired) | 0.035 ms / 0 |
| AstroGalaxy | 10.002 s / 226 (expired) | 959.482 ms / 0 |
| AstroDome | 0.052 ms / 0 | 0.051 ms / 0 |
| EggStarGalaxy | 9.393 s / 0 | 0.044 ms / 0 |

Submission-to-gate-exit elapsed times were respectively 11.276/1.456 s for
FileSelect, 10.807/1.715 s for AstroGalaxy, 1.792/1.792 s for AstroDome and
15.990/6.880 s for EggStar (cold/warm). For expired gates these are **not** full
preparation times: later completion is not logged per stage. Every logged real
stage post-prep first use was covered; cold EggStar had 191/191 covered.

All-frame p50/p95/p99 were 16.681/17.400/19.685 ms cold and
16.682/17.644/20.216 ms warm. Counts above 16.7/33.3 ms were 2752/7 and 2789/10.
Stage gates contribute the largest intervals (10.020 s cold, 0.977 s warm), and
the current phase labels include some transition work in gameplay. Cold had no
unfocused frames; warm had 483 unfocused frames and four focus changes, so this
is not a controlled focused-frame A/B. Cold also had a 603.82 ms frame dominated
by 587.57 ms draw-done wait, and a 270.83 ms frame with 259.06 ms EFB submit wait.
Neither showed a pipeline wait on that frame. Pipeline preparation does not
explain or eliminate those stalls, and these results do not establish locked 60.

### Global preparation evaluation (8c4 frozen app)

`build/pipeline-prep-measure/resume-8c4/global-cold` passed the same galaxy
route with an independent empty app-cache fixture, the same frozen app, four
compile workers, and all three quiet locks. macOS's own Metal cache was not
cleared. `PETARI_PIPELINE_GLOBAL_PRECOMPILE=1` prepared all 8,355 GX configs
in 221.016 seconds (8,363 runtime builds including clear masks); whole-process
elapsed time was 331.590 seconds. Early work progressed around 21 configs/s;
the full preparation averaged 37.8/s as later configs increasingly hit caches.
This should not be compared as pure compiler scaling against the earlier
contended 7/s run.

Build p50/p95/p99/max was 4.590/592.058/710.751/862.507 ms. Pipeline API time
was 2.333/588.831/705.750/859.319 ms and accounted for 97.7% of build time.
Module p50/p95 was 0.054/7.036 ms, and WGSL p50/p95 was 0.057/0.106 ms.
Queue p50/p95 was 175.005/219.896 seconds: startup intentionally waits for
this whole queue, so those numbers are not in-play render-thread stalls.
Host-load p50/p95 was 22.23/27.71.

Every subsequent stage gate completed in <0.3 ms, with no expiry, no uncovered
post-prep first use, and no individual blocking resolve >=10 ms. Telemetry
still recorded 19 waits >=50 us totaling 3.301 ms. Audio had zero underrun
frames and one expected startup replay. All 6,509 frames were focused; frame
p50/p95/p99/max was 16.682/17.335/20.950/608.018 ms, with 2,647 frames >16.7 ms
and 12 >33.3 ms. The worst frame recorded no pipeline wait. Global preparation
eliminated the measured stage gates but did not establish locked 60 fps.
The isolated process's reported maximum RSS was approximately 10.0 GB;
this is whole-process high-water usage, not attributed pipeline-cache storage.
The completed warm comparison is recorded below; this cold result alone did not establish relaunch cost.

### Experimental background preparation

`PETARI_PIPELINE_GLOBAL_PRECOMPILE=background` submits the global manifest
before the game thread starts without waiting for completion. Unset or `1` now
selects the skippable full-preparation screen; `0` disables global work. The pool is sized from
performance cores with four reserved and a cap of four workers. Background
jobs run at utility QoS; stage requests move ahead of them, and actual draw
requests move ahead of speculative stage work. Already compiling work cannot
be preempted. Therefore QoS and queue priority alone are not proof of clean
frame times or audio; a live comparison is still required.

The background manifest is excluded from stage attribution and stage gates.
Repeated submission is idempotent, and speculative configs persist as never
drawn so a subsequent explicit `0` or `background` launch does not wait for every
unused global config at startup. Shutdown reports pending and terminal compile counts.
The scheduling harness covers these properties, including the missing-manifest
fallback fix. The harness and full app build passed. Completed background measurements are
reported below. Shared-observed fallback must exclude never-drawn global
configs, otherwise an Unknown stage would promote and gate the global queue. The pre-game manifest read/enqueue is synchronous and is timed in
its stage-prep log; shader compilation itself runs on the pool.

The inspector found 3,789 distinct exact WGSL strings for 8,355 default-layout
GX configurations, and 18,945 strings for all 41,775 legal variant checks.
All variants generated successfully. Dawn already content-caches identical
shader modules, so the 2.2 configs/source ratio is not evidence that another
WGSL module cache would save 2.2x compilation. Deferred MSL translation and
Metal compilation live inside the pipeline API boundary. See the pinned
package/source audit in `build/pipeline-prep-measure/dawn-package-audit` for
Release provenance and the existing MSL blob cache. No Metal binary archive
or new Metal library cache has been implemented.


The slowest global-cold config (`4867779342108c6d`) built in 862.507 ms:
0.216 ms outside module/API, 2.972 ms module creation, and 859.319 ms pipeline
API. Its WGSL was 17,483 bytes/512 lines with two TEV stages and one indirect
stage. The largest generated shader in the run was 18,312 bytes/532 lines.
Of 8,363 builds, 3,935 spent <1 ms in the API, 1,720 spent 1–100 ms, 2,106
spent 100–500 ms, and 602 spent >=500 ms. This bimodality is consistent with
cache reuse, but the current instrumentation does not identify which backend
cache supplied each hit. Summed build time of 882.267 seconds over 221.016
seconds preparation is consistent with four concurrent workers, not a
single-worker serialization bottleneck.

A possible product workflow after successful warm/background measurements is an
explicit first-launch preparation choice. Store completion only after every
manifest target succeeds, keyed by seed/config ABI, Dawn version, device,
render-target layout and relevant driver/OS identity. On later launches use
normal observed startup plus stage lookahead. An interrupted preparation must
retain partial caches and remain retryable; a completion marker alone must not
claim that the OS Metal cache still contains every compiled result. This is a
design option, not an implemented first-launch UI or automatic default.


The exact-key check (`resume-8c4/stage-key-comparison.json`) found identical
sets of 2,142 runtime keys in the stage-cold and stage-warm runs: no new warm
variants. Nevertheless, 120 warm API calls exceeded 50 ms (maximum 201.728 ms).
Their latency cannot be explained by discovering different configuration keys.
This strengthens the case for backend library/PSO timing before promising an
instant cache hit or adding a redundant WGSL module cache.

A subsequent offline reconstruction matches each frozen stage DB's config hashes,
plus eight clear masks, to logged compile-completion timestamps. It accepts a
stage only when every target has exactly one recorded runtime variant and the
set size equals the logged target count. For this pair those checks pass.
`resume-8c4/stage-completion-estimates.json` estimates full target readiness from
begin at 14.708/1.136 s for FileSelect, 18.960/1.727 s for AstroGalaxy, and
16.000/1.791 s for EggStar (cold/warm). ScenarioSelect took 7.498/0.024 s;
AstroDome and GalaxyMap targets were already compiled at begin. The global-cold
run had all these targets ready before begin. These are log-receipt-based
estimates, not new exact stage-completion instrumentation; they exclude extra
overlay target unions. They clarify that the two cold gate expiries preceded
actual completion rather than representing the total preparation duration.


## First-frame ImGui pipeline preparation

The two Dawn CPU-held waits in `build/soak/validate1/app.log:504` (88.1 ms)
and `:505` (360.1 ms) precede `first Aurora frame open; starting the game`
at line 506. They are startup evidence, not EggStar stage-load evidence.
The captured stack ends at Dawn APICreateRenderPipeline and does not include
its frontend caller. Source tracing identifies an unprepared ImGui pipeline:
`app_main.cpp:232` opens the first frame after starting the emulated OS;
Aurora's `imgui::new_frame` builds the font atlas/device objects on that frame,
and the backend also lazily creates device objects if its pipeline is absent.
Both lead to the synchronous WebGPU render-pipeline API. GX stage layout
finalization itself only computes a key.

`patch_aurora_pipeline_imgui.py` moves ImGui device-object creation immediately
after backend initialization in Aurora initialization, before `startOS()`.
It records `[gx pipeline prewarm] kind=imgui ready=... build_ms=...` and preserves
the SDL renderer branch. The lead separately added the normal host-blocking
release around first-frame event/presentation work in `frame_seam.cpp`; this
also protects any remaining lazy host work on that frame.

The full app build passed with the generated source. The CPU regression
`pipeline_imgui_tests.py` compiles the actual patched initializer and upstream
first-frame functions against stubs, verifies no creation while holding a
simulated baton, and rejects the original first-frame path by exit status.
The test passed (`resume-8c4/imgui-test-2.log`). This validates call ordering,
not live Metal timing. The subsequent frozen background control/trial both
recorded successful eager ImGui preparation and no detected CPU-held waits
of at least 20 ms before the first Aurora frame. These checks establish the
monitor's observed threshold, not the absence of every shorter host wait.


The next observed extension published 462 additional per-stage configs from
17 completed PASS routes in `build/stage-sweep/batch1b` (281 globally new),
bringing the union to 8,636 GX configs. These retain stage-observed provenance,
not offline-replay attribution. The inspector generated all 43,180 legal
variants with zero invalid states (3,809 distinct default WGSL strings).
The normal app build passed and all 20 changed bundled DB/metadata hashes
match the published seeds (`resume-8c4/observed-extension/rebundle.log`).
The 8,355-config measurement apps remain frozen for their existing comparisons.
Historical hashes from the merged routes are covered by the extension; this
is not evidence of zero gaps on a fresh run or identification of draw owners.


The background experiment's acceptance criterion is gameplay frame/audio cost
while compiles are active, not merely lower stage-gate waits. The recorded
stage-only cold baseline is 19.685 ms overall p99 and 19.720 ms gameplay p99,
with zero device underrun frames. Compare matching categories, focus state,
load and replay counts with both that baseline and the matched frozen control.
Existing global cold preparation has primed the system Metal cache; the new
control/trial have empty application caches but are not OS-cache-cold tests.
Full-warm duration requires every exact global target to finish successfully;
a route that exits with pending compiles yields only a lower bound, not a
completed preparation time. Compile log timestamps are receipt times. The
frame CSV lacks an absolute timestamp/pending-compile field, so any overlap
window reconstructed from cumulative frame intervals must be labeled as an
estimate, alongside the unambiguous whole-gameplay frame statistics.


Overnight extension 1 adds 68 TriLegLv1Galaxy, 55 SurfingLv1Galaxy and
13 TamakoroExLv1Galaxy stage-observed configs from completed PASS sweep runs.
Seventy-seven are globally new, bringing the bundled union to 8,713 GX configs.
All 385 output variants of the new configs generated successfully. Every
logged post-prep hash in those three runs is present in the updated respective
stage manifest. The full app build passed and all eight changed DB/metadata
hashes match the bundle (`build/pipeline-prep-measure/overnight-1/extend-seeds.log`).
This closes historical observed gaps, not unseen draw states; draw-owner
probe results and fresh coverage runs remain separate evidence. Frozen timing
apps retain their original 8,355-config manifests.

## Completed frozen preparation comparisons

The triple-lock runs below passed their synthetic observatory-to-Good-Egg smoke.
Application-cold means an empty isolated application cache; the system Metal
cache was not cleared. Sources and exact binary hashes are in each `run.json`
under `build/pipeline-prep-measure/`.

| Mode / artifact directory | Full global preparation | Gameplay p99 | Audio underrun frames | Stage-gate expiries |
| --- | --- | --- | --- | --- |
| Global app-cold, `resume-8c4/global-cold` | 221.016 s | 21.368 ms | 0 | 0 |
| Global warm, `resume-8c4/global-warm` | 1.559 s | 18.380 ms | 0 | 0 |
| Stage-only control, `resume-background/control` | Not requested | 18.509 ms | 0 | 2 |
| Background trial, `resume-background/background` | Incomplete at exit | 17.863 ms | 0 | 0 |

All four runs had one expected startup audio replay and no unfocused frames.
The global pair used the same binary and exactly 8,363 runtime pipeline keys,
including eight clear masks. All completed successfully. Warm per-pipeline
build p50/p95/p99/max was 0.505/1.566/2.515/7.105 ms; API total was 4.291 s
across workers. Every warm stage gate was below 0.24 ms. Host load differed:
global-cold p50/p95 was 22.23/27.71 versus warm 7.60/8.23. The persisted Dawn
and Metal caches together are effective; this does not isolate Metal's cache
or guarantee cache survival across OS, driver, device or renderer changes.

The matched background pair showed no gameplay p99 or audio regression while
the trial compiled through exit. It completed 4,554 pipelines, including 2,412
keys beyond the control, but had 3,809 pending: only 54.45% of global targets
were ready. Full background preparation time is unobserved and exceeds about
110.5 s. Do not extrapolate that into a measured completion time.

Loading improvements in that pair are cache-confounded. For the exact same
2,142 keys, summed API time fell from 250.619 s in the control to 28.355 s in
the trial; initial preparation fell from 18.14 s to 2.35 s. The control primed
the system cache. Its gate waits totalled about 28.8 s, versus about 108 ms in
the trial, but that difference cannot be attributed solely to scheduling.
Host load p50/p95 was 8.41/9.95 and 9.18/10.67 respectively.

These frozen binaries predate the triangle fan/strip index-reservation fix in
7af9123ca. Their approximately 9.5–10 GB whole-process RSS includes the old
8 GiB allocation, and their roughly 600 ms mission-entry draw-done stalls
are not pipeline waits. They do not validate the corrected port's memory or
worst-frame behavior. A cold/warm confirmation on the current corrected build
is required before adopting a first-launch policy.

For a user accepting a few minutes once, full preparation with progress is
the best-supported option for avoiding first-visit shader gates: the measured
warm repeat took 1.56 s. Background preparation is a promising immediate-play
alternative, but its uncached loading benefit and total completion time still
need measurement. Based on this evidence, the lead approved default full
preparation with a skippable native progress screen. Corrected-build live
validation of that new screen is pending; no completion-marker shortcut is used.

The four draw-owner probes (Good Egg and BattleShip scenarios 1–2) independently
passed their functional checks: 800 post-preparation first-use records had zero
coverage gaps. Across both stages, all 367 unique observed configs were seeded
and captured, with owner stacks covering J3D, JPA, fonts, layouts and procedural
draws. The two loading-time clear/intermission entries marked uncovered precede
manifest membership publication and are already seeded. This is sampled entry
coverage, not complete-mission or whole-game coverage. Consolidated evidence:
`build/pipeline-prep-measure/overnight-1/owners-all-summary.json`.

### Corrected-port startup UI validation

The frozen startup UI comparison under `build/pipeline-prep-measure/startup-ui-1/`
prepared all 8,721 variants in 220.639 seconds with empty app caches, versus
1.095 seconds on the same fixture's warm rerun (same binary and exact pipeline
keys). The warm startup wait was 0.972 seconds and the progress screen stayed
hidden. The OS Metal cache was not cleared; this measures the combined caches.
A real Return-key retry left 3,025 pipelines pending, continued preparation in
the background, and passed the mission-entry smoke with zero audio underruns.
Its remaining work finished during startup/menu, so it does not establish
sustained gameplay performance while cold compiles run. All three runs had no
stage-gate expiries or pipeline resolves logged at 10 ms or longer. Gameplay
p99 was 18.0–18.4 ms, and progress-loop outliers remained: this is not locked 60.
See `VALIDATION.md` in that artifact directory for exact metrics, cache caveats,
screenshots, and the earlier run named `skip` that did not exercise skipping.

### Factory dependency closure and PASS-only coverage repair

The collector follows NameObjFactory's one-to-many extra-archive table and
callback registrations, PlanetMapCreator's unique-child table, conservative
literal dependencies in selected actor/callback source units, recursively
constructed child classes, and StationedFileInfo shared resources. Source
file/line reasons and SHA-256 provenance are retained. Names resolve to canonical
disc spelling. Only StageData scenario/zone tables act as placement data; shared
registries must not pull every listed planet into every stage. Conditional
branches are unioned. Constructed names, indirect calls and arbitrary runtime
material mutations remain explicit gaps.

close_pipeline_seed_gaps.py creates an unpublished candidate from that input
manifest and a coverage-audit snapshot. Observed additions require retained
unassisted PASS smoke results, exit 0, no timeout or renderer/crash/heap errors,
and an unchanged source log. PASS_WARN with a clean renderer is eligible; failed
and NON_GAMEPLAY entries are not. The helper retains per-run validation and
cache/log/result hashes rather than labeling observations as offline replay.

The closure-4 candidate repaired all ten previously sub-95% stages to 100% of
recorded stage/global observations using 108 eligible runs. It adds 484 globally
missing observed configs (8,713 → 9,197 GX configs). Three other hashes occur only
in the excluded NON_GAMEPLAY Epilogue run and remain unseeded. All 45,985 legal
WGSL output variants generated successfully; database integrity and global/stage
byte equality passed. This is sampled corpus coverage, not whole-game rendering
or Metal compilation proof. The manifests are larger and cold stage-only prep
may cost more. Evidence and publication state:
build/pipeline-prep-measure/closure-4/REPORT.md and publish-result.json.
Frozen tuning app copies retain their older seeds for a fair worker comparison.

Final regeneration is under build/pipeline-prep-measure/closure-5/. The StageData
scope includes nested scenario archives as well as zone archives; a regression
requires scenario → child-zone traversal while rejecting shared-registry
expansion. That correction adds static dependencies to eight stages without
changing the global set or importing additional observations. The same 9,197
configs / 45,985 variants pass generation again; final publication and bundle
verification are recorded in closure-5/publish-result.json.

`collect_pipeline_observations.py` snapshots completed telemetry-survey and
extra-scenario sweep evidence for subsequent observed merges. It reparses logs
with the sweep analyzer, requires a clean unassisted PASS, checks provenance
files did not change, and backs up retained SQLite caches. Telemetry runs must
have a frozen executable matching both the recorded launch SHA-1 prefix and its
full SHA-256; prefix-only historical runs are explicitly excluded. Source paths,
hashes, manifest entries, command/environment metadata and original results stay
attached to the snapshot. No source runs are modified or relaunched.

The closure-6 snapshot accepts five such surveys and 43 extra-scenario runs.
It adds 174 global configs (9,197 → 9,371), closing all observed stage/global gaps
in its 18-stage corpus. All 46,855 legal WGSL output variants generate without
invalid configs. This includes HeavenlyBeach: its survey had 210 total first uses
(139 covered, 71 uncovered), while the combined survey/extra-scenario set has
256 keys. Against closure-5 seeds that union had 92 stage and 26 global gaps;
the candidate closes both. Later sweep completions require a new validated
snapshot. Publication/bundle status and the complete before/after table live in
build/pipeline-prep-measure/closure-6/. The final startup comparison stays frozen
with 9,197 GX configs; it does not measure this expansion's preparation cost.

Closure-7 completes the extra-scenario sweep: 52 retained clean PASS sweep runs
plus the same five full-identity surveys. Its nine later sweep runs add 28 global
configs and 87 stage rows (ReverseKingdom 17, SandClock 62, StarDust 8), bringing
the global set to 9,399. All 20 stages observed in that snapshot have full
stage/global membership; all 46,995 WGSL variants pass source generation.
Earlier provenance exclusions remain unchanged. See closure-7/REPORT.md and
publish-result.json under build/pipeline-prep-measure for the completed-sweep
coverage table and publication status. No additional live run measures this
delta; the startup timing corpus remains frozen.

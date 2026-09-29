# Pipeline preparation and diagnostics

Blocking GX rendering remains the default. `PETARI_PIPELINE_POLICY=async` opts
into Aurora's existing skip-draw path while a pipeline is pending. This can
hide geometry or effects for an unbounded number of frames under load; it is
an experiment requiring visual review, not a correctness-preserving default.
`PETARI_PIPELINE_THREADS=1..4` sets the compile pool. The automatic size is performance cores minus four,
clamped to 1–4 workers (four on the measurement M4 Max). Requested jobs
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
for at most ten seconds, prints residual wait/pending/failed counts, and proceeds.
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

```sh
build/locked-build.sh pipeline c++ -std=c++20 -pthread -O1 -Inative/include native/tools/pipeline_worker_tests.cpp -o build/pipeline-worker-tests
build/locked-build.sh pipeline build/pipeline-worker-tests
PETARI_PIPELINE_POLICY=async build/locked-build.sh pipeline build/pipeline-worker-tests async
build/locked-build.sh pipeline c++ -std=c++20 -pthread -O1 -Inative/include native/tools/pipeline_stage_tests.cpp -lsqlite3 -o build/pipeline-stage-tests
build/locked-build.sh pipeline build/pipeline-stage-tests
build/locked-build.sh pipeline python3 native/tools/pipeline_seed_tests.py
build/locked-build.sh pipeline cmake --build build/macos-gx \
  --target petari_pipeline_cache_inspect petari_gx_shader_tests -j2
build/locked-build.sh pipeline ctest --test-dir build/macos-gx \
  -R '^native_gx_shader$' --output-on-failure
```

These are component and source checks. Cold/warm route measurements are separate
and must hold app, sweep-lane and build locks in that order. All heavy CPU work,
including standalone compilations and sanitizer/stress runs, holds the build lock.


## Opt-in global preparation and retained observations

`PETARI_PIPELINE_GLOBAL_PRECOMPILE=1` imports bundled `__global__.db` before the
normal startup preparation loop. It is **off by default**: the full union can
require minutes on first use. The union includes all replayed archives, including
ones absent from static stage maps, plus the observed baseline and explicitly
attributed additions. Global jobs use the background queue and utility QoS;
requested draws retain priority. This implementation uses the existing blocking
startup preparation screen/log; it does not quietly compile all shaders during
normal gameplay. Never-drawn speculative rows are omitted from normal startup
when the opt-in flag is absent, even if an earlier opt-in run imported them.

Configs persist in the user's Aurora SQLite DB; Dawn translation blobs persist
separately. Neither stores ready Metal pipeline objects. A warm launch still
creates Metal libraries/pipelines, so near-instant global preparation is not
established. `[gx global prep]` reports progress every five seconds. Shader API
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
and global seeds; frozen measurement bundles retain the original 8,270-row union.
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

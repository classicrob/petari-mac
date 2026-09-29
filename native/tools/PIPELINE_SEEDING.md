# Pipeline preparation and diagnostics

Blocking GX rendering remains the default. `PETARI_PIPELINE_POLICY=async` opts
into Aurora's existing skip-draw path while a pipeline is pending. This can
hide geometry or effects for an unbounded number of frames under load; it is
an experiment requiring visual review, not a correctness-preserving default.
`PETARI_PIPELINE_THREADS=1..4` sets the compile pool (default 2). Requested jobs
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
still reads a small SQLite manifest on the calling game thread; measure enqueue_ms.

## First-cut seeds

Generate four usable manifests from a captured cache:

```sh
python3 native/tools/export_pipeline_stage_seeds.py \
  --cache build/observatory-user-2/pipeline_cache.db --output build/pipeline-seeds
```

Defaults: AstroGalaxy, EggStarGalaxy, ScenarioSelect, GalaxyMap. All four initially
contain the same **shared observed baseline**, explicitly labeled in JSON sidecars.
This is not an offline material replay and does not prove full stage coverage.
The local 2026-09-29 baseline contains 602 rows (599 GX and three clears).
Runtime preparation adds clear masks independently.

Use `PETARI_PIPELINE_SEED_DIR="$PWD/build/pipeline-seeds"` for an integration run,
or copy the directory into the app's Resources. The exporter can use new
`--log build/<run>.log --stage EggStarGalaxy` records to filter the cache into a
stage-observed seed. Missing attributed configs are listed in the sidecar. A
requested stage with no attribution fails instead of silently producing a tiny
seed. Keep broad shared configs until measured coverage justifies narrowing.
The tool reads its source DB read-only and replaces only its named output files.

## Broader offline coverage: implemented collector and remaining replay

```sh
python3 native/tools/collect_pipeline_seed_inputs.py \
  --files build/game-data/RMGE01/files --output build/pipeline-seed-inputs.json
```

This inventory decodes RARC/Yaz0/Yay0, follows scenario ZoneList and extensionless
stage placement tables, resolves static NameObjFactory archive aliases, and
collects shared player/object families plus galaxy/map/scenario layouts. It
records exact INF1 material/shape pairs, MDL3 packet offsets and input hashes.
All scenario layers are included. `--stage` can target other stages. Unresolved
placements and nonmodel emitters are explicit rather than silently counted as
covered. Some placements legitimately have no model; others need dynamic archive
callbacks, which the static resolver cannot enumerate.

Observed collector output: AstroGalaxy 165 archives, 142 models, 1,087 pairs,
79 unresolved placement names; EggStarGalaxy 202 archives, 162 models, 611 pairs,
124 unresolved names. Output states `config_seed_emitted: false`.

Next implementation steps for a true offline config seed:

1. Build a CPU-only recorder around native J3D material loading and Aurora's GX
   state/config builder. Reuse native endian/pointer fixups and J3D load flags.
   Feed each inventoried material and its exact shape vertex descriptor through
   the same GX register/FIFO decoding path as runtime. Capture PipelineConfig
   immediately before pipeline lookup, without creating a device or drawing.
2. BDL's MDL3 packet can supply fixed BP/XF material state; BMD needs native MAT3
   material generation. Include SHP1 VCD/VAT, texgen/channel configuration and
   array-index handling. Isolated material packets are insufficient to recover
   the shader's complete input configuration.
3. Replay material animation states that alter configuration, lighting/depth/
   blend/pass presets, indirect/bloom/water variants and native procedural
   renderers. Add layout and particle emitters for ScenarioSelect/GalaxyMap;
   BRLYT inventory alone does not construct their GX state.
4. Serialize using the existing version/hash builder. Compare offline hashes to
   fresh per-stage first-use logs; merge conservative observed configs until the
   collector's missing families are understood. Exhaustive combinations are too
   broad: instantiate actual engine presets rather than every possible GX state.

Ownership remains native/gx pipeline patches and new native/tools collectors;
scene hooks/CMake belong to the lead. A native J3D/FIFO replay target will need
explicit CMake/dependency integration and a bounded extraction seam before use.

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
three TEV stages and one indirect stage. This inspector currently uses no normal
attachment and DstAlphaMode=None; real layouts/prepasses can produce other variants.
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
c++ -std=c++20 -pthread -O1 native/tools/pipeline_worker_tests.cpp -o build/pipeline-worker-tests
build/pipeline-worker-tests
PETARI_PIPELINE_POLICY=async build/pipeline-worker-tests async
c++ -std=c++20 -pthread -O1 native/tools/pipeline_stage_tests.cpp -lsqlite3 -o build/pipeline-stage-tests
build/pipeline-stage-tests
python3 native/tools/pipeline_seed_tests.py
build/locked-build.sh pipeline cmake --build build/macos-gx \
  --target petari_pipeline_cache_inspect petari_gx_shader_tests -j2
build/locked-build.sh pipeline ctest --test-dir build/macos-gx \
  -R '^native_gx_shader$' --output-on-failure
```

No Petari app was launched for these worker checks. These are component and source
checks; cold/warm visual playtests and timing distributions remain integration work.

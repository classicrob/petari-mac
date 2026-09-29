# Offline asset-reference sweep

Run from the repository root; this never launches the game:

```sh
python3 native/tools/asset_reference_sweep.py \
  --output build/asset-reference-sweep.json \
  --inventory build/asset-reference-inventory.json
build/locked-build.sh asset-sweep ctest --test-dir build/macos-gx \
  -R '^native_(asset_reference_sweep|galaxy_name_plate)$' --output-on-failure
```

`--files` accepts an extracted disc's `files` directory. `--repo` selects the
source tree. `--reuse-inventory` explicitly reuses the inventory snapshot for a
quick source-only pass; it does **not** check whether disc assets have changed.
The report records that choice, source and scanned-asset SHA-256 hashes,
per-category name counts, individual lookup locations, evidence, suggestions,
and parser errors. A report is an audit, not a passing full-game validation.

## Coverage and confidence

The inventory reuses `collect_pipeline_seed_inputs.py`'s bounded Yaz0/Yay0,
RARC and BCSV readers. It parses BRLYT pane/group blocks, BRLAN animation targets
and filenames, BDL/BMD joint/material/texture name tables, BCSV field hashes and
string values, BMG message counts/numeric IDs, MessageId.tbl string IDs, and
BSTN sound names through the audio archive command stream. ParticleNames and
AutoEffectList provide particle/effect names. Model animation presence is checked
by filename and typed J3D header; their payloads are not decoded.

Every supported source file under `src/` is scanned, but this is a lexical audit
of recognized APIs and identifiable resource literals, not a C++ compiler or an
exhaustive proof that every string is correct. API argument indices for layout
operations come from their definitions. Layout scope is inferred only for a
`this` lookup in a class with one literal layout initialization; follow-actor
operations use the followed actor's argument. Member functions named `startAnim`
are not confused with `MR::startAnim`.

- `definite_mismatch`: a name absent from the explicit layout's available assets,
  or a sound absent from the structurally parsed sound registry. Reachability,
  regional differences and intended replacement still require source review.
- `present_scoped`: found in the inferred layout. This does not prove that the
  operation uses the right pane type or animation layer.
- `present_global`: exists somewhere in the corpus; ownership remains unproved.
- `pattern_candidates`: printf-like pattern matches exist, but reachable format
  arguments are unknown. Width is deliberately permissive; this cannot certify
  every expansion. Local literal assignments, literal ternary arms and simple
  local `snprintf` formats are followed conservatively.
- `unverifiable_dynamic` / `unverifiable_missing`: unsupported dataflow,
  optional/runtime-created resources, missing ownership, or missing global names.

Pane names are case-sensitive. Layout animation and model-resource names follow
the engine's lowercase resource lookup. File-presence comparisons use lowercase
names, but do not certify host filesystem lookup semantics. BCSV hashes can
collide and global hash presence does not prove membership in the accessed table.
RARC directory-qualified paths, embedded executable BCSV tables, preprocessor
branches, aliases, dynamic effect registration, and general interprocedural
string construction remain gaps. Parse errors are retained and prevent definite
layout claims for affected archives.

The RMGE01 localized message archives omit final FLI1 alignment padding while
retaining that padding in the declared block length. The parser accepts this
only for the final FLI1 block, after validating that all declared entries exist;
truncated entry data still fails. The omitted byte counts are inventoried.

## Reviewed source fixes

- `DinoPackunBattleEggVs2::exeTurn`: `SE_BM_D_PAKKUN_LAVER` →
  `SE_BM_D_PAKKUN_SLAVER`. The same animation/voice sequence in
  `DinoPackunBattleEgg::exeTurn` requests the latter; BSTN contains only that name.
- `Tico::exeReaction`: `SE_BM_BUTLER_ABSORB` → `SE_SM_BUTLER_ABSORB`.
  `ButlerStateStarPieceReaction::exeWait` uses the latter with the same
  `limitedStarPieceHitSound` operation; BSTN contains only that name.

The RMGE01 `sys/main.dol` also contains the replacement literals (9 and 2
occurrences respectively), and neither original misspelling. This corroborates
the spellings; it is not a disassembled call-site trace.

These restore registered sound requests. Their audible gameplay behavior has
not been playtested. The regression reads real BSTN names and checks both source
call sites, rejecting mutations back to the old strings. It also injects both
original GalaxyNamePlate pane typos into a temporary source copy and requires
the general sweep to report both, without changing the working checkout.

## Reviewed remaining name mismatches

- `RaceManagerLayout::playRecord`: two requests for animation `Record`, absent
  from `Race.arc`. The source explicitly labels this function an approximation
  absent from the original build; no caller exists in `src/`. The archive has a
  `Record` pane, but choosing a different animation would be speculation.
- `PowerStarList::setTotalPowerStarNumForMessageBoardCapture`: `TxtStarTotal` is
  absent from RMGE01's `AllStarList.arc`. Its caller is the message-board capture
  path. There is no unambiguous total-star pane replacement. Regional/source
  differences or an incomplete capture implementation need investigation before
  changing behavior. The native follow-up now treats this capture-only field as
  optional: it skips the update when absent and retains the original update when
  present. See [runtime guard findings](LAYOUT_REFERENCE_GUARDS.md). The lexical
  sweep still reports the literal because it does not prove conditional reachability.

No allowlist hides these findings, and no pass threshold was lowered.

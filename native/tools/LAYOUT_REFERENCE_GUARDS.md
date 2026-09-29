# Native layout-reference guards

The guards are native-only. Required missing panes, pane controls and animations
are diagnosed before mutation and skipped. Optional existence probes stay quiet.
Native missing pane indices are `-1`, so a typo cannot select the root's control
or matrix slot. Scalar queries return zero, missing visibility queries return
hidden, missing stopped-animation queries return stopped, and output coordinate
queries initialize their output. Required pointer queries return null; audited
consumers check it. An unsuccessful animation start leaves the previous animation
bound and its frame unchanged.

The MR layout utilities, LayoutManager, LayoutPaneCtrl, LayoutAnmPlayer and raw
pane consumers in LayoutActor, dialogue text processing, pane-effect attachment,
Mii selection, ISBN fields and PowerStarList are covered. This is defense against absent names,
not a substitute for validating arbitrary malformed layout binaries.

## Diagnostics and smoke integration

Include `petari/asset_diagnostics.hpp` (host-safe, no SDK headers). These C-linkage
hooks return monotonically increasing process-lifetime `uint64_t` counts:

```cpp
petari_layout_missing_reference_count();
petari_sound_missing_reference_count();
petari_layout_missing_reference_unique_count();
petari_sound_missing_reference_unique_count();
```

The first two count **every failed required lookup**, including repeats. The
last two count distinct `(layout, name)` pairs (sound uses `BSTN` as its owner).
Logging happens once per pair; e.g.:

```text
[layout] missing pane layout="GalaxyNamePlate" name="TxtGaxyName" count=1; operation skipped
[layout] missing animation layout="Race" name="Record" count=2; operation skipped
```

The lead/sweep worker should require both occurrence counters to be zero before
reporting smoke PASS, and include their values in final run diagnostics. No
counter reset is exposed; startup failures must not disappear at scene changes.
This worker exposes the hooks but does not modify the concurrently edited smoke
state machine or claim that PASS gating has already been wired.

The diagnostic ledger is thread-safe, has process lifetime, and enters
`PetariNative::HostAllocationScope` before all standard-library storage operations.
It retains full names rather than truncating them into potentially equal keys.

## TxtStarTotal reachability and regional handling

`PowerStarList::init` registers `drawForMessageBoardCapture` in
`DrawType_MessageBoardCapture`. That callback calls
`setTotalPowerStarNumForMessageBoardCapture`. It is separate from normal star-list
drawing and ordinary pause-menu drawing.

The source sequence is: select the star list's Capture button → wait for its
selection animation → answer Yes to `AllStarList_ConfirmCapture` → enter
`exeCaptureWait` → request an ODH image. `GameScene::drawOdhCapture` executes the
capture category only when its `_29` flag and the ODH capture request are both
set. It then captures the image; the star-list flow advances to sending it to
the Wii Message Board. Canceling the prompt does not request capture.

The current source tree contains no construction/initialization caller for
`PowerStarList` outside its own implementation. Thus this is a latent callback
failure if that screen is connected, not a demonstrated reachable native
pause-menu crash. Its ordinary drawing path does not set the total-star field.

RMGE01 `AllStarList.arc` has no `TxtStarTotal` pane, and its original `main.dol`
has no such string. No substitute pane is justified. On native, this one
region-dependent capture field is checked using the optional lookup and omitted
when absent; when present, the original count-formatting/update body runs. This
does not disable the whole capture, redirect text to an unrelated pane, or hide
failures of other required pane operations.

Before the guard, `executeTextBoxRecursive` tried the null-safe DynamicCast, then
unconditionally dereferenced the missing pane's child list. It was **not** a
benign no-op in the native implementation. The unmodified Wii source has that
same unchecked operation; the original US executable's precise capture-function
control flow has not been disassembled or playtested. Native-only guards do not
change Wii source behavior.

## Unknown sounds

An unknown name in a recognized category returns sound ID `-1`; AudSoundObject's
start/level-start paths already reject that ID. An unrecognized category could
instead index `mGroupItemOffsets[-1]`, and null/short names could be read out of
bounds. AudSoundNameConverter now validates those inputs on native, reports the
missing name and returns `-1`. Unknown names in valid categories are also counted.

## Regression

`native_layout_asset_guards` compiles actual production function bodies into a
CPU-only fixture with stand-in NW4R containers. It uses ASan/UBSan and verifies:
missing panes cannot alias root controls or mutate text; missing animations leave
bindings and frame state unchanged; invalid layers do not access arrays; optional
capture fields work both absent and present; malformed/unknown sound names return
`-1`; and 400 failures across four threads produce one distinct-pair diagnostic.
The fixture's HostAllocationScope implementation is a counting stub; full engine
compilation and the existing allocation tests remain separate validation.

```sh
build/locked-build.sh asset-guards cmake --build build/macos-gx \
  --target petari_game_objects petari_host_runtime -j4
build/locked-build.sh asset-guards ctest --test-dir build/macos-gx \
  -R '^native_(layout_asset_guards|asset_reference_sweep|galaxy_name_plate)$' \
  --output-on-failure
```

No app launch is required by this regression. Live capture, sound playback and
fault-free later-game behavior still require integration playtests.

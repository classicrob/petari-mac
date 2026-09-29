# Native Apple Silicon port

This is an in-progress source port of Petari at `e5bc761c0`, whose commit marks the
completed SMG1 decompilation. **The opening is playable on Apple Silicon;
full-game compatibility remains in progress.**
The upstream README's decompilation progress wording does not describe this fork's
native-port progress. Native compilation and successful game behavior are separate
milestones.

The full arm64 `Petari.app` now links and starts the Metal window, platform
services, and original game entry point. The original frame loop runs at about
60 Hz, audio initialization and stationed-resource loading complete, and the fresh
save check finishes. The full title smoke accepts A+B, reaches selectable save
slots, and exits through the game's normal power-off path with status 0. The
extended smoke creates a Mario file through the real pointer, completes both
saves, presses Start, advances all five prologue pages and Peach's letter, and
finishes the arrival cutscene. After control is handed to Mario, holding the
movement binding for 90 frames moves him 963 units in Peach's Castle Garden.
The run then shuts down normally with status 0 (development run app25).
A separate audio boot test plays a real coin
sound and the title music through JAudio and the native DSP/AI path, including
50 seconds of streaming through a loop, and the transition to file-select music
with its chord table. Focused
tests have rendered both a J3D Mario model and the default
Mii face/icon on Metal, with GPU pixel readback checks.

To play on this development checkout, double-click `Play Petari.command` in the
repository root. It locates the built app and extracted disc independently of
the Terminal's current directory. Normal saves, settings and crash reports go to
`~/Library/Application Support/Petari`, separate from automated test saves.
The launcher accepts the app's `--user DIR` option for a different save location.

At the title, hold **Space + left mouse** together (A+B). Point with the mouse
and press **Space** or **right mouse** to select menu buttons. **WASD** moves,
**Space** jumps, **F** spins, **Shift** is Z, and **Escape** pauses/goes back.
See [all input bindings](input/README.md#default-bindings) for camera and tilt controls.
Initial visits to new scenes can pause while Metal shader pipelines compile.
Later stages, movie rendering in the game, and overall visual fidelity remain
unverified; this is a development build.

With the GX build configured below, build and launch the development app using:

```sh
cmake --build build/macos-gx --target petari -j 8
build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari \
  --disc build/game-data/RMGE01 --user build/native-boot-state
```

The explicit `--user` directory keeps development saves, settings, and crash
reports under `build/`. Native saves are not byte-compatible with Wii saves.
Set `PETARI_TRACE_BOOT=1` when launching to log startup phases, heap headroom,
and frame progress. The trace is disabled by default.

Set `PETARI_SMOKE=title` with a fresh `--user` directory to exercise strap-screen
input and A+B on the title through the normal input layer. The script passes only
when the file selector enters its selectable state, then requests the game's normal
shutdown. Exit statuses are 0 for pass, 1 for failure, 2 for a blocked save
sequence, 124 for stalled frames, and 125 for shutdown timeout. Its component
tests and the complete full-app title run pass.

`PETARI_SMOKE=playable PETARI_SMOKE_FRAMES=36000` extends the script to create a
fresh Mario file, advance the prologue, and require Mario to move in response to
the movement binding. It uses observed UI targets and the normal input layer.
This extended run passes end to end on RMGE01 with an empty test save directory.

## Build the native core

Requires Apple Silicon, Xcode Command Line Tools, and CMake 3.25 or newer.
No game assets, Wii compiler, Rosetta, or emulator are needed for these tests.
Run from the repository root:

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64
ctest --preset macos-arm64
```

This builds `build/macos-arm64/libpetari_core.a` and
`build/macos-arm64/petari_core_tests` for arm64, with AddressSanitizer and
UndefinedBehaviorSanitizer enabled. Tests remain active in Release builds:

```sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
```

The library compiles Petari's original Nerve/Spine/ActorState code, JMath and trig
tables, RNG, audio sequence reader, and the portable portions of the SDK math
code. Native replacements supply the PowerPC vector, affine matrix, quaternion,
and scalar-intrinsic operations. Tests cover state transitions, RNG overflow,
in-place math, inverse composition, projection conventions, table endpoints,
unaligned big-endian reads, and audio sequence call/return/loop behavior.

`PETARI_NATIVE` keeps the native ABI changes separate from the matching Wii build:
`u32`/`s32` remain 32 bits, host pointers remain 64 bits, and `nullptr` and
`override` retain their C++ meanings. Native code uses Apple's standard library
instead of the original MSL C runtime. The original paired-single assembly remains
in its source files behind conditional compilation.

The scalar math replacements are not bit-exact emulations of PowerPC reciprocal
estimates or paired-single rounding. Floating-point behavior still needs gameplay
comparison with the original game. The matching Wii build has not been validated
here.

## GX/Metal integration probe

This optional development executable renders a rotating textured triangle or a disc model through
Aurora's native GX implementation and Metal. The transformation comes from
Petari's `JMAEulerToQuat` and native matrix path. It is a renderer integration
test, not a game launcher.

Vertex submission uses Petari's original GX declarations with native function
calls replacing Wii FIFO writes. Native texture and palette handles have the
storage required by Aurora. The `native_gx_abi` test compares layouts and selected
enum values across the two header sets, checks texture initialization for buffer
overwrites, and checks the BP decoder's legacy texture/palette address resolution.
The probe alternates little- and big-endian indexed vertex arrays and exercises
real J3D model loading and drawing. A corrected 120-frame Mario run on M4 Max
Metal passed with repeated scene-heap destruction/recreation. GPU readback found
31,797 non-background pixels with matching depth coverage in its first frame;
two captures in that frame preserved the image across a mid-frame submission.
The last frame submitted 11 draws, 58,014 vertex bytes and 29,268 index bytes.
Earlier runs counted draw submissions but omitted GXInit in the probe, so those
runs did not establish visible geometry. The corrected probe initializes GX.
A subsequent four-frame run delivered two real GX draw-sync callbacks that read
their ticket-specific color/depth snapshots, and its mid-frame GXDrawDone returned
before end_frame. Pixel checks establish nonempty output and capture preservation, not visual
fidelity; the window has not yet been visually inspected.

Build-time patches to the pinned Aurora BP decoder translate texture addresses
through the game memory arenas and resolve palette TMEM slots. A second patch
removes Aurora's placeholder VI exports, retaining its window helpers while the
native platform supplies real retrace timing. Patch anchors fail configuration
if the backend source changes unexpectedly. The VI bridge forwards mode changes;
Draw-sync events now use the platform interrupt thread, and mid-frame capture
submission is tested on Metal. XFB selection, blanking, dimming, and the image
rectangle are connected to the game frame loop. Focused Metal tests verify
display-copy snapshots and presentation; full-game behavior remains unverified.

Requires SQLite 3.37 or newer. On a Homebrew installation:

```sh
brew install cmake sqlite
cmake -S . -B build/macos-gx \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DPETARI_BUILD_GX_PROBE=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sqlite)"
cmake --build build/macos-gx --target petari_gx_probe -j 8
./build/macos-gx/native/gx/petari_gx_probe.app/Contents/MacOS/petari_gx_probe --frames 120
```

Pass `--model build/game-data/RMGE01/files/ObjectData/Kuribo.arc` to exercise
the real J3D model path. Omit `--frames` to leave the window open. Run from the repository root so the
probe's configuration and GPU caches stay under `build/gx-probe-state`.
The build pins Aurora to `08122911e8621acb7ded6563813b264bec1494b5` and downloads
its dependencies. A graphical macOS session and Metal device are required.
The development executable may link Homebrew libraries; it is not a self-contained
distributable `.app` bundle.

Aurora is MIT licensed. Its source and build instructions are at
[encounter/aurora](https://github.com/encounter/aurora). Keep its license and the
licenses of its dependencies when packaging a future application.

## Resource validation

The default build also produces `petari_asset_check`. It validates big-endian
RARC and U8 archive tables, resource bounds, and Yaz0/Yay0 decompression. Host metadata
is stored separately from the serialized Wii structures. Sanitizer tests include
truncated input, invalid offsets, overlapping compression runs, and compressed
resource payloads. JKR archive classes now build native heap-owned metadata
from serialized RARC tables while retaining original resource payload bytes.
A real JKR heap/decompression-thread test mounts all 2,228 disc RARC archives and
compares 24,788 resources, including lookup and cached-pointer identity. The original
SDK ARC API for U8 archives now reads big-endian fields and preserves host
pointers; validate the buffer with `parseU8` before calling `ARCInitHandle`, whose
original signature has no buffer length.

```sh
./build/macos-arm64/petari_asset_check /path/to/extracted-game/files
```

Passing a directory recursively checks `.arc` files; passing one archive checks
only that archive. Successful validation does not establish model, animation,
text, audio, or gameplay correctness.

On the extracted USA `RMGE01` revision 0 assets, validation passed for 2,228 RARC
archives, four U8 archives, and 25,108 file payloads. The native DVD service also
passed a full byte comparison of all 2,371 files (3,228 MiB) against direct host
reads, using the original disc file table. The extraction used nodtool 1.4.4
with partition hash validation enabled. Retail assets are never included in the repository. When the extracted files
exist locally, CTest enables additional read-only model, message, and table tests;
synthetic tests run without them. The full-disc DVD/archive checks are manual.

## Native source and runtime conventions

Apple Clang cannot emit Shift-JIS string literals directly. The native build
generates source/header copies under its `legacy-sjis` directory, converting
non-ASCII narrow literals to explicit byte escapes. The original editable files
stay UTF-8. Compiler diagnostics use their original paths through `#line`.
Comments retain Unicode. Legacy `wchar_t` tokens become `PetariChar16` (C++
`char16_t`), `L` literals become `u` literals, and wide-string calls use the
separate `petari_utf16_*` runtime. System headers and native host code retain the
macOS wide-character ABI. BMG message loading converts big-endian UTF-16 to host-order copies; save,
Mii, and other text-bearing formats require their own conversion. Native helper namespaces use `PetariNative`, because the
original game already has an enemy class named `Petari`.

The native memory layer reserves 128 MiB for MEM1 and 256 MiB for MEM2. It supplies
the arena boundaries and boot information used by heap initialization. The SDK
allocator's descriptor sizes and alignment arithmetic now preserve host pointers.
Physical-address conversion accepts only pointers within those arenas. The native
Aurora BP adapter uses that mapping for legacy texture and palette commands;
texture image and palette bytes retain their Wii byte order.
Fixed-size child heaps still need sizing checks against larger native objects.
`petari_boot` supplies main-thread `OSInit`, initializing the arenas and native
scheduler once. `petari_heaps` builds the real JKR expandable heap and its
allocation operators; its sanitizer test covers root/child startup, mixed head
and tail allocations, alignment, host allocation isolation, and destruction.
Its test diagnostics print and abort instead of linking the game's framebuffer
exception viewer; a native game crash UI has not been integrated.

JMap/BCSV headers, fields, and cells now decode big-endian values without changing
their on-disc layouts. The real-data test reads 11,494 stage tables and over
1.4 million cells. When the extracted RMGE01 files are present, CTest includes
this read-only check in addition to the synthetic format tests.

KCL collision resources now normalize header, prism, vector, and octree data
without changing their on-disc sizes. The integrated asset check converts all
1,016 collision files and validates about 5.2 million octree lookups. Camera
animation and other game-specific resource readers are being converted separately.

Ordinary allocation defaults to the host allocator on non-game threads. Native
OS thread startup registers the threads that should use the current JKR heap.
All JKR heaps must reside in the MEM1/MEM2 arenas; global deletion of pointers
outside those arenas bypasses the mutable game-heap hierarchy.
Calls into host libraries must use `PetariNative::HostAllocationScope` when a
game heap is current, so host objects do not accidentally get freed with a scene.
The native language selector uses the mounted disc region and constrains the
selected language to that region's available languages. This does not establish
overall compatibility for additional regions.

The approved keyboard and mouse layout is recorded in [CONTROLS.md](CONTROLS.md).
The native input component and remapping are implemented and tested against the
original KPAD and game WPad code; in-game control validation is still pending.

## Audit the remaining source

```sh
python3 native/tools/audit_compile.py
python3 native/tools/audit_compile.py --output build/libraries-audit.json src/JSystem src/nw4r
```

The JSON reports contain compiler diagnostics for every checked translation unit.
Exit status 1 means there are remaining compile failures. This is a syntax audit;
it does not link the game or establish runtime correctness. the baseline game audit covered
1,605 C++ translation units; the Wii build manifest also includes SDK
and other library units.

An explicit CMake target attempts to compile **all** game C++ sources without
silently omitting failing files:

```sh
cmake --build build/macos-arm64 --target petari_game_objects -j 8
```

All 1,605 baseline game C++ sources built with the UTF-16 source transformation.
Native-only additions and ongoing changes need subsequent full-build verification.
The target also embeds the game's two built-in BCSV tables as Mach-O assembly.
It is excluded from the default component build because it is large. It is an
object target, not a game executable.

## Remaining validation and packaging

1. Exercise gameplay beyond the opening: jumps, spins, encounters, movies,
   subsequent stages and save/reload across sessions. The complete opening smoke,
   pointer selection, both file-creation saves, movement and normal shutdown pass.
2. Continue serialized resource runtime validation. Archive, BCSV,
   BMG, KCL, models, layouts, fonts and TPL conversions have real-disc tests.
   All 1,761 disc model/material images construct real J3DModelData successfully.
   All 4,753 disc animations also load into real animation objects and evaluate
   successfully. Particle and audio conversion tests pass; their full-game
   behavior still needs integration.
3. Validate the integrated GX draw-sync, FIFO, XFB presentation, blanking and
   frame-loop bridges in actual scenes. Metal tests cover model drawing,
   token-time color/depth reads, display copies, and Mii face/icon rendering.
   Overall visual fidelity, depth effects, and shadows remain unvalidated.
4. Exercise platform and input services in the running game. Keyboard/mouse tests run
   the original KPAD and game WPad code. The DSP/AI mixer has synthetic and real
   AFC-data checks, and the SDL output device is wired into startup, but the
   complete game audio sequence still needs testing. See native/platform/STATUS.md
   for fidelity limitations. Mii boot and rendering pass focused tests; particle
   loading and movie playback also need full-game validation.
5. Validate boot, title screen, gameplay, pointer/shake/tilt equivalents, audio,
   saves, scene transitions and performance using real assets.
6. Package an arm64 `.app` with its runtime dependencies and repeat validation
   outside the development checkout.

Petari's matching build targets
Korean `RMGK01`; `config/RMGK01/config.yml` records the expected `sys/main.dol`
SHA-1. For gameplay, an extracted game tree also needs the full `files/` content,
not just `main.dol`. A user-provided USA image was extracted under the ignored
`build/game-data` directory; its disc header identifies `RMGE01`, revision 0.
Native compatibility with that regional asset set has not been established.
No retail game data is included in this repository.

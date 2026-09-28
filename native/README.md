# Native Apple Silicon port

This is an in-progress source port of Petari at `e5bc761c0`, whose commit marks the
completed SMG1 decompilation. **There is no playable native game executable yet.**
The upstream README's decompilation progress wording does not describe this fork's
native-port progress. Native compilation and successful game behavior are separate
milestones.

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
here because the original game executable has not been supplied.

## GX/Metal integration probe

This optional development executable renders a rotating colored triangle through
Aurora's native GX implementation and Metal. The transformation comes from
Petari's `JMAEulerToQuat` and native matrix path. It is a renderer integration
test, not a game launcher.

Requires SQLite 3.37 or newer. On a Homebrew installation:

```sh
brew install cmake sqlite
cmake -S . -B build/macos-gx \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DPETARI_BUILD_GX_PROBE=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sqlite)"
cmake --build build/macos-gx --target petari_gx_probe -j 8
./build/macos-gx/native/gx/petari_gx_probe --frames 120
```

Omit `--frames` to leave the window open. Run from the repository root so the
probe's configuration and GPU caches stay under `build/gx-probe-state`.
The build pins Aurora to `08122911e8621acb7ded6563813b264bec1494b5` and downloads
its dependencies. A graphical macOS session and Metal device are required.
The development executable may link Homebrew libraries; it is not a self-contained
distributable `.app` bundle.

Aurora is MIT licensed. Its source and build instructions are at
[encounter/aurora](https://github.com/encounter/aurora). Keep its license and the
licenses of its dependencies when packaging a future application.

## Audit the remaining source

```sh
python3 native/tools/audit_compile.py
python3 native/tools/audit_compile.py --output build/libraries-audit.json src/JSystem src/nw4r
```

The JSON reports contain compiler diagnostics for every checked translation unit.
Exit status 1 means there are remaining compile failures. This is a syntax audit;
it does not link the game or establish runtime correctness. `src/Game` currently
contains 1,605 C++ translation units; the Wii build manifest also includes SDK
and other library units.

An explicit CMake target attempts to compile **all** game C++ sources without
silently omitting failing files:

```sh
cmake --build build/macos-arm64 --target petari_game_objects -j 8
```

It is excluded from the default core build because the source conversion is
unfinished. It is an object target, not a game executable.

## Work still required for a playable port

1. Finish Clang compatibility throughout the game and libraries: remaining
   MSL algorithm extensions, overloads that depend on 32-bit `long`, numeric
   uses of `nullptr`, and invalid pointer-to-32-bit casts. Hash tables that
   store pointers need pointer-sized payloads, not casts that hide truncation.
2. Separate serialized Wii resource layouts from native host structures.
   Archive, model, animation, layout, and audio loaders need explicit big-endian
   conversion and fixed-width offsets. The audio sequence reader is the first
   converted consumer; this is not a repository-wide endian conversion.
3. Connect the game's GX/GD/J3D display-list and texture paths to Aurora. The probe
   only establishes the basic API path; it does not validate game rendering,
   pixel formats, depth effects, shadows, or EFB copies.
4. Supply the Wii OS, heap, threading, DVD/archive, VI timing, save-data, controller,
   and audio/DSP services used by `GameSystem`. Hardware register access cannot
   run on a Mac. Native implementations must replace it.
5. Add the native game entry point and connect the actual frame loop. Validate
   boot, title screen, gameplay, Wiimote pointer/shake equivalents, audio, saves,
   scene transitions, and performance using real assets.
6. Package an arm64 `.app` with its runtime dependencies and repeat validation
   outside the development checkout.

The game-data path has not yet been provided. Petari's matching build targets
Korean `RMGK01`; `config/RMGK01/config.yml` records the expected `sys/main.dol`
SHA-1. For gameplay, an extracted game tree also needs the full `files/` content,
not just `main.dol`. Support for other regional asset sets has not been verified.
No retail game data is included in this repository.

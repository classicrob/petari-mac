# Petari for Apple Silicon (unofficial native port)

A native macOS (Apple Silicon) build of **Super Mario Galaxy**, made from the
[Petari](https://github.com/SMGCommunity/Petari) decompilation. The original game
code is compiled for arm64 and runs directly on your Mac, rendering with Metal
through [Aurora](https://github.com/encounter/aurora). There is no emulator.

> [!IMPORTANT]
> **This is an unofficial, independent project, not affiliated with the Petari project or its
> Discord, and not with Nintendo.** The Petari decompilation itself is not meant
> to be a PC port: please do **not** ask the Petari maintainers or their Discord
> about this port. Report problems here instead.
>
> **No game data is included.** You need your own copy of the game, extracted
> from a disc you own. This repository contains no Nintendo assets and no
> download links for them.
>
> Most of the native port (everything under `native/` and the `PETARI_NATIVE`
> hooks) was written by AI models under the repository owner's direction (see
> [Acknowledgments](#acknowledgments)). It is not decompilation work and is not
> intended for submission upstream.

The game is playable but this is still a development build: expect bugs, and
see [Known issues](#known-issues).

## Requirements

- A Mac with Apple Silicon (M1 or newer). Intel Macs are not supported.
- **Xcode 26 or newer** (or its Command Line Tools). Aurora uses C++20
  `std::jthread` and `std::stop_token`, which Apple's libc++ only has from Xcode
  26: the build works with Xcode 26.4 (Apple clang 21) and fails with Xcode 16.4
  (Apple clang 17). Versions in between are untested. Check with
  `clang --version`; switch with `sudo xcode-select -s /Applications/Xcode.app`.
- Developed on macOS 27. The app is built for the macOS version of your SDK (no
  older deployment target has been tested).
- [CMake](https://cmake.org/) 3.25 or newer, and SQLite 3.37 or newer
  (Homebrew: `brew install cmake sqlite`).
- An internet connection for the first configure: CMake downloads Aurora and its
  dependencies (SDL3, Dawn, Dear ImGui, Tracy, xxHash) at pinned versions.
- About 10 GB of disk space for the build, plus the extracted game.

## Your game data

Petari needs the **USA release of Super Mario Galaxy, revision 0 (game ID
`RMGE01`)**. Other regions and revisions have not been tested.

Make a backup of your own disc (for example with a Wii and a disc-dumping
homebrew tool), then extract its data partition:

- [Dolphin](https://dolphin-emu.org/): right-click the game, **Properties >
  Filesystem**, right-click the **Data Partition**, **Extract Entire Partition**.
- Or Wiimms ISO Tools: `wit extract <your-image> build/game-data/RMGE01`.

Put the result at `build/game-data/RMGE01` in this repository (or anywhere,
and point `PETARI_GAME_DIR` at it). Either layout works:

```
build/game-data/RMGE01/
  files/          the game's files (StageData/, ObjectData/, AudioRes/, ...)
  sys/            boot.bin, fst.bin, ... (optional, but used when present)
```

or Dolphin's `build/game-data/RMGE01/DATA/files` and `DATA/sys`.

## Build

From the repository root:

```sh
brew install cmake sqlite
cmake -S . -B build/macos-gx \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DPETARI_BUILD_GX_PROBE=ON \
  -DCMAKE_PREFIX_PATH="$(brew --prefix sqlite)"
cmake --build build/macos-gx --target petari -j 8
```

The first build compiles the whole game and takes a while. To run the native
test suite as well (no game data needed for most tests):

```sh
cmake --build build/macos-gx -j 8
cmake --build build/macos-gx --target petari_tests -j 8
ctest --test-dir build/macos-gx -j 4
```

(The app and some test executables are not part of the default `all` target;
`--target petari` and `--target petari_tests` build them.)

Tests labelled `timing` check real-time behaviour (thread hand-off latency,
audio pacing, the audio thread during shader compiles). They need a quiet,
real Mac: run them alone with `ctest --test-dir build/macos-gx -L timing`, and
skip them on shared or virtual machines with `-LE timing` (CI does).
More build detail, including the sanitizer core-library presets, is in
[native/README.md](native/README.md).

## First launch

Double-click **`Play Petari.command`** in the repository root, or run:

```sh
build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --disc build/game-data/RMGE01
```

- There is no preparation screen: the game starts straight away. The first
  visit to each new area compiles Metal shaders, so it can hitch briefly. The
  shader cache is kept in `~/Library/Caches/Petari`, so later visits are smooth.
- Saves, settings (`controls.txt`, `mods.txt`) and crash reports live in
  `~/Library/Application Support/Petari`. Use `--user DIR` for a separate save
  location. Native saves are not compatible with Wii saves.
- At the title screen press **Return** (the game's "press A and B").
- To go straight into a galaxy after loading your file, add `--stage` and
  `--scenario`, e.g. `./Play\ Petari.command --stage good-egg --scenario 2`.
  Names, missions and rules: [native/LEVEL_SELECT.md](native/LEVEL_SELECT.md).

## Controls

Keyboard and mouse by default, with game controller support. The main keys:
**WASD** move, **Space** jump, **F** spin, **Shift** crouch, mouse aims the Star
Pointer, left click shoots Star Bits, **Q/E** rotate the camera, **Escape**
pauses, and **`** (the key under Escape) or **F1** opens the native Home menu
(Controls, Mods, Restart, Quit). Everything is listed, and remappable, in
[native/CONTROLS.md](native/CONTROLS.md). Controller support follows SDL3's
standard layout but has not yet been tested with physical hardware.

## Saves

Unlocked saves (the observatory restored, so you can jump straight to later
galaxies) are made from your own save file by a script, through the game's own
save code. How to make one, install it, and restore your previous save with a
backup: [native/SAVES.md](native/SAVES.md).

## Mods

Optional gameplay helpers, all **off by default**; turn them on in the Home
menu (**`** or **F1**, then **Mods**):

- **Collect Star Bits** (**G**, or **L3** on a controller): the Star Pointer
  collects every Star Bit currently on screen.
- **Shoot nearest enemy** (**V**): fires a Star Bit at the nearest enemy in view.

Both use the game's own collection and shooting, so counters, sounds and stuns
behave normally. Details: [native/MODS.md](native/MODS.md). With every mod off
the game behaves as the original.

## Known issues

- This is a development build. Automated testing enters every one of the 135
  galaxy missions and checks that each loads and plays (move, jump, spin,
  camera, pause) without a crash, hang or renderer error; every observatory
  dome has been toured from unlocked saves; and full missions have been played
  to the Power Star by hand. Not every mission has been played to completion,
  and the ending sequence has not been played through.
- The first time a Mac runs the game it compiles shaders in the background, so
  the first few minutes can stutter and objects may pop in briefly. After that
  the shader cache in `~/Library/Caches/Petari` makes launches immediate and
  smooth, including with other save files.
- Physical game controllers have not been tested yet.
- Visual fidelity is close but not yet systematically compared with the Wii;
  some effects may differ.
- The original small UI textures are low resolution by design.
- Report bugs in this repository's issues with what you did and the log
  printed in Terminal. Do not report them to the Petari project.

Detailed test coverage: [native/RELIABILITY.md](native/RELIABILITY.md) and
[native/PLAYTEST.md](native/PLAYTEST.md).

## Credits and licenses

- **Petari decompilation**: the [SMGCommunity/Petari](https://github.com/SMGCommunity/Petari)
  contributors. Released under CC0 1.0 ([LICENSE](LICENSE)).
- **This port's own code** (the native port, its tools and documentation) is
  also released under **CC0 1.0**, the same terms as the decompilation: you can
  use it for any purpose without asking. The dependencies below keep their own
  licenses.
- **Aurora** by Luke Street ([encounter/aurora](https://github.com/encounter/aurora)),
  the GX-to-WebGPU/Metal backend: MIT License, Copyright (c) 2022 Luke Street.
- **Dawn** ([dawn.googlesource.com](https://dawn.googlesource.com/dawn)), the
  WebGPU implementation used by Aurora, fetched as a prebuilt package: BSD
  3-Clause License, Copyright 2017-2023 The Dawn & Tint Authors.
- **SDL3** ([libsdl.org](https://www.libsdl.org/)): zlib License,
  Copyright (C) 1997-2026 Sam Lantinga.
- **Dear ImGui** ([ocornut/imgui](https://github.com/ocornut/imgui)): MIT License,
  Copyright (c) 2014-2025 Omar Cornut.
- **Tracy** ([wolfpld/tracy](https://github.com/wolfpld/tracy)): BSD 3-Clause License.
- **xxHash** ([Cyan4973/xxHash](https://github.com/Cyan4973/xxHash)): BSD 2-Clause
  License, Copyright (c) 2012-2021 Yann Collet.
- **SQLite**: public domain.

These dependencies are downloaded at build time, not included in this
repository; their license files come with their sources (Dawn's prebuilt
package does not include one; see the Dawn repository). If you distribute a
built app, include those licenses with it.

## Acknowledgments

Most of this port was written by AI models, working under the direction of the
repository owner ([classicrob](https://github.com/classicrob)), who chose the
goals, played the builds and reviewed the results:

- **Claude**, by Anthropic, used through Claude Code.
- **Astra**, an OpenAI model, used through Codex.

Many of their commits carry `Co-Authored-By` trailers. The port builds on
the work of the Petari decompilation contributors and on Aurora; without them it
would not exist.

Super Mario Galaxy is a trademark of Nintendo. This project is not affiliated
with or endorsed by Nintendo.

---

# About the Petari decompilation

The rest of this file is the upstream Petari README, kept as is. It describes
the decompilation project and its matching Wii build, not this port.

Petari
[![Build Status]][actions] ![Progress] [![Discord Badge]][discord]
=============

[Build Status]: https://github.com/SMGCommunity/Petari/actions/workflows/build.yml/badge.svg
[actions]: https://github.com/SMGCommunity/Petari/actions/workflows/build.yml

[Progress]: https://decomp.dev/SMGCommunity/Petari.svg?mode=shield&measure=code&label=Code

[Discord Badge]: https://img.shields.io/discord/727908905392275526?color=%237289DA&logo=discord&logoColor=%23FFFFFF
[discord]: https://discord.gg/ZxEqyYeZbf
[progress_link]: https://decomp.dev/SMGCommunity/Petari

<!-- markdownlint-disable MD033 -->
[<img src="https://decomp.dev/SMGCommunity/Petari.svg?w=512&h=256" width="512" height="256" alt="A visual">][progress_link]
<!-- markdownlint-enable MD033 -->

A work-in-progress decompilation of Super Mario Galaxy 1.

This repository does **not** contain any game assets or assembly whatsoever. An existing copy of the game is required.

This project is **not** meant to be an effort to create a PC Port. Please do not ask for any information on a PC port on this repository or in the Discord server.

> [!NOTE]
> AI may be used for code cleanup, formatting, documentation, and naming assistance. AI-generated decompilation work is not allowed. Pull requests containing obvious AI-generated decompilation output or other AI slop will be rejected. Contributors should be able to explain and justify any decompilation work they submit. This also applies to all tool-generated code. We want to keep this project as human as possible.

Supported versions:

- `RMGK01`: Rev 0 (Korea)

Dependencies
============

Windows
--------

On Windows, it's **highly recommended** to use native tooling. WSL or msys2 are **not** required.  
When running under WSL, [objdiff](#diffing) is unable to get filesystem notifications for automatic rebuilds.

- Install [Python](https://www.python.org/downloads/) and add it to `%PATH%`.
  - Also available from the [Windows Store](https://apps.microsoft.com/store/detail/python-311/9NRWMJP3717K).
- Download [ninja](https://github.com/ninja-build/ninja/releases) and add it to `%PATH%`.
  - Quick install via pip: `pip install ninja`

macOS
------

- Install [ninja](https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages):

  ```sh
  brew install ninja
  ```

[wibo](https://github.com/decompals/wibo), a minimal 32-bit Windows binary wrapper, will be automatically downloaded and used.

Linux
------

- Install [ninja](https://github.com/ninja-build/ninja/wiki/Pre-built-Ninja-packages).

[wibo](https://github.com/decompals/wibo), a minimal 32-bit Windows binary wrapper, will be automatically downloaded and used.

Building
========

- Clone the repository:

  ```sh
  git clone https://github.com/SMGCommunity/Petari.git
  ```

- Using [Dolphin Emulator](https://dolphin-emu.org/), extract your game to `orig/GAMEID`.
![](assets/dolphin-extract.png)
  - To save space, the only necessary files are the following. Any others can be deleted.
    - `sys/main.dol`
- Configure:

  ```sh
  python configure.py
  ```

  To use a version other than `GAMEID` (USA), specify it with `--version`.
- Build:

  ```sh
  ninja
  ```

Diffing
=======

Once the initial build succeeds, an `objdiff.json` should exist in the project root.

Download the latest release from [encounter/objdiff](https://github.com/encounter/objdiff). Under project settings, set `Project directory`. The configuration should be loaded automatically.

Select an object from the left sidebar to begin diffing. Changes to the project will rebuild automatically: changes to source files, headers, `configure.py`, `splits.txt` or `symbols.txt`.

![](assets/objdiff.png)

Credits
=======
Big thanks to the [doldecomp team](https://github.com/doldecomp/sdk_2009-12-11) for their efforts on bte, [tp](https://github.com/zeldaret/tp) for JSystem, and [ogws](https://github.com/doldecomp/ogws/tree/master), where this repository has sourced code and headers from.

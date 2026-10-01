# Native mods

Optional gameplay helpers. Every mod is **off** unless you turn it on, so an
unmodified game and every test run behave exactly as before.

| Mod (`mods.txt` name) | Button | What it does |
| --- | --- | --- |
| Collect visible Star Bits (`CollectStarBits`) | G, controller L3 (left stick click) | Every Star Bit on screen flies to Mario, as if the pointer had touched each one |
| Fire a Star Bit at the nearest enemy (`ShootEnemy`) | V (no controller default) | Shoots one Star Bit at the nearest enemy on screen |
| Odyssey movement (`OdysseyMovement`) | none (a mode; it uses the normal Jump, Crouch and Spin buttons) | Mario moves with Super Mario Odyssey's physics (normal Mario on foot; swimming and the special suits stay Galaxy's). Run: SMO's speed and acceleration. Jumps: single, double and triple (SMO's chain rules and held-jump arc); long jump (run, Crouch, Jump); backflip (Crouch, Jump); sideflip (run, flick the stick back, Jump); ground-pound jump (Jump 5–30 frames after a ground pound lands); dive (Spin during a ground pound); roll (Crouch, then Spin on the ground; Spin again to boost, Jump for a long jump); wall jump (SMO's arc). Measured in the game, the heights and speeds match SMO's numbers exactly (for example jump 258 / 105, triple 550, backflip and sideflip 496, run 14 u/f); the wall jump is model-tested only. Not included: Cappy, the ground-pound roll and SMO's wall slide. Keyboard: Space jump, Shift crouch, F spin (controller: bottom button, LT, left button/RB). See docs/dev/ODYSSEY_MOVEMENT.md. Switching it mid-air takes effect from the next jump. |

## Turning mods on

- In the game: F1, then **Mods**. Each line shows On or Off; choose it to switch.
  The choice is saved at once in `mods.txt` in the user directory. The same
  page leads to **Level Select** (native/LEVEL_SELECT.md).
- Or edit `mods.txt` (next to `controls.txt`):

  ```
  CollectStarBits=on
  ShootEnemy=off
  ```

- For one run: `PETARI_MODS=CollectStarBits,ShootEnemy` (or `none`) overrides
  the file.

The buttons are remappable in `controls.txt` like any other action:
`ModCollectStarBits=Key:G,Pad:LeftStick` and `ModShootEnemy=Key:V`
(see native/CONTROLS.md for input names). A button press while its mod is
off does nothing and is not remembered.

## Odyssey camera

A player-controlled orbit camera like Super Mario Odyssey's, wherever Galaxy uses
an ordinary follow camera. Cutscenes, boss intros, talk, launch stars, cannons,
2D sections and fixed or rail cameras stay as designed. Turn it on in F1 → Mods
→ Odyssey camera..., which also has speed (1-5) and invert options; they are
saved in `camera.txt` (`PETARI_ODYSSEY_CAMERA=1` for one run). It works with or
without the Odyssey movement mod.

| Input (camera mod on) | Action |
| --- | --- |
| Right stick | orbit; the stick no longer moves the Star Pointer (the mouse still does) |
| J / L, I / K (and Q / E, arrow left / right) | orbit left / right, pitch up / down |
| Hold Command or the middle mouse button and move the mouse | orbit; the pointer stays put |
| Two-finger trackpad or Magic Mouse scroll | orbit |
| Mouse wheel clicks, trackpad pinch, Z / X | zoom in / out |
| C | recentre behind Mario |

All of these are remappable in `controls.txt` (`CameraOrbitHold`,
`CameraOrbitLeft`, `CameraOrbitRight`, `CameraPitchUp`, `CameraPitchDown`,
`CameraZoomIn`, `CameraZoomOut`). `ScrollMode=orbit` or `zoom` in `camera.txt`
makes every scroll orbit or zoom. Design and value sources: docs/dev/ODYSSEY_CAMERA.md.

## How they follow the game's rules

Both mods act only where the game itself would let the pointer collect or
shoot Star Bits: not during cutscenes, not while the Star Pointer is disabled
(for example in the file select's two-player transparency mode), and not while
the game is paused (they run in the Star Bit director's per-frame update,
which stops with the scene).

- **Collect** uses the game's own collection path: for four frames after the
  press, `StarPiece::tryGotJudge` treats every eligible Star Bit as pointed at,
  so `goToPlayer` sends it to Mario with the normal sparkle, sound and counter.
  Eligible means what the pointer could collect (within the game's collection
  distance, past its short delay after appearing) and on screen: not clipped,
  not hidden, inside the camera frustum.
- **Shoot** uses `StarPieceShooter::shoot`'s path: `StarPiece::throwToTarget` from
  the usual shot origin, the usual shot sound, one Star Bit spent. The hit, stun
  and damage are the enemy's normal response to a Star Bit. The target is the
  enemy sensor nearest to Mario that is valid, inside the camera frustum, whose
  actor is not clipped or hidden, and that the camera can see (no map polygon
  between). No target means no shot. With no Star Bits it plays the game's
  "no Star Bits" sound, as a normal shot would.

Diagnostics go to the log as `[mods] ...` lines (what was collected, what was
shot at, what it hit, and "not allowed now" / "no enemy on screen").

## Testing

- `native_input` covers the bindings, press counting, dropped presses while a
  mod is off, and `mods.txt`/`PETARI_MODS` parsing; `native_home_menu` covers the
  Mods page.
- `PETARI_MODS_AUTOPRESS=<frames>` (test driver in `app_main.cpp`) presses the
  collect button every `<frames>` gameplay frames and the shoot button halfway
  between, so background smoke runs can exercise the mods; with the mods off the
  same presses must change nothing.

# Mod folder: replacing or adding disc files

The mod folder lets a mod replace any file of the game disc, or add new ones
(custom stages need new archives), without touching your game copy. Mods are a
folder of files that mirror the disc's `files/` tree. They are **off by default**:
with no mods, or none enabled, the game reads the disc exactly as before.

## Layout

```
<user folder>/mods/                 (also: every directory in PETARI_MOD_DIRS, ":"-separated, for development)
  MyMod/
    mod.txt                         optional
    files/
      StageData/EggStarGalaxy.arc   replaces the disc's /StageData/EggStarGalaxy.arc
      StageData/MyStage/MyStage.arc a new file (the folder MyStage is created)
      LayoutData/...  ObjectData/...  AudioRes/...  (anything below the disc's files/)
```

- The user folder is the one given by `--user` (the folder that holds `NAND/`,
  `controls.txt` and `mods.txt`). If a mod name exists in several of the searched
  folders, the first one found wins (the user folder before `PETARI_MOD_DIRS`).
- `mod.txt` has `name=` (shown in the menu), `description=` and `priority=` (a
  whole number, default 0). Unknown lines are ignored.
- Paths match the way the game matches them: ASCII case-insensitively, `/` or `\`
  separators, leading slashes ignored. `..` is refused. The case of the file's
  path in your mod is kept when it adds a new file.
- Hidden macOS files (`.DS_Store`, `._*`) are ignored.
- A file may be any size: a replacement larger than the original, and added files,
  are placed after the disc's last file in the virtual disc. Files read by the
  game, by streamed audio and by movies all go through the same virtual disc.

## Turning mods on

- In the game: F1, **Mods**, **Mod folder...**. Each mod shows On or Off, three to a
  page. Changes are saved at once in `mods.txt` as `Folder.<ModName>=on|off` and
  apply **the next time the game starts** (the disc is read once at start-up). The
  gameplay mods' lines (`CollectStarBits=on`...) share the file and are not touched.
- Or edit `mods.txt`:

  ```
  Folder.MyMod=on
  ```

- For one run: `PETARI_MOD_FOLDERS=MyMod,OtherMod` (or `none`) overrides the file.

## Precedence and the log

Enabled mods are ordered by `priority` (higher first), then by name. For a file
supplied by several mods the first one in that order wins. At start-up the log
says what happened:

```
[mods] mod MyMod (priority 5, rank 1 of 2): 14 files
[mods] conflict StageData/EggStarGalaxy.arc: MyMod wins over OtherMod
[mods] disc overlay applied: 3 files replaced, 11 added
[mods] game opened /StageData/EggStarGalaxy.arc from mod MyMod (91616 bytes)   <- first open of each mod file
```

A mod file that cannot be used (for example a path where the disc has a directory
in the way) is skipped with a `[mods] skipped <path>: <reason>` line; the rest of the
mod still applies.

## Compressed and uncompressed archives

The game decides by the file's header, as on the console. Stage archives
(`.arc`) can be Yaz0-compressed (header `Yaz0`) or plain (`RARC`); both load.
Whitehole writes the compressed form; either is fine in `files/`. Nothing in the
mod folder is converted.

## Broken files are refused, not loaded

Before a mod file can replace a disc file, its header is checked (a few dozen bytes, at start-up):
archives (`.arc`, `.szs`) need a Yaz0 header with a sane size and, for `.arc`, a RARC or U8 archive after
it, or a RARC or U8 header whose sizes fit the file; `.bmg` needs `MESGbmg1`; `.brstm` needs `RSTM`. Other
files are not checked. A file that fails is skipped and the disc file (or another mod's valid file) is
used, with a log line:

```
[mods] MyMod: StageData/EggStarGalaxy.arc is not a valid archive; using the disc file
```

This only catches obvious damage (a truncated download, the wrong file, a corrupted header); an archive
with a valid header and bad contents can still fail when the game uses it. The check was run against all
2,233 archive, message and stream files of a real disc with no false rejections.

## Make your first mod

1. Pick a file to change. The disc's files are in `build/game-data/RMGE01/files/`
   (your own extracted copy). For example `StageData/EggStarGalaxy.arc` (a stage).
2. Make the folder: `<user folder>/mods/MyFirstMod/files/StageData/`.
3. Put your edited archive there with the same name:
   `<user folder>/mods/MyFirstMod/files/StageData/EggStarGalaxy.arc`. With Whitehole:
   open the galaxy, change something, save, and copy the archive Whitehole wrote
   into the folder above. Your original files are never modified.
4. Optional: `<user folder>/mods/MyFirstMod/mod.txt` containing
   `name=My first mod` and `description=Moves the start point`.
5. Start the game, press F1, **Mods**, **Mod folder...**, switch **My first mod** On,
   quit and start the game again (or start it with `PETARI_MOD_FOLDERS=MyFirstMod`).
6. Check the log for `[mods] disc overlay applied: 1 files replaced, 0 added` and
   `[mods] game opened /StageData/EggStarGalaxy.arc from mod MyFirstMod`.

To undo it, switch the mod off (or delete its folder). Nothing else changed.

## A verification example with no Nintendo data

`native/tools/mod_smoke.py` builds a test mod at run time from your own copy of
the disc and checks it in the real game, without shipping any game data:

```sh
python3 native/tools/mod_smoke.py --app build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --output build/mod-smoke
```

It decompresses `EggStarGalaxy.arc`, appends 4096 zero bytes and raises the header's size field to match (a valid,
larger archive), and installs the result as an uncompressed archive and as a Yaz0 stream. It then
launches the game four times on that stage: mod present but not enabled (the archive must mount at
its disc size), enabled uncompressed, enabled Yaz0 (the archive must mount 4096 bytes larger, the log must
show the overlay and the first open of the mod's file, and the stage must still load and pass), and a fourth with a corrupt archive (refused with the log line above;
the disc archive loads and the stage passes). The size
the game mounted comes from its own `[heap-arc] /StageData/EggStarGalaxy.arc -> file cache, N bytes` line.

(Do not use Mario's start point as the probe: the stage fixture's synthetic entry does not take it from
the archive's start rows. Corrupting the archive's `RARC` magic crashes the game when it loads, which is
the other way to show the game reads the mod's bytes.)

## Tests

- `native_platform_dvd` (`testOverlay`): replacement of a file the same size, smaller
  and larger, other-case paths, new files and directories, conflicts (a file where a
  directory is, and the reverse), byte reads, absolute disc-offset reads, and the
  unmodified mount.
- `native_mod_folder`: header validation (good and corrupt RARC, U8 and Yaz0 archives, BMG, BRSTM; with
  `PETARI_DISC_FILES=<disc>/files` it also checks every real disc archive), path normalization (case, slashes, leading slashes, `..`),
  discovery, `mod.txt`, the states in `mods.txt` (other lines kept), default off,
  precedence and conflict logging, a missing `files/` directory, `PETARI_MOD_FOLDERS`
  and `PETARI_MOD_DIRS`.
- `native_input`: `Folder.` lines are accepted by the gameplay mods parser and kept when
  it saves. `native_home_menu` (`testModFolderPage`): the page, paging, toggles.
- `native/tools/mod_smoke.py`: the live check above.

## Make a level edit

A level edit is a stage archive with changed placements, put in a mod folder. The
game loads it in place of the disc's archive; nothing else is needed.

**With Whitehole** (the usual Galaxy level editor): point it at your extracted disc,
open the galaxy, move or add objects, and save. Whitehole writes the stage
archive (for example `StageData/EggStarGalaxy.arc`, Yaz0-compressed). Copy that
file into `<user folder>/mods/<YourMod>/files/StageData/`, under the same name, and
switch the mod on (see "Make your first mod"). Work on a copy of the disc files:
Whitehole saves in place.

**With `native/tools/stage_edit.py`** (small edits from the command line, and
the tests): it reads an archive (Yaz0 or plain), changes rows in its placement
tables, and writes a new archive. The disc file is only read.

1. See what is in a stage:

   ```sh
   A=build/game-data/RMGE01/files/StageData/EggStarGalaxy.arc
   python3 native/tools/stage_edit.py tables $A
   python3 native/tools/stage_edit.py show $A jmp/placement/common/objinfo
   python3 native/tools/stage_edit.py show $A jmp/start/layera/startinfo
   ```

   Each row is printed as `#index  name  l_id  (x, y, z)`. Tables are per layer
   (`common`, `layera`...): a scenario uses `common` plus its own layers.
   Positions are in the stage's own space. Objects in a zone archive are moved
   by that zone's placement in the galaxy.

2. Make the edit: a ring of coins around Good Egg's first start point, and a
   Launch Star moved up:

   ```sh
   M="<user folder>/mods/CoinRing/files/StageData"
   python3 native/tools/stage_edit.py edit $A "$M/EggStarGalaxy.arc" \
     --new jmp/placement/common/objinfo Coin -2914.6 -12961.3 -15332.0 \
     --new jmp/placement/common/objinfo Coin -3614.6 -12961.3 -15332.0 \
     --move jmp/placement/common/objinfo SuperSpinDriver@0 0 500 0
   ```

   - `--move TABLE ROW DX DY DZ` moves a row.
   - `--set TABLE ROW FIELD=VALUE` sets any field (for example `Obj_arg0=3`, `name=Kuribo`).
   - `--add TABLE ROW X Y Z` copies a row to a new place.
   - `--new TABLE NAME X Y Z` adds a new object with default settings.

   A ROW is `NAME@N` (the Nth row with that name, from 0) or `#N`. New rows get
   an unused `l_id`. The output is Yaz0-compressed unless you pass `--plain`.
   Everything you did not change stays byte for byte as on the disc.

3. Switch the mod on (`Folder.CoinRing=on` in `mods.txt`, or the Mods page) and start
   the game. To check what the game placed, start it with
   `PETARI_PLACEMENT_TRACE=Coin,SuperSpinDriver` (or `all`). The log then lists each
   placed object of those names with its world position:

   ```
   [mods] game opened /StageData/EggStarGalaxy.arc from mod CoinRing (25854 bytes)
   [placement] Coin zone 0 l_id 125 at (-2914.6, -12961.3, -15332.0)
   [placement] SuperSpinDriver zone 0 l_id 4 at (-12216.7, -14924.5, -3022.8)
   ```

A new object must be one the game knows (the names in
`src/Game/NameObj/NameObjFactory.cpp`). Its model and sounds must be loadable in
that stage; common objects such as `Coin`, `StarPiece` and `Kuribo` are. Some
objects need their own settings (`Obj_arg`s, switches, a path) to do anything;
copy a working one with `--add` and change it.

### The live check

`native/tools/level_edit_smoke.py` makes the CoinRing mod above at run time from
your copy of the disc, so no Nintendo data is shipped. It launches Good Egg
mission 1 twice:
- with the mod present but off: no new coins, and the Launch Star where the disc
  has it;
- with it on: the overlay is applied, the game opens the mod's archive, and the
  log shows the 8 coins and the moved Launch Star where the edit put them.

Screens from the idle phase are kept in `<output>/<run>-xfb/`.

```sh
python3 native/tools/level_edit_smoke.py --app build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari \
  --output build/level-edit-smoke
```

`native_stage_edit` (ctest) covers the tool without Nintendo data: Yaz0 round trips,
an unedited rebuild identical to its source, moves, field sets, added and new rows,
unique `l_id`s, and errors. With an extracted disc in `build/game-data`, every stage
archive must also rebuild byte for byte when unedited.

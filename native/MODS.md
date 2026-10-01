# Native mods

Optional gameplay helpers. Every mod is **off** unless you turn it on, so an
unmodified game and every test run behave exactly as before.

| Mod (`mods.txt` name) | Button | What it does |
| --- | --- | --- |
| Collect visible Star Bits (`CollectStarBits`) | G, controller L3 (left stick click) | Every Star Bit on screen flies to Mario, as if the pointer had touched each one |
| Fire a Star Bit at the nearest enemy (`ShootEnemy`) | V (no controller default) | Shoots one Star Bit at the nearest enemy on screen |
| Odyssey movement (`OdysseyMovement`) | none (a mode) | Mario moves with Super Mario Odyssey's physics. In progress: the single, double and triple jump (heights, held-jump arc, SMO's chain rules, air control) so far; see docs/dev/ODYSSEY_MOVEMENT.md. A change takes effect from the next jump. |

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
mod folder is converted or checked: a broken archive fails the same way a broken
disc file would.

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

It decompresses `EggStarGalaxy.arc`, moves the layer A start point by 200 units in
place (one float in the archive's start-point table), and installs the result as an
uncompressed archive and as a Yaz0 stream. It then launches the game three times
on that stage: mod present but not enabled (Mario's start must not change),
enabled uncompressed, enabled Yaz0 (Mario's start must move by about 200). Each
run must log the overlay and the first open of the mod's file.

## Tests

- `native_platform_dvd` (`testOverlay`): replacement of a file the same size, smaller
  and larger, other-case paths, new files and directories, conflicts (a file where a
  directory is, and the reverse), byte reads, absolute disc-offset reads, and the
  unmodified mount.
- `native_mod_folder`: path normalization (case, slashes, leading slashes, `..`),
  discovery, `mod.txt`, the states in `mods.txt` (other lines kept), default off,
  precedence and conflict logging, a missing `files/` directory, `PETARI_MOD_FOLDERS`
  and `PETARI_MOD_DIRS`.
- `native_input`: `Folder.` lines are accepted by the gameplay mods parser and kept when
  it saves. `native_home_menu` (`testModFolderPage`): the page, paging, toggles.
- `native/tools/mod_smoke.py`: the live check above.

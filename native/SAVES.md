# Unlocked saves

Ready-made saves unlock the observatory destinations. Choose `grand-finale`
for all content including Grand Finale and an already-completed Luigi file.
They are for playing and testing. Each save is in its own directory under
`build/saves/`. Your own save in `~/Library/Application Support/Petari` is never
read or changed.

| Directory | File 1 contains | Use it for |
|---|---|---|
| `build/saves/all-missions` | Mario with 120 stars. All six domes and the regular missions are open; Grand Finale remains locked. The observatory is fully restored. | Replaying the regular game as Mario. |
| `build/saves/complete-luigi` | The same, plus the 120-star ending counted as seen, so Luigi is playable. Luigi's own game starts from the beginning, as in the real game. | Playing as Luigi. |
| `build/saves/grand-finale` | Mario and Luigi each with 120 stars and both endings seen, so the Grand Finale Galaxy is open. | All content, including Grand Finale and unlocked Luigi levels. |

## Why the stars are collected

The game cannot open a galaxy without collecting stars. Every dome, galaxy,
comet and Hungry Luma galaxy opens because of collected stars or Grand Stars.
Mission select also depends on stars. It shows the missions you have finished
plus the next one. Comet, purple-coin and hidden missions appear in the list only
after their star is collected. The only way to open everything with no stars
would be to change the game's rules, so these saves hold the stars. A mission
whose star you already have can still be played in full. It gives a see-through
star at the end.

In every variant, the Grand Finale's own star is left uncollected. Tutorial and
first power-up explanations are still unseen. Records such as best race times
and coin scores are empty.

## Playing one

Use the directory directly. This leaves it unchanged except for your own
progress:

```sh
build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --user build/saves/grand-finale
```

Choose file 1 on the file select screen. The game saves progress to this
directory. It also keeps its settings and shader caches there, so the first
launch may prepare shaders for longer than your usual directory does. To start
over, make the save again (see below). You can also copy the directory before
playing and use the copy.

## Install as your normal save (about one minute)

**Quit Petari first**. Run these commands
from this repository's root in Terminal. Choose the variant on the first line.
These are instructions for you to run; the test tools never install a save into
your normal directory.

```sh
(
set -e
variant=grand-finale
save_relative=NAND/title/00010000/524d4745/data/GameData.bin
petari_user="$HOME/Library/Application Support/Petari"
petari_backup=$(mktemp -d "$HOME/Desktop/Petari-save-backup.XXXXXX")
if [ -d "$petari_user/NAND" ]; then
  ditto "$petari_user/NAND" "$petari_backup/NAND"
fi
mkdir -p "$petari_user/NAND/title/00010000/524d4745/data"
cp "build/saves/$variant/$save_relative" "$petari_user/$save_relative"
printf 'Keep this backup: %s\n' "$petari_backup"
)
```

**This replaces the entire six-slot save container**, not just file 1. Settings
and shader caches are retained. Each published variant changes file 1; the
other five slots come from the test seed, not your existing save. Keep the printed
backup directory. Launch Petari normally and select file 1.

### Restore your previous save

Quit Petari, replace the example backup path
below with the printed path, and copy back the original container:

```sh
cp "$HOME/Desktop/Petari-save-backup.EXAMPLE/NAND/title/00010000/524d4745/data/GameData.bin" \
   "$HOME/Library/Application Support/Petari/NAND/title/00010000/524d4745/data/GameData.bin"
```

For Luigi with every level already unlocked, use `grand-finale` and switch to
Luigi on file 1's character selection. `complete-luigi` unlocks the character,
but his separate adventure has no collected stars yet. Grand Finale is reached
through its observatory NPC, rather than one of the six dome maps. Hungry Luma
and other observatory destinations also use their own entrances.

## Making a save again

`native/tools/make_unlocked_save.py` makes these saves:

```sh
python3 native/tools/make_unlocked_save.py --variant all-missions   # writes build/saves/all-missions
```

It refuses to overwrite an existing directory, so pass `--output` with a new
path, or remove the old directory first. It needs a built app and a seed user
directory whose file 1 exists. The default seed is `build/observatory-user-2`,
which is only read. File 1 keeps the seed's icon. Other files are copied from
the seed unchanged. If several automated runs share one checkout, run them one
at a time.

In a fresh clone `build/saves/` and `build/observatory-user-2` do not exist
(they are local, untracked files). Make your own seed first: launch the game with
`--user build/my-seed`, start a new game in file 1 and let it save, quit, then

```sh
python3 native/tools/make_unlocked_save.py --variant grand-finale --seed build/my-seed
```

The save is made by the game's own code, not by editing bytes directly:

1. `Petari --make-unlocked-save VARIANT` accepts only an explicit `--user`
   directory marked with `.petari-make-unlocked-save`. It never accepts the
   normal save directory.
2. At boot, the game loads the seed's file with its usual loader. File 1 is
   rebuilt using the game's progression functions (`GameDataHolder`):
   - every star except the Grand Finale's
   - galaxies already visited
   - the observatory Hungry Lumas fully fed
   - the final story event reached
   - the library open and the storybook read
   - Luigi's hiding events finished
   - the observatory's two scene-revealed warp pods already revealed
     (`WarpPodSaveBits` 0 and 1; no other stage uses these bits)
   - every galaxy-opening scene and one-time conversation counted as seen

   All other unlocks come from these through the game's own rules. The ending
   records are stored in the file's config data.
3. The game's save routine writes `GameData.bin`, including its checksum. The
   game then reads it back through the same path it uses at startup. That
   includes the version, size and checksum checks, plus both corruption flags.
   The reloaded data must match the stored bytes exactly. The count must be 120
   stars and all 7 Grand Stars. No galaxy may still be waiting to open. Every
   dome's galaxies must be open, except the Grand Finale when the variant does
   not allow it. The app prints `PETARI UNLOCKED SAVE: VERIFIED` and exits with
   status 0, or prints each failure and exits with status 1.
4. The script checks the file format again with a separate parser (header,
   checksum and chunk layout, and 120 stars). Only then does it publish the
   `NAND/` folder, `generation.log` and `unlocked-save.json`, which contains the
   game's report and the file hashes.

`native/tests/unlocked_save_tests.py` (ctest `native_unlocked_save`) tests the
format checker and the isolation rules. It also re-checks every published save
against its manifest.

## Verification and limits

The three saves pass the game's own generation and read-back checks and an
independent file-format and manifest regression (the `native_unlocked_save`
test). That establishes the saved progression, not a
completed playthrough or that every mission runs without bugs.

Live checks, each with an automated dome tour (`native/tools/dome_tour.py`:
a normal file load, the walk to the dome through the real observatory, the dome
map, the mission menu, entering mission 1 of every galaxy on the map, and the
return through the pause menu), on isolated copies of the saves:

| Save | Domes checked | Result |
|---|---|---|
| `all-missions` | Terrace, Fountain, Kitchen, Bedroom, Engine Room, Garden | 32 galaxy visits, all PASS; full maps and the exact mission IDs in every menu (for example Battle Rock and Dusty Dune 1–7, Buoy Base 1–2); no missing layout or sound references, crashes or hangs. Good Egg missions 4–6 were also entered. |
| `grand-finale` | all six domes, as Mario | 29 visits, all PASS, same checks. |
| `complete-luigi` | all six domes, as Mario | all visits PASS, same checks. |
| `grand-finale` | Terrace, as Luigi | 5/5 PASS; loading the file as Luigi passes the movement and pause checks. |
| `complete-luigi` | Luigi's own new game | Loading as Luigi starts at the Gateway with no letter prologue, as on the Wii (`StorySequenceExecutor`: `!isDataMario()`); after its opening demo (advanced with A) Luigi is playable: jump, movement both ways, pause and resume pass. |

Notes:

- The Engine Room is reached by a route a person recorded (wall kicks up a
  chimney, a star that catches Mario at a ledge, a spin launch); see
  `native/ROUTE_RECORDING.md`. Three consecutive tours passed with the wall-kick
  climb succeeding first time.
- The Grand Finale galaxy route is not automated yet: the Launch Star works, but
  the walk on its small planet to the Grand Finale Luma does not reach it. This
  is a test-driver limitation, not a save or game fault.
- Movement in each visited galaxy is reported, not asserted as mission
  completion (a few galaxies recorded only a short movement distance).

# Unlocked saves

Ready-made saves let you open any dome, galaxy and mission from the observatory.
They are for playing and testing. Each save is in its own directory under
`build/saves/`. Your own save in `~/Library/Application Support/Petari` is never
read or changed.

| Directory | File 1 contains | Use it for |
|---|---|---|
| `build/saves/all-missions` | Mario with 120 stars. Every dome, galaxy and mission is open, and the observatory is fully restored. | Playing any level. **Start here.** |
| `build/saves/complete-luigi` | The same, plus the 120-star ending counted as seen, so Luigi is playable. Luigi's own game starts from the beginning, as in the real game. | Playing as Luigi. |
| `build/saves/grand-finale` | Mario and Luigi each with 120 stars and both endings seen, so the Grand Finale Galaxy is open. | The Grand Finale Galaxy. |

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
build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --user build/saves/all-missions
```

Choose file 1 on the file select screen. The game saves progress to this
directory. It also keeps its settings and shader caches there, so the first
launch may prepare shaders for longer than your usual directory does. To start
over, make the save again (see below). You can also copy the directory before
playing and use the copy.

To use it as your normal save instead, first back up your own save. Then copy the
file over:

```sh
cp -R "$HOME/Library/Application Support/Petari/NAND" ~/Desktop/Petari-NAND-backup
cp build/saves/all-missions/NAND/title/00010000/524d4745/data/GameData.bin \
   "$HOME/Library/Application Support/Petari/NAND/title/00010000/524d4745/data/GameData.bin"
```

This replaces all six files in your normal save. To go back, copy the backup
over it.

## Making a save again

`native/tools/make_unlocked_save.py` makes these saves:

```sh
python3 native/tools/make_unlocked_save.py --variant all-missions   # writes build/saves/all-missions
```

It refuses to overwrite an existing directory, so pass `--output` with a new
path, or remove the old directory first. It needs a built app and a seed user
directory whose file 1 exists. The default seed is `build/observatory-user-2`,
which is only read. File 1 keeps the seed's icon. Other files are copied from
the seed unchanged. In the shared development checkout, run the whole command
under the app lock (`build/locked-app.sh NAME python3 ...`).

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

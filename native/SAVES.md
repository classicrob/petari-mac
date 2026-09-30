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

The three published saves pass the game's generation/read-back checks and the
independent file-format/manifest regression. This establishes saved progression,
not a completed playthrough or that every mission runs without bugs.

Recovered live evidence: `build/unlocked-save/tour6/dome1/dome-tour.json` and
`dome1.log` record a normal file load, observatory walk, Terrace map, all five
Terrace galaxies at mission 1, and Good Egg missions 4, 5 and 6: **8/8 visits
PASS**, exit 0, zero missing layout/sound references and no retained crash/hang
reports. This used the frozen `Petari6.app`, not the latest renderer build.

Earlier Fountain attempts are retained as failures: `tour9/dome2` got stuck
before the dome; `resume-11/dome2` crashed in the smoke observer at warp-pod
entry. The route planner now rejects drops through intervening floors. The
observer no longer assumes every rush state has a bound actor: ordinary warp
status legitimately has no rush sensor in the unchanged decompilation. Its
source audit and 13 sanitizer-backed regression checks are documented in
`build/unlocked-save/resume-12/OBSERVER-AUDIT.md`. The successful run below
exercised both fixes.

**Fountain live retry passed:** `build/unlocked-save/resume-14/dome2/` contains
`dome2.log`, `dome-tour.json`, the per-galaxy `dome-tour.md`, launch hashes and
settings. Exit 0 in 272 seconds; **5/5 visits PASS**, all five expected galaxies
selectable, zero missing references, no retained crash/hang reports. Battle Rock,
Hurry-Scurry, Bowser's Star Reactor, Space Junk and Rolling Green each loaded
mission 1 through the normal UI, became ready, recorded movement, and returned
through the pause menu. The mission menus showed Battle Rock 1–7, Space Junk
1–6 and one star for each other galaxy. The run used stage-only shader
preparation in a background window; it is functional evidence, not a timing or
visual-fidelity benchmark. The Fountain route is now updated in the source
table.

**Bedroom live tour passed:** `build/unlocked-save/resume-14/dome4/` records
exit 0 and **5/5 visits PASS**: Gusty Garden, Honeyclimb, Freezeflame, Bowser's
Dark Matter Plant and Dusty Dune, each at mission 1, with all five expected map
destinations selectable and zero missing references or retained crash/hang
reports. Freezeflame moved 27 units and logged the driver's little-movement
warning; other visits moved 51–60 units. Movement distance is reported, not a
mission-completion assertion. The menus showed Gusty Garden and Freezeflame
1–6, Dusty Dune 1–7, and the other two galaxies 1.

`build/unlocked-save/resume-15/verified-menu-audit.json` independently compares
the Fountain and Bedroom menu logs with every expected scenario ID: no
mismatches. The runner now rejects missing or incorrect mission menus.

Kitchen's `resume-14/dome3` attempt failed near its entrance before any galaxy
visit; its successful retry is recorded below. Engine Room and variant-specific
Luigi/Grand Finale checks remain pending.

**Garden live tour passed:** `build/unlocked-save/resume-14/dome6/` records
**4/4 visits PASS**, the complete four-galaxy map and every expected mission ID,
zero missing references, and no retained crash/hang reports. Dreadnought,
Melty Molten and Deep Dark each showed missions 1–6; Matter Splatter showed 1.
All entered mission 1 and returned through the pause menu. Matter Splatter's
movement check recorded only 2.4 units (warning), so this does not establish
unobstructed movement there. The other visits recorded 44–66 units.

Engine Room's `resume-14/dome5` approach failed before reaching the map after
an airborne route overshoot. The Kitchen retry is recorded below; Engine Room remains pending.

**Kitchen retry passed:** `build/unlocked-save/resume-15/kitchen/` records
**5/5 visits PASS**, all five expected map destinations and all expected
mission IDs, zero missing references and no retained crash/hang reports.
Bubble Breeze, Beach Bowl, Bowser Jr.'s Airship Armada, Buoy Base and Ghostly
Galaxy each loaded mission 1, became ready and returned via pause. Movement
was 51–75 units. Beach Bowl and Ghostly showed 1–6, Buoy Base 1–2, the others 1.
The verified route follows the outer lower ledge past the pillar and jumps up
near the doorway; it is now in the source route table.

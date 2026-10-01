# Record a foreground route

Run from the repository root after quitting any previous Petari instance.
This command uses your normal save directory; it does not install or replace a save.
The recorder itself only writes the CSV, while normal gameplay can save progress.

```sh
env -u PETARI_SMOKE -u PETARI_DOME_ROUTE PETARI_SMOKE_BACKGROUND=0 \
  PETARI_ROUTE_RECORD="$PWD/build/route-$(date +%Y%m%d-%H%M%S).csv" \
  build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --disc build/game-data/RMGE01
```

Load a file and play the stretch you want to record (for example the walk from
the observatory start to a dome), then quit.
The terminal prints the recording path. Use a fresh launch for each route.
Recording is off unless `PETARI_ROUTE_RECORD` is set, refuses existing files,
and is disabled in background mode. Parent directories must already exist.

Convert the recording (replace the timestamp):

```sh
python3 native/tools/recorded_route.py \
  build/route-TIMESTAMP.csv build/route-waypoints.csv
```

The converter selects one continuous AstroGalaxy visit. It rejects recordings
with multiple visits instead of joining unrelated journeys. `--stage NAME`
selects another stage. The output uses the dome route CSV format, with `Walk`
points at landings and direction changes, `Hop` for a recorded jump without an
automatic spin, `Kick` for a jump pressed in the air (a wall kick, at the wall
contact), `Launch` for a bind the player spun out of (a Launch or Sling Star,
at the point where it caught Mario), `Warp` for a bind that carried Mario at
least 800 units without a spin, and separate `Spin` points. The driver presses A
for a `Kick` when Mario clings to the wall within 200 units of the recorded
contact, and retries from the chain's `Hop` (at most three times) if he lands
first. Conversion counts game frames, not wall-clock time, so a recording made
at a low frame rate converts the same way. It refuses existing output files.
This is a candidate route, not an exact frame-by-frame input replay. Validate
it with `PETARI_DOME_ROUTE` and a bounded dome tour before promoting it.

The CSV records every game-thread frame: stage/scenario, position, gravity and
normalized up, grounded/bound flags, remapped move vector and jump/spin/crouch/
camera actions, camera Z axis and world-Y camera yaw in radians. `valid=0`
marks loading or missing-player frames. `zone_ground_id` is the collision
polygon's placement-zone ID, potentially the last contact while airborne;
`-1` means unknown. It is not a spatial zone-volume lookup. World-Y yaw is
ambiguous at vertical poles, so the full camera axis and gravity are retained.

Writes are buffered and flushed every 60 frames and at normal exit. An abrupt
termination can lose the final buffered frames. Data is read under the game
thread's ownership with a host-allocation scope and the observer's scene/core
validity guards. Recording never changes game state or emits input.

Covered by the native_route_record tests. The CSV writer is exercised under UBSan, including background rejection,
escaping and no-overwrite behavior; conversion tests cover jump/spin/landing
markers and multiple-stage-visit rejection. Full human-route recording and
replay remain to be verified with an actual recording.

## Replay a recorded mission as a regression test

Every playtest recording can become a test. Split it into one route per stage
visit (FileSelect is skipped; each segment starts once Mario first stands in
the stage, so the arrival demo is left to the game):

```sh
cd native/tools && python3 recording_segments.py ../../build/playtest-recordings/NAME.csv ../../build/replays/NAME
```

The output directory gets `NN-<stage>-s<scenario>.csv` routes and a
`manifest.json` (frames, start and end, action counts, and `star`: `yes` when
the recording's `power_stars` column rose, `unknown` for recordings made before
that column existed). Short same-stage gaps (up to `--join` frames, default 120)
are joined. Launches include a spin pressed up to six frames before the bind.
A teleport or launch back to where the route already went (a death's respawn,
a return to a hub after a failed loop) keeps only the final attempt;
`--keep-loops` keeps everything.

Replay a star segment in a stage fixture:

```sh
python3 native/tools/create_observatory_fixture.py --source build/observatory-user-2 --output build/<dir> --kind stage
PETARI_SMOKE=replay PETARI_STAGE=EggStarGalaxy PETARI_SCENARIO=2 \
  PETARI_REPLAY_ROUTE=$PWD/build/replays/NAME/02-EggStarGalaxy-s2.csv \
  build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari \
  --disc build/game-data/RMGE01 --user build/<dir> --test-fixture stage
```

The replay (native/app/smoke_replay.hpp) steers with the game's own stick
mapping, answers talk pages, notices and prompts as a player would (yes/no gets
Yes), and resumes from the nearest route point after a death. It PASSes only
when the game reports `PowerStar.Get` (or `GrandStar.Get`) and the return then
reaches a playable observatory. Save prompts are answered Yes (the fixture is a
copy). Route replays follow positions, not exact inputs: timing-sensitive
hazards can still differ from the recording.

Status (2026-09-30): the replay is unit-tested (route parsing, stage/scenario
entry, walking, hops, launches, deaths, prompts, star and return rules) and was
run live on a mission-2 recording of Good Egg: it entered after the arrival,
crossed the Disk Garden (including its underside) without damage, held the
Luma talks and fought enemies with spins, and reached route point 334 of 2454,
then failed at the first Launch Star (a wall-kick shortcut left Mario on a ledge
above it). No recorded mission has yet replayed to the star.

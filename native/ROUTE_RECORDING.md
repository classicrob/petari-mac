# Record a foreground route

Run from the repository root after quitting any previous Petari instance.
This command uses your playtest copy; it does not install or replace a save.
The recorder itself only writes the CSV, while normal gameplay can save progress.

```sh
env -u PETARI_SMOKE -u PETARI_DOME_ROUTE PETARI_SMOKE_BACKGROUND=0 \
  PETARI_ROUTE_RECORD="$PWD/build/engine-room-$(date +%Y%m%d-%H%M%S).csv" \
  build/locked-app.sh unlocked-save \
  build/unlocked-save/recorder/Petari.app/Contents/MacOS/Petari \
  --user "$HOME/Petari-playtest/all-missions"
```

Select file 1, walk to the Engine Room, and quit once you enter the dome.
The terminal prints the recording path. Use a fresh launch for each route.
Recording is off unless `PETARI_ROUTE_RECORD` is set, refuses existing files,
and is disabled in background mode. Parent directories must already exist.
The app lock serializes this with automated tests; it may wait for their slot.

Convert the recording (replace the timestamp):

```sh
python3 native/tools/recorded_route.py \
  build/engine-room-TIMESTAMP.csv build/engine-room-waypoints.csv
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

Build and five targeted CTests passed in `build/unlocked-save/recorder/`.
The CSV writer is exercised under UBSan, including background rejection,
escaping and no-overwrite behavior; conversion tests cover jump/spin/landing
markers and multiple-stage-visit rejection. Full human-route recording and
replay remain to be verified with an actual recording.

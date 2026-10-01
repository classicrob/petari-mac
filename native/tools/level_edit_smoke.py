#!/usr/bin/env python3
"""Live check that an edited stage loads from a mod (native/MODS.md "Make a level edit").

Builds a level-edit mod at run time from your own copy of the disc (no Nintendo data
is stored), with native/tools/stage_edit.py: EggStarGalaxy.arc is decompressed, a ring
of 8 new Coins is added around Mario's mission-1 start and a Launch Star
(SuperSpinDriver, l_id 4) is moved 500 units up, and the archive is rebuilt and
Yaz0-compressed. Two launches of the stage smoke on EggStarGalaxy scenario 1, each in
its own fixture user directory, with PETARI_PLACEMENT_TRACE=Coin,SuperSpinDriver:

  1. mod present, not enabled  -> no overlay; no new Coins; the Launch Star where the disc has it
  2. mod enabled               -> overlay applied, the mod's archive opened; the 8 Coins and the
                                  moved Launch Star placed where the edit put them

Screens from the idle phase are dumped (XFB) for a look at the ring. One app at a time
through build/locked-app.sh.

usage: level_edit_smoke.py --app Petari.app/Contents/MacOS/Petari --output build/level-edit-smoke [--owner leveledit]
"""
import argparse
import math
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_edit  # noqa: E402
import stage_sweep as sweep  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
STAGE, SCENARIO = "EggStarGalaxy", 1
START_TABLE = "jmp/start/layera/startinfo"  # scenario 1's start (layer A)
OBJ_TABLE = "jmp/placement/common/objinfo"
MOVED_LID, LIFT = 4, 500.0
RING, RADIUS, HEIGHT = 8, 350.0, 120.0


def up_vector(dir_deg):
    """The start's Y axis (MR::makeMtxTR's rotation)."""
    rx, ry, rz = (math.radians(v) for v in dir_deg)
    sx, sy, sz, cx, cy, cz = math.sin(rx), math.sin(ry), math.sin(rz), math.cos(rx), math.cos(ry), math.cos(rz)
    return (cz * sy * sx - sz * cx, sz * sy * sx + cz * cx, cy * sx)


def build_mod(disc):
    """The edited archive and what the game must place: ([coin positions], launch star (before, after))."""
    edit = stage_edit.StageEdit((disc / "files/StageData" / f"{STAGE}.arc").read_bytes())
    start = edit.table(START_TABLE)
    origin = [start.get(0, f"pos_{a}") for a in "xyz"]
    up = up_vector([start.get(0, f"dir_{a}") for a in "xyz"])
    # Two unit vectors across the start's up.
    side = (up[1], -up[0], 0.0) if abs(up[2]) < 0.9 else (0.0, up[2], -up[1])
    norm = math.sqrt(sum(v * v for v in side))
    side = tuple(v / norm for v in side)
    other = (up[1] * side[2] - up[2] * side[1], up[2] * side[0] - up[0] * side[2], up[0] * side[1] - up[1] * side[0])
    coins = []
    for i in range(RING):
        angle = 2 * math.pi * i / RING
        point = [origin[k] + up[k] * HEIGHT + RADIUS * (math.cos(angle) * side[k] + math.sin(angle) * other[k]) for k in range(3)]
        edit.new(OBJ_TABLE, "Coin", *point)
        coins.append(point)
    objects = edit.table(OBJ_TABLE)
    row = next(r for r in range(len(objects.rows)) if objects.get(r, "l_id") == MOVED_LID)
    before = [objects.get(row, f"pos_{a}") for a in "xyz"]
    edit.move(OBJ_TABLE, f"#{row}", 0.0, LIFT, 0.0)
    after = [objects.get(row, f"pos_{a}") for a in "xyz"]
    return edit.build(compress=True), coins, (before, after)


def placements(text, name):
    return [(int(lid), tuple(float(v) for v in pos)) for zone, lid, *pos in
            re.findall(rf"\[placement\] {name} zone (\d+) l_id (-?\d+) at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text) if zone == "0"]


def near(a, b, tolerance=1.0):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


def run(args, name, user):
    log = args.output / f"{name}.log"
    command, env_extra = sweep.app_command(args, user, STAGE, SCENARIO)
    env = dict(os.environ, **env_extra)
    env.update(PETARI_LOCK_CLASS="functional", PETARI_PLACEMENT_TRACE="Coin,SuperSpinDriver",
               PETARI_XFB_DUMP=str((args.output / f"{name}-xfb").resolve()), PETARI_XFB_DUMP_LABELS="idle",
               PETARI_XFB_DUMP_EVERY="60", PETARI_XFB_DUMP_SPAN="240")
    full = [str(ROOT / "build/locked-app.sh"), args.owner, "/usr/bin/perl", "-e", "alarm shift; exec @ARGV or die",
            str(args.timeout)] + command
    with log.open("wb") as stream:
        status = subprocess.run(full, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT).returncode
    text = log.read_text(errors="replace")
    return {
        "name": name, "status": status,
        "ready": re.search(r"stage ready: frame \d+, Mario at", text) is not None,
        "overlay": re.search(r"\[mods\] disc overlay applied: (\d+) files replaced, (\d+) added", text),
        "opened": re.findall(r"\[mods\] game opened (\S+) from mod (\S+) \((\d+) bytes\)", text),
        "coins": placements(text, "Coin"),
        "drivers": placements(text, "SuperSpinDriver"),
        "result": (re.findall(r"PETARI SMOKE RESULT: (\w+)", text) or [None])[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--disc", type=Path, default=ROOT / "build/game-data/RMGE01")
    parser.add_argument("--source", type=Path, default=ROOT / "build/observatory-user-2")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--owner", default="leveledit")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--frames", type=int, default=12000)
    parser.add_argument("--idle-frames", type=int, default=300)
    args = parser.parse_args()
    args.pipeline_policy = "default"  # functional check; the app's own pipeline policy
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = sweep.load_fixture_module()
    archive, coins, (driver_before, driver_after) = build_mod(args.disc)

    results = []
    for name, enabled in (("1-present-not-enabled", False), ("2-enabled", True)):
        user = args.output / f"{name}-user"
        shutil.rmtree(user, ignore_errors=True)
        shutil.rmtree(args.output / f"{name}-xfb", ignore_errors=True)
        fixture.create(args.source, user, "stage")
        mod = user / "mods/CoinRing"
        (mod / "files/StageData").mkdir(parents=True)
        (mod / "mod.txt").write_text("name=Coin ring\ndescription=Good Egg: coins around the start, a Launch Star moved up\n")
        (mod / "files/StageData" / f"{STAGE}.arc").write_bytes(archive)
        if enabled:
            (user / "mods.txt").write_text("Folder.CoinRing=on\n")
        results.append(run(args, name, user))

    base, edited = results
    failures = []
    for r in results:
        if not r["ready"] or r["result"] != "PASS":
            failures.append(f"{r['name']}: stage smoke {r['result']}, ready {r['ready']}")
    if base["overlay"] or base["opened"]:
        failures.append("run 1: a mod that is present but not enabled must change nothing")
    if any(near(pos, coin) for _, pos in base["coins"] for coin in coins):
        failures.append("run 1: a new Coin was placed without the mod")
    if not any(near(pos, driver_before) for _, pos in base["drivers"]):
        failures.append(f"run 1: no Launch Star at the disc's {driver_before}")
    if not edited["overlay"] or edited["overlay"].groups() != ("1", "0"):
        failures.append("run 2: expected 'disc overlay applied: 1 files replaced, 0 added'")
    if not any(p == f"/StageData/{STAGE}.arc" and m == "CoinRing" for p, m, _ in edited["opened"]):
        failures.append(f"run 2: the game never opened the mod's {STAGE}.arc")
    missing = [coin for coin in coins if not any(near(pos, coin) for _, pos in edited["coins"])]
    if missing:
        failures.append(f"run 2: {len(missing)} of {len(coins)} new Coins not placed, e.g. {missing[0]}")
    if not any(lid == MOVED_LID and near(pos, driver_after) for lid, pos in edited["drivers"]):
        failures.append(f"run 2: Launch Star l_id {MOVED_LID} not at the edited {driver_after}")
    if any(lid == MOVED_LID and near(pos, driver_before) for lid, pos in edited["drivers"]):
        failures.append(f"run 2: Launch Star l_id {MOVED_LID} still at the disc's {driver_before}")
    for r in results:
        print(f"{r['name']}: exit {r['status']} result {r['result']} overlay {r['overlay'].groups() if r['overlay'] else None} "
              f"opened {r['opened']} coins {len(r['coins'])} launch stars {len(r['drivers'])}")
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        return 1
    print(f"PASS: an edited {STAGE}.arc (rebuilt, Yaz0) loaded from the mod folder: {len(coins)} added Coins and the "
          f"Launch Star moved to {[round(v, 1) for v in driver_after]} were placed; without the mod enabled, neither")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

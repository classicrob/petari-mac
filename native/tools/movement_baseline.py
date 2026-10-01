#!/usr/bin/env python3
"""Movement baseline: Mario's measured jumps and run against recorded values.

Runs the movement harness (PETARI_SMOKE=movement, native/app/smoke_movement.hpp)
on a fresh observatory fixture and compares its MOVEMENT lines with:
  - off: Galaxy's own movement (no mods). Any difference means a change meant
    for a mod leaked into the unmodified game.
  - on:  the OdysseyMovement mod (SMO's numbers, docs/dev/ODYSSEY_MOVEMENT.md).

  movement_baseline.py --app build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari --output build/mv-baseline
  movement_baseline.py ... --mods off      # only the unmodified game

Launch it through the app lock in shared checkouts. Exit status 0 when every
value matches, 1 otherwise. Only moves whose result does not depend on the
run-up are compared (the long jump, dive and flips after a run vary with where
the run ends).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

ONLY = "standstill,ground-pound jump,running jump 1,chain jump,backflip"

# name -> (field, expected, tolerance); "apex" is the height, "speed" the run's top speed.
EXPECTED = {
    "off": {
        "standstill held jump": ("apex", 260.0, 0.5),
        "standstill tap jump": ("apex", 156.4, 0.5),
        "ground-pound jump": ("apex", 260.0, 0.5),  # Galaxy has none: a normal jump
        "run": ("speed", 11.968, 0.1),
        "running jump 1": ("apex", 260.0, 0.5),
        "chain jump 2": ("apex", 345.2, 0.5),
        "chain jump 3": ("apex", 738.0, 0.5),
        "backflip": ("apex", 585.0, 0.5),
    },
    "on": {
        "standstill held jump": ("apex", 258.0, 0.5),
        "standstill tap jump": ("apex", 105.0, 0.5),
        "ground-pound jump": ("apex", 513.5, 0.5),
        "run": ("speed", 14.0, 0.1),
        "running jump 1": ("apex", 312.0, 0.5),
        "chain jump 2": ("apex", 346.5, 0.5),
        "chain jump 3": ("apex", 550.0, 0.5),
        "backflip": ("apex", 496.0, 0.5),
    },
}

JUMP = re.compile(r"MOVEMENT (.+?): apex (-?[\d.]+) after")
RUN = re.compile(r"MOVEMENT run: max speed (-?[\d.]+) u/f")


def parse(text):
    """MOVEMENT lines -> {name: {"apex"|"speed": value}} (the first of each)."""
    found = {}
    for line in text.splitlines():
        if (m := RUN.search(line)):
            found.setdefault("run", {"speed": float(m.group(1))})
        elif (m := JUMP.search(line)):
            found.setdefault(m.group(1), {"apex": float(m.group(2))})
    return found


def compare(found, expected):
    """Problems as text; empty when every expected value is there and within tolerance."""
    problems = []
    for name, (field, value, tolerance) in expected.items():
        got = found.get(name, {}).get(field)
        if got is None:
            problems.append(f"{name}: not measured")
        elif abs(got - value) > tolerance:
            problems.append(f"{name}: {field} {got:.3f}, expected {value:.3f} (±{tolerance})")
    return problems


def run(app, disc, output, mods):
    user = output / f"user-{mods}"
    shutil.rmtree(user, ignore_errors=True)
    subprocess.run([sys.executable, str(ROOT / "native/tools/create_observatory_fixture.py"), "--source",
                    str(ROOT / "build/observatory-user-2"), "--output", str(user), "--kind", "observatory"],
                   check=True, stdout=subprocess.DEVNULL)
    env = dict(os.environ, PETARI_SMOKE="movement", PETARI_MOVEMENT_ONLY=ONLY, PETARI_SMOKE_FRAMES="20000",
               PETARI_MODS="OdysseyMovement" if mods == "on" else "none", PETARI_PIPELINE_GLOBAL_PRECOMPILE="0")
    log = output / f"movement-{mods}.log"
    with log.open("w") as stream:
        subprocess.run([str(app), "--disc", str(disc), "--user", str(user), "--test-fixture", "observatory"],
                       stdout=stream, stderr=subprocess.STDOUT, env=env, timeout=900)
    return log.read_text(errors="replace")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--disc", type=Path, default=ROOT / "build/game-data/RMGE01")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mods", choices=["off", "on", "both"], default="both")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    failed = False
    for mods in (["off", "on"] if args.mods == "both" else [args.mods]):
        problems = compare(parse(run(args.app, args.disc, args.output, mods)), EXPECTED[mods])
        if problems:
            # The harness's 1-frame tap can land on two game frames (one extra held
            # frame: +17 with the mod); a real change fails again.
            print(f"movement baseline, mods {mods}: retrying once after: " + "; ".join(problems))
            problems = compare(parse(run(args.app, args.disc, args.output, mods)), EXPECTED[mods])
        print(f"movement baseline, mods {mods}: " + ("PASS" if not problems else "FAIL"))
        for problem in problems:
            print("  " + problem)
        failed |= bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

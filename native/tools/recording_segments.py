#!/usr/bin/env python3
"""Split a PETARI_ROUTE_RECORD recording into replayable route segments.

One segment per continuous visit to a stage and scenario (gaps of invalid
frames up to --join frames in the same stage/scenario are joined; a teleport
across a gap becomes a Warp waypoint). Front-end stages (FileSelect, title) are
skipped. Each segment's route is written by recorded_route.convert_selected to
<output>/<NN>-<stage>-s<scenario>.csv, and a manifest.json lists every segment
with its frames, start and end, and whether its power star count rose
(power_stars column; absent in older recordings, then "unknown").
Replay a star segment with PETARI_SMOKE=replay (native/app/smoke_replay.hpp).
"""
import argparse
import csv
import json
import math
from pathlib import Path

from recorded_route import convert_selected

SKIP = {"", "FileSelect"}


def segments(rows, join=120):
    out = []
    current = None
    gap = 0
    for row in rows:
        key = (row["stage"], row["scenario"])
        if row["valid"] != "1":
            if current is not None and key == current["key"]:
                gap += 1
                if gap > join:
                    current = None
            elif current is not None and row["stage"] != current["key"][0]:
                current = None
            continue
        if current is None or key != current["key"]:
            current = {"key": key, "rows": []}
            out.append(current)
        gap = 0
        current["rows"].append(row)
    for s in out:
        # A replay starts once Mario first stands in the stage: the arrival (an
        # intro demo, the flight in) is the game's, not the player's.
        first = next((i for i, r in enumerate(s["rows"]) if r["grounded"] == "1"), len(s["rows"]))
        s["rows"] = s["rows"][first:]
    return [s for s in out if s["key"][0] not in SKIP and len(s["rows"]) >= 2]


def star_gain(rows):
    if "power_stars" not in rows[0]:
        return "unknown"
    counts = [int(r["power_stars"]) for r in rows if r.get("power_stars", "-1") not in ("", "-1")]
    return "yes" if counts and max(counts) > counts[0] else "no"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("recording", type=Path)
    p.add_argument("output", type=Path, help="new directory for the segment routes")
    p.add_argument("--join", type=int, default=120, help="longest same-stage gap joined (frames)")
    p.add_argument("--spacing", type=float, default=150.0)
    p.add_argument("--keep-loops", action="store_true", help="keep sections a later teleport or launch returns to")
    args = p.parse_args()
    if args.join < 0 or not math.isfinite(args.spacing) or args.spacing <= 0:
        p.error("--join must be >= 0 and --spacing positive")
    with args.recording.open() as f:
        rows = list(csv.DictReader(f))
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = []
    for n, seg in enumerate(segments(rows, args.join)):
        stage, scenario = seg["key"]
        points = convert_selected(seg["rows"], args.spacing, args.keep_loops)
        name = f"{n:02d}-{stage}-s{scenario}.csv"
        with (args.output / name).open("x", newline="") as f:
            csv.writer(f).writerows(points)
        first, last = seg["rows"][0], seg["rows"][-1]
        manifest.append({
            "route": name, "stage": stage, "scenario": int(scenario),
            "frames": [int(first["frame"]), int(last["frame"])], "valid_frames": len(seg["rows"]),
            "start": [float(first[k]) for k in "xyz"], "end": [float(last[k]) for k in "xyz"],
            "waypoints": len(points), "actions": {a: sum(1 for *_, b in points if b == a) for a in sorted({b for *_, b in points})},
            "star": star_gain(seg["rows"]),
        })
    with (args.output / "manifest.json").open("x") as f:
        json.dump({"recording": str(args.recording), "segments": manifest}, f, indent=1)
    for m in manifest:
        print(f"{m['route']}: {m['valid_frames']} frames, {m['waypoints']} waypoints {m['actions']}, star {m['star']}")


if __name__ == "__main__":
    main()

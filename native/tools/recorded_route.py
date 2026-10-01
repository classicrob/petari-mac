#!/usr/bin/env python3
"""Convert foreground PETARI_ROUTE_RECORD CSV to dome-driver waypoints.
Hop is a recorded jump (no automatic spin); Spin preserves the recorded spin.
Kick is a jump pressed while airborne (a wall kick), at the wall contact.
Launch is a bind (Launch/Sling Star) the player spun out of, at the capture
point; Warp is a bind that carried the player at least 800 units without a spin.
Only one continuous visit to --stage is accepted. Output needs live validation.
Everything is counted in game frames, so the recording's wall-clock frame rate
does not matter.
"""
import argparse
import csv
import math
from pathlib import Path


def convert(rows, stage="AstroGalaxy", spacing=150.0):
    selected = []
    started = ended = False
    for row in rows:
        valid = row["valid"] == "1" and row["stage"] == stage
        if valid:
            if ended:
                raise ValueError("recording contains multiple stage visits; trim to one route")
            started = True
            selected.append(row)
        elif started:
            ended = True
    return convert_selected(selected, spacing)


def convert_selected(selected, spacing=150.0, keep_loops=False):
    """Waypoints for consecutive valid rows of one stage visit (recording_segments.py
    joins short same-stage gaps; a teleport across one becomes a Warp)."""
    if len(selected) < 2:
        raise ValueError("need at least two valid frames in the selected stage")
    def pos(row):
        return tuple(float(row[k]) for k in ("x", "y", "z"))
    if any(not all(math.isfinite(v) for v in pos(row)) for row in selected):
        raise ValueError("non-finite recorded position")
    points = [(pos(selected[0]), "Walk")]
    last_ground = selected[0]
    bind_start = None
    bind_spun = False
    last_spin = -100
    for i, row in enumerate(selected):
        previous = selected[max(0, i-1)]
        grounded = row["grounded"] == "1"
        bound = row.get("bound") == "1"
        # A teleport between two unbound frames (a joined gap, a scripted move):
        # wait where it happened for the game to move Mario.
        if i > 0 and not bound and previous.get("bound") != "1" and bind_start is None \
                and math.dist(pos(previous), pos(row)) >= 800:
            points.append((pos(previous), "Warp"))
            last_ground = row
        if row["spin"] == "1" and previous["spin"] != "1":
            last_spin = i
        if bound and bind_start is None:
            # The spin that fires a star lands a frame or two before the bind.
            bind_start = pos(previous if previous.get("bound") != "1" else row)
            bind_spun = i - last_spin <= 6
        if bound:
            bind_spun = bind_spun or (row["spin"] == "1" and previous["spin"] != "1")
            continue
        if bind_start is not None:
            if bind_spun: points.append((bind_start, "Launch"))
            elif math.dist(bind_start, pos(row)) >= 800: points.append((bind_start, "Warp"))
            bind_start = None
        if row["jump"] == "1" and previous["jump"] != "1":
            if previous["grounded"] == "1" or grounded or i == 0: points.append((pos(last_ground), "Hop"))
            elif i >= 2 and math.dist(pos(selected[i-2]), pos(previous)) < 0.5 and math.dist(pos(previous), pos(row)) < 0.5:
                # Clinging to a wall (still in the air): a wall kick. Other mid-air
                # presses do nothing in the game and are not route actions.
                points.append((pos(row), "Kick"))
        if row["spin"] == "1" and previous["spin"] != "1":
            points.append((pos(row), "Spin"))
        landed = grounded and previous["grounded"] != "1"
        # Preserve observed direction changes, without fitting across jump arcs.
        turn = False
        if grounded and i > 0 and i+1 < len(selected):
            before = selected[max(0,i-5)]; after = selected[min(len(selected)-1,i+5)]
            a = tuple(v-u for u,v in zip(pos(before),pos(row)))
            b = tuple(v-u for u,v in zip(pos(row),pos(after)))
            aa=math.sqrt(sum(v*v for v in a)); bb=math.sqrt(sum(v*v for v in b))
            turn = aa > 2 and bb > 2 and sum(u*v for u,v in zip(a,b))/(aa*bb) < .94
        if grounded and (landed or turn or math.dist(points[-1][0],pos(row)) >= spacing):
            if points[-1][0] != pos(row): points.append((pos(row), "Walk"))
        if grounded: last_ground = row
    if points[-1][0] != pos(selected[-1]): points.append((pos(selected[-1]), "Walk"))
    return [(*point, action) for point,action in (points if keep_loops else erase_retries(points))]


def erase_retries(points, reach=300.0):
    """A teleport or launch back to where the route already went (a death's
    respawn at a checkpoint, a star back to a hub after a failed or exploratory
    loop) repeats a section: keep only the final attempt. Teleports and launches
    to new places stay. (A loop that collected something needed later would be
    lost; recording_segments.py --keep-loops disables this.)"""
    points = list(points)
    i = 0
    while i < len(points) - 1:
        if points[i][1] in ("Warp", "Launch"):
            dest = points[i + 1][0]
            back = next((j for j in range(i) if points[j][1] == "Walk" and math.dist(points[j][0], dest) < reach), None)
            if back is not None:
                del points[back + 1:i + 1]  # the failed attempt and its Warp
                i = back
                continue
        i += 1
    return points


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("recording",type=Path); p.add_argument("output",type=Path)
    p.add_argument("--stage",default="AstroGalaxy");p.add_argument("--spacing",type=float,default=150)
    args=p.parse_args()
    if not math.isfinite(args.spacing) or args.spacing <= 0: p.error("spacing must be positive")
    with args.recording.open() as f: points=convert(list(csv.DictReader(f)),args.stage,args.spacing)
    with args.output.open("x",newline="") as f: csv.writer(f).writerows(points)
    print(f"Wrote {len(points)} candidate waypoints to {args.output}; validate with a bounded dome tour.")

if __name__ == "__main__": main()

#!/usr/bin/env python3
"""Plan the dome tour's observatory routes (PETARI_SMOKE=domes) from a map probe.

Input: the CSV of PETARI_COLLISION_PROBE (native/app/collision_probe.cpp), taken in
AstroGalaxy with an unlocked save (the restored observatory, scenario 5): the game's
own map collision, animated parts in their current pose. Output: C++ route tables
(native/app/smoke_domes_routes.cpp) from the file-load start to each dome's entrance.

Walk graph: upward surfaces (ny >= 0.55) with 160 units of headroom on a 50-unit
grid; a step to a neighbouring cell when the probe's horizontal rays found that
direction clear and the rise is at most 55 units; a drop of up to 450 units the
same way (one-way). Paths keep away from edges (cells missing neighbours). Warp
pods active in the save are extra links: the route stops on the pod (a warp
waypoint) and resumes at its pair. Dome entrances are the AstroChangeStageCube
areas (Obj_arg0 5, Obj_arg2 = dome).
"""
import argparse
import collections
import csv
import heapq
import math
import sys
from pathlib import Path

CELL = 50.0
STEP_UP = 55.0
JUMP_UP = float(__import__("os").environ.get("JUMP_UP", "320"))  # jump (peak about 203 units) plus an apex spin
DROP = 450.0
HEADROOM = 160.0
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
START = (2825.0, 785.6, -3750.0)
# AstroChangeStageCube dome entrances (AstroGalaxy.arc areaobjinfo, Obj_arg2 = dome).
DOMES = {1: (2634.4, 385.4, 1322.5), 2: (-2140.6, 725.4, 7128.3), 3: (-6830.5, 2115.8, -2696.8),
         4: (-3037.4, 450.6, 1913.3), 5: (3.7, 3783.4, 956.7), 6: (-192.5, 6164.5, 19.2)}
DOME_NAMES = {1: "Terrace", 2: "Fountain", 3: "Kitchen", 4: "Bedroom", 5: "Engine Room", 6: "Garden"}
# WarpPod pairs (GroupId) with both ends; all active in an unlocked save (grand-star
# requirements met, WarpPodSaveBits 0 and 1 set).
WARPS = [((-1083.7, 4453.8, -1300.4), (-7041.8, 5237.4, 3437.3)),
         ((-4732.5, 1650.0, -1583.1), (-2139.6, 3797.2, 807.9)),
         ((-1145.3, 6241.5, 0.1), (-6779.2, 5346.2, 4764.1)),
         ((-1672.8, 739.5, 6329.7), (-703.4, -44.5, 4761.8)),
         ((-601.5, 832.8, -4450.2), (1978.4, 3796.2, -1664.4)),
         ((1124.1, 6238.0, 0.3), (6106.6, 162.0, 4035.8))]
WARP_COST = 1500.0


def static_floors(triangles):
    """(column, y) for upward static triangles rasterized at cell centres."""
    import numpy as np
    normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    lengths = np.linalg.norm(normals, axis=1)
    out = []
    for t in np.nonzero(lengths > 1e-6)[0]:
        n = normals[t] / lengths[t]
        if n[1] < 0.55:
            continue
        a, b, c = triangles[t]
        v0, v1 = b - a, c - a
        d00, d01, d11 = v0[0] ** 2 + v0[2] ** 2, v0[0] * v1[0] + v0[2] * v1[2], v1[0] ** 2 + v1[2] ** 2
        den = d00 * d11 - d01 * d01
        if abs(den) < 1e-9:
            continue
        lo = np.floor(triangles[t][:, [0, 2]].min(0) / CELL).astype(int)
        hi = np.floor(triangles[t][:, [0, 2]].max(0) / CELL).astype(int)
        for i in range(lo[0], hi[0] + 1):
            for j in range(lo[1], hi[1] + 1):
                px, pz = (i + 0.5) * CELL - a[0], (j + 0.5) * CELL - a[2]
                d20, d21 = px * v0[0] + pz * v0[2], px * v1[0] + pz * v1[2]
                v, w = (d11 * d20 - d01 * d21) / den, (d00 * d21 - d01 * d20) / den
                if v >= -0.02 and w >= -0.02 and v + w <= 1.02:
                    out.append(((i, j), a[1] + v * v0[1] + w * v1[1], n[1]))
    return out


class StaticWalls:
    """Segment tests against static near-vertical triangles (either facing)."""

    def __init__(self, triangles):
        import numpy as np
        self.np = np
        normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
        lengths = np.linalg.norm(normals, axis=1)
        keep = (lengths > 1e-6) & (np.abs(normals[:, 1]) < 0.55 * np.maximum(lengths, 1e-9))
        self.tris = triangles[keep]
        self.cells = collections.defaultdict(list)
        for t, tri in enumerate(self.tris):
            lo = np.floor(tri[:, [0, 2]].min(0) / CELL).astype(int)
            hi = np.floor(tri[:, [0, 2]].max(0) / CELL).astype(int)
            for i in range(lo[0], hi[0] + 1):
                for j in range(lo[1], hi[1] + 1):
                    self.cells[(i, j)].append(t)

    def clear(self, x, y, z, column):
        np = self.np
        result = 0
        for d, (di, dj) in enumerate(DIRS):
            candidates = set(self.cells.get(column, ())) | set(self.cells.get((column[0] + di, column[1] + dj), ()))
            blocked = False
            if candidates:
                tris = self.tris[list(candidates)]
                length = CELL
                for height in (40.0, 110.0):
                    p = np.array([x, y + height, z])
                    q = p + np.array([di * length, 0.0, dj * length])
                    e1, e2 = tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]
                    dvec = q - p
                    h = np.cross(dvec, e2)
                    a = (e1 * h).sum(1)
                    ok = np.abs(a) > 1e-9
                    f = np.where(ok, 1 / np.where(ok, a, 1), 0)
                    sv = p - tris[:, 0]
                    u = f * (sv * h).sum(1)
                    qq = np.cross(sv, e1)
                    v = f * (qq @ dvec)
                    t = f * (e2 * qq).sum(1)
                    if np.any(ok & (u >= 0) & (u <= 1) & (v >= 0) & (u + v <= 1) & (t >= 0) & (t <= 1)):
                        blocked = True
                        break
            if not blocked:
                result |= 1 << d
        return result


def load(paths, static=None):
    """Probe hits per column from one or more probes (a full survey plus surveys taken
    near far structures). A surface seen twice keeps the more restrictive clear bits."""
    columns = collections.defaultdict(list)
    for path in ([paths] if isinstance(paths, (str, Path)) else paths):
        with open(path) as stream:
            for row in csv.DictReader(stream):
                key = (round((float(row["x"]) - CELL / 2) / CELL), round((float(row["z"]) - CELL / 2) / CELL))
                hit = (float(row["y"]), float(row["ny"]), int(row["clear"]), float(row["x"]), float(row["z"]))
                hits = columns[key]
                same = next((n for n, h in enumerate(hits) if abs(h[0] - hit[0]) <= 30), None)
                if same is None:
                    hits.append(hit)
                elif hit[2] >= 0 and hits[same][2] >= 0:
                    hits[same] = hits[same][:2] + (hits[same][2] & hit[2],) + hits[same][3:]
    walls = None
    if static is not None:
        walls = StaticWalls(static)
        added = 0
        for key, y, ny in static_floors(static):
            if all(abs(y - h[0]) > 30 for h in columns[key]):
                columns[key].append((y, ny, 255, (key[0] + 0.5) * CELL, (key[1] + 0.5) * CELL))
                added += 1
        print(f"static collision: {added} floor cells the probe did not see", file=sys.stderr)
    nodes, index = [], {}
    for key, hits in columns.items():
        hits.sort(key=lambda h: -h[0])
        for n, (y, ny, clear, x, z) in enumerate(hits):
            ceiling = hits[n - 1][0] if n > 0 else math.inf
            if ny >= 0.55 and clear >= 0 and ceiling - y >= HEADROOM:
                if walls is not None:
                    clear &= walls.clear(x, y, z, key)
                index[(key, n)] = len(nodes)
                nodes.append((x, y, z, clear, key))
    by_column = collections.defaultdict(list)
    for k, (x, y, z, clear, key) in enumerate(nodes):
        by_column[key].append(k)
    return nodes, by_column


def graph(nodes, by_column):
    """Edges (j, kind): kind None walks, "drop" falls, "jump" needs a jump up a ledge."""
    edges = collections.defaultdict(list)
    degree = [0] * len(nodes)
    for k, (x, y, z, clear, key) in enumerate(nodes):
        for d, (di, dj) in enumerate(DIRS):
            back = (d + 4) % 8
            for j in by_column.get((key[0] + di, key[1] + dj), ()):
                dy = nodes[j][1] - y
                if clear >> d & 1 and -STEP_UP <= dy <= STEP_UP:
                    edges[k].append((j, None))
                    degree[k] += 1
                elif clear >> d & 1 and -DROP <= dy < -STEP_UP:
                    # A lower floor is not reachable through an intervening platform.
                    # Choosing every hit in a column routed Mario underneath the
                    # Terrace floor while he was still standing on it.
                    intervening = any(nodes[j][1] < nodes[h][1] <= y + STEP_UP
                                      for h in by_column.get((key[0] + di, key[1] + dj), ()))
                    if not intervening:
                        edges[k].append((j, "drop"))
                elif STEP_UP < dy <= JUMP_UP and nodes[j][3] >> back & 1:
                    # A ledge: blocked at +40 from below, open from above.
                    edges[k].append((j, "jump"))
    return edges, degree


def nearest(nodes, point, height=300.0, reach=200.0):
    best = None
    for k, (x, y, z, _, _) in enumerate(nodes):
        d = math.hypot(x - point[0], z - point[2])
        if d <= reach and abs(y - point[1]) <= height:
            score = d + abs(y - point[1]) * 0.3
            if best is None or score < best[0]:
                best = (score, k)
    return None if best is None else best[1]


def plan(nodes, edges, degree, start, goal, warps):
    # Edge clearance: distance (in cells, up to 4) to a cell with missing neighbours.
    edge_cells = [k for k in range(len(nodes)) if degree[k] < 8]
    clearance = [4] * len(nodes)
    frontier = collections.deque((k, 0) for k in edge_cells)
    for k in edge_cells:
        clearance[k] = 0
    while frontier:
        k, c = frontier.popleft()
        if c >= 3:
            continue
        for j, kind in edges[k]:
            if kind is None and clearance[j] > c + 1:
                clearance[j] = c + 1
                frontier.append((j, c + 1))
    dist, prev = {start: 0.0}, {}
    queue = [(0.0, start)]
    while queue:
        d, k = heapq.heappop(queue)
        if k == goal:
            break
        if d > dist[k]:
            continue
        x, y, z = nodes[k][:3]
        links = list(edges[k]) + [(j, "warp") for j in warps.get(k, ())]
        for j, kind in links:
            if kind == "warp":
                cost = WARP_COST
            else:
                step = math.dist((x, y, z), nodes[j][:3])
                cost = step * (1 + 3.0 * (4 - clearance[j]) / 4) + {None: 0, "drop": 400, "jump": 600}[kind]
            if d + cost < dist.get(j, math.inf):
                dist[j] = d + cost
                prev[j] = (k, kind)
                heapq.heappush(queue, (d + cost, j))
    if goal not in dist:
        return None
    path, k = [], goal
    while k != start:
        path.append((k, prev[k][1]))
        k = prev[k][0]
    path.append((start, None))
    return path[::-1]


WALK, WARP, JUMP = "Walk", "Warp", "Jump"


def waypoints(nodes, path, spacing=150.0):
    """(x, y, z, action): Warp stops on a pod; Jump jumps from here toward the next point."""
    points, run = [], 0.0
    for n, (k, kind) in enumerate(path):
        x, y, z = nodes[k][:3]
        after = path[n + 1][1] if n + 1 < len(path) else None
        if n > 0 and kind != "warp":
            run += math.dist(nodes[path[n - 1][0]][:3], (x, y, z))
        turn = False
        if 0 < n < len(path) - 1 and after is None and kind is None:
            a, b = nodes[path[n - 1][0]], nodes[path[n + 1][0]]
            # Every change of direction: a straight line between points must not
            # cut the corner of a building the path goes around.
            turn = (x - a[0]) * (b[2] - z) - (z - a[2]) * (b[0] - x) != 0
            turn = turn or abs(b[1] - a[1]) > 60  # keep points at drops and slopes
        action = WARP if after == "warp" else JUMP if after == "jump" else WALK
        if n == len(path) - 1 or action != WALK or kind in ("warp", "jump", "drop") or run >= spacing or turn:
            points.append((x, y, z, action))
            run = 0.0
    return points


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("probe", type=Path, nargs="+", help="probe CSVs (a full survey first, then local ones)")
    parser.add_argument("--static", type=Path, help="placed collision (.npy, observatory_static_collision.py): adds the "
                        "floors and walls a probe missed")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "app/smoke_domes_routes.cpp")
    args = parser.parse_args()
    static = None
    if args.static:
        import numpy as np
        static = np.load(args.static)
    nodes, by_column = load(args.probe, static)
    edges, degree = graph(nodes, by_column)
    warp_links = collections.defaultdict(list)
    for a, b in WARPS:
        ka, kb = nearest(nodes, a), nearest(nodes, b)
        if ka is None or kb is None:
            print(f"warp pair {a} {b}: no walkable cell at an end", file=sys.stderr)
            continue
        warp_links[ka].append(kb)
        warp_links[kb].append(ka)
    start = nearest(nodes, START)
    routes = {}
    for dome, cube in DOMES.items():
        # The entrance area may enclose no floor cell (the Garden's is in the dome's
        # structure): end at the nearest walkable cell, then walk into the area.
        goal = nearest(nodes, cube, height=200.0) or nearest(nodes, cube, height=300.0, reach=700.0)
        path = plan(nodes, edges, degree, start, goal, warp_links) if goal is not None else None
        if path is None:
            print(f"dome {dome} ({DOME_NAMES[dome]}): no route", file=sys.stderr)
            continue
        routes[dome] = waypoints(nodes, path)
        if math.hypot(nodes[goal][0] - cube[0], nodes[goal][2] - cube[2]) > 60:
            routes[dome].append((cube[0], nodes[goal][1], cube[2], WALK))
        warps = sum(1 for p in routes[dome] if p[3] == WARP)
        jumps = sum(1 for p in routes[dome] if p[3] == JUMP)
        print(f"dome {dome} ({DOME_NAMES[dome]}): {len(path)} cells, {len(routes[dome])} waypoints, {warps} warp(s), {jumps} jump(s)")
    lines = ["// Generated by native/tools/observatory_routes.py from a PETARI_COLLISION_PROBE survey of",
             "// the restored observatory (AstroGalaxy scenario 5). Do not edit by hand.",
             "", '#include "smoke_domes.hpp"', "", "namespace PetariNative::App::Smoke {", "",
             "const std::vector<DomeWaypoint>& domeRoute(int dome) {"]
    lines.append("    static const std::vector<DomeWaypoint> kRoutes[7] = {")
    lines.append("        {},")
    for dome in range(1, 7):
        points = routes.get(dome, [])
        lines.append(f"        // {dome}: {DOME_NAMES[dome]}")
        lines.append("        {" + ", ".join(f"{{{x:.1f}f, {y:.1f}f, {z:.1f}f, DomeWaypoint::{a}}}" for x, y, z, a in points) + "},")
    lines += ["    };", "    static const std::vector<DomeWaypoint> kNone;",
              "    return dome >= 1 && dome <= 6 ? kRoutes[dome] : kNone;", "}", "", "}  // namespace PetariNative::App::Smoke", ""]
    args.output.write_text("\n".join(lines))
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()

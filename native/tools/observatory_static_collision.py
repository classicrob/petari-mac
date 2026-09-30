#!/usr/bin/env python3
"""Export the observatory's placed map collision (AstroGalaxy, one scenario's layers).

Every placed object and map part whose ObjectData archive has a .kcl, in its placed
pose (AstroDomeEntrance uses the dome's entrance model, in its open state). The
AstroMapObj revival animations only change colours, so the placed pose is the
restored shape. Complements PETARI_COLLISION_PROBE, which sees only collision
active near the player: the dome entrance buildings, for example, are missing from
a probe taken at the file-load start. Writes an .npy array (triangles, 3, 3).
"""
import argparse
import math
import struct
from pathlib import Path

import numpy as np

ENTRANCES = {1: "AstroDomeEntranceObservatory", 2: "AstroDomeEntranceWell", 3: "AstroDomeEntranceKitchen",
             4: "AstroDomeEntranceBedRoom", 5: "AstroDomeEntranceMachine", 6: "AstroDomeEntranceTower"}


def yaz0(data):
    if data[:4] != b"Yaz0":
        return data
    size = struct.unpack(">I", data[4:8])[0]
    out, src = bytearray(), 16
    while len(out) < size:
        code = data[src]
        src += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(data[src])
                src += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                length = b1 >> 4
                if length == 0:
                    length = data[src] + 0x12
                    src += 1
                else:
                    length += 2
                for _ in range(length):
                    out.append(out[-dist])
    return bytes(out)


def rarc(data):
    data_offset = struct.unpack(">I", data[12:16])[0] + 0x20
    _, node_offset, _, entry_offset, _, string_offset = struct.unpack(">IIIIII", data[0x20:0x38])
    node_offset += 0x20
    entry_offset += 0x20
    string_offset += 0x20

    def name(offset):
        end = data.index(b"\0", string_offset + offset)
        return data[string_offset + offset:end].decode("latin1")

    files = {}

    def walk(node, path):
        _, _, _, count, first = struct.unpack(">4sIHHI", data[node_offset + node * 16:node_offset + node * 16 + 16])
        for i in range(count):
            e = entry_offset + (first + i) * 20
            _, _, type_flags, name_offset, offset, size = struct.unpack(">HHHHII", data[e:e + 16])
            entry = name(name_offset)
            if entry in (".", ".."):
                continue
            if type_flags >> 8 & 2:
                walk(offset, path + entry + "/")
            else:
                files[(path + entry).lower()] = data[data_offset + offset:data_offset + offset + size]

    walk(0, "")
    return files


def load(path):
    return rarc(yaz0(Path(path).read_bytes()))


def jhash(text):
    value = 0
    for c in text.encode("latin1"):
        value = ((c - 256 if c >= 128 else c) + value * 31) & 0xFFFFFFFF
    return value


FIELDS = ["name", "pos_x", "pos_y", "pos_z", "dir_x", "dir_y", "dir_z", "scale_x", "scale_y", "scale_z", "Obj_arg0"]
HASHES = {jhash(n): n for n in FIELDS}


def bcsv(data):
    count, fields, data_offset, entry_size = struct.unpack(">IIII", data[:16])
    columns = []
    for i in range(fields):
        h, mask, offset, shift, kind = struct.unpack(">IIHBB", data[16 + i * 12:28 + i * 12])
        if h in HASHES:
            columns.append((HASHES[h], mask, offset, shift, kind))
    strings = data_offset + count * entry_size
    rows = []
    for r in range(count):
        base = data_offset + r * entry_size
        row = {}
        for name, mask, offset, shift, kind in columns:
            if kind == 2:
                row[name] = struct.unpack(">f", data[base + offset:base + offset + 4])[0]
            elif kind == 6:
                start = strings + struct.unpack(">I", data[base + offset:base + offset + 4])[0]
                row[name] = data[start:data.index(b"\0", start)].decode("shift_jis", "replace")
            elif kind in (0, 3):
                value = (struct.unpack(">I", data[base + offset:base + offset + 4])[0] & mask) >> shift
                row[name] = value - (1 << 32) if value >= 1 << 31 else value
        rows.append(row)
    return rows


def kcl(data):
    positions, normals, prisms, octree = struct.unpack(">IIII", data[:16])
    P = np.frombuffer(data[positions:normals], dtype=">f4").reshape(-1, 3).astype(np.float64)
    N = np.frombuffer(data[normals:normals + ((prisms + 0x10 - normals) // 12) * 12], dtype=">f4").reshape(-1, 3).astype(np.float64)
    tris = []
    for i in range((octree - (prisms + 0x10)) // 0x10):
        o = prisms + 0x10 + i * 0x10
        height, pi, fn, e1, e2, e3, _ = struct.unpack(">fHHHHHH", data[o:o + 16])
        p, f, a, b, c = P[pi], N[fn], N[e1], N[e2], N[e3]
        ca, cb = np.cross(a, f), np.cross(b, f)
        tris.append((p, p + cb * (height / np.dot(cb, c)), p + ca * (height / np.dot(ca, c))))
    return np.array(tris)


def placement_matrix(row):
    # JMap rotation order as the planning tools use it (Z * Y * X), then scale.
    sx, sy, sz = (math.sin(math.radians(row[k])) for k in ("dir_x", "dir_y", "dir_z"))
    cx, cy, cz = (math.cos(math.radians(row[k])) for k in ("dir_x", "dir_y", "dir_z"))
    rotation = np.array([[cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
                         [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
                         [-sy, cy * sx, cy * cx]])
    return rotation @ np.diag([row["scale_x"], row["scale_y"], row["scale_z"]]), np.array([row["pos_x"], row["pos_y"], row["pos_z"]])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--disc", type=Path, default=Path(__file__).resolve().parents[2] / "build/game-data/RMGE01")
    parser.add_argument("--layers", default="common,layera,layerb,layerg", help="scenario 5 (the restored observatory): common,a,b,g")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    layers = set(args.layers.split(","))
    stage = load(args.disc / "files/StageData/AstroGalaxy.arc")
    cache, parts = {}, []
    for key, data in sorted(stage.items()):
        p = key.split("/")
        if len(p) != 4 or p[2] not in layers or p[3] not in ("objinfo", "mappartsinfo"):
            continue
        for row in bcsv(data):
            name = row["name"] if row["name"] != "AstroDomeEntrance" else ENTRANCES.get(row.get("Obj_arg0"), "")
            archive = args.disc / "files/ObjectData" / f"{name}.arc"
            if not archive.exists():
                continue
            if name not in cache:
                files = load(archive)
                models = [k for k in files if k.endswith(".kcl")]
                chosen = [k for k in models if "open" in k] or models
                cache[name] = kcl(files[chosen[0]]) if chosen else None
            if cache[name] is None:
                continue
            matrix, offset = placement_matrix(row)
            parts.append(cache[name] @ matrix.T + offset)
    triangles = np.concatenate(parts)
    np.save(args.output, triangles)
    print(f"{len(triangles)} triangles from {len(parts)} placed objects -> {args.output}")


if __name__ == "__main__":
    main()

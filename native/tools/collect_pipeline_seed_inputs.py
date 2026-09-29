#!/usr/bin/env python3
"""Collect stage/shared model material-shape inputs for an offline GX seed replay.

This first cut emits an auditable input manifest, not a pipeline cache. It reads
RARC/Yaz0/Yay0, follows stage zones and static archive aliases, and indexes J3D
hierarchy pairs and BDL material packets. Dynamic factory dependencies and
procedural render states remain explicit coverage gaps.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

LIMIT = 256 * 1024 * 1024


def require(value, message):
    if not value:
        raise ValueError(message)


def region(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, 'range outside input')
    return data[offset:offset + size]


def u16(data, offset):
    return struct.unpack('>H', region(data, offset, 2))[0]


def u32(data, offset):
    return struct.unpack('>I', region(data, offset, 4))[0]


def string(data, offset):
    require(0 <= offset < len(data), 'string offset outside input')
    end = data.find(b'\0', offset)
    require(end >= 0, 'unterminated string')
    return data[offset:end].decode('shift_jis', errors='replace')


def decompress(data):
    if data[:4] not in (b'Yaz0', b'Yay0'):
        require(len(data) <= LIMIT, 'input exceeds limit')
        return data
    size = u32(data, 4)
    require(size <= LIMIT, 'decompressed input exceeds limit')
    yay = data[:4] == b'Yay0'
    maskpos, linkpos, literalpos = 16, u32(data, 8) if yay else 16, u32(data, 12) if yay else 16
    if yay:
        require(16 <= linkpos <= literalpos <= len(data), 'invalid Yay0 stream offsets')
    maskend, linkend = (linkpos, literalpos) if yay else (len(data), len(data))
    result = bytearray()
    bits, mask = 0, 0
    while len(result) < size:
        if not bits:
            if yay:
                require(maskpos + 4 <= maskend, 'truncated Yay0 mask')
                mask = u32(data, maskpos)
                maskpos += 4
                bits = 32
            else:
                mask = region(data, literalpos, 1)[0] << 24
                literalpos += 1
                bits = 8
        if mask & 0x80000000:
            result += region(data, literalpos, 1)
            literalpos += 1
        else:
            cursor = linkpos if yay else literalpos
            require(cursor + 2 <= linkend, 'truncated compression link')
            a, b = region(data, cursor, 2)
            if yay:
                linkpos += 2
            else:
                literalpos += 2
            distance = ((a & 15) << 8 | b) + 1
            length = a >> 4
            if length:
                length += 2
            else:
                length = region(data, literalpos, 1)[0] + 18
                literalpos += 1
            require(distance <= len(result) and length <= size - len(result), 'invalid compression back-reference')
            for _ in range(length):
                result.append(result[-distance])
        mask = (mask << 1) & 0xffffffff
        bits -= 1
    return bytes(result)


def archive_files(data):
    data = decompress(data)
    require(data[:4] == b'RARC', 'expected RARC archive')
    size, header = u32(data, 4), u32(data, 8)
    require(64 <= size <= len(data) and header >= 32, 'invalid RARC header')
    data = data[:size]
    payload, payloadsize = header + u32(data, 12), u32(data, 16)
    region(data, payload, payloadsize)
    count, entries = u32(data, header + 8), header + u32(data, header + 12)
    strings = region(data, header + u32(data, header + 20), u32(data, header + 16))
    region(data, entries, count * 20)
    for i in range(count):
        entry = entries + i * 20
        flags = data[entry + 4]
        if flags & 2:
            continue
        require(flags & 1, 'invalid RARC file flags')
        name = string(strings, int.from_bytes(data[entry + 5:entry + 8], 'big'))
        offset, length = u32(data, entry + 8), u32(data, entry + 12)
        require(offset + length <= payloadsize, 'resource outside RARC payload')
        yield i, name, decompress(region(data, payload + offset, length))


def field_hash(name):
    value = 0
    for char in name.encode('ascii'):
        value = (value * 31 + char) & 0xffffffff
    return value


def bcsv_strings(data):
    count, fields, offset, stride = (u32(data, i) for i in (0, 4, 8, 12))
    require(16 + fields * 12 <= offset <= len(data), 'invalid BCSV header')
    region(data, offset, count * stride)
    strings = data[offset + count * stride:]
    for row in range(count):
        values = {}
        record = region(data, offset + row * stride, stride)
        for field in range(fields):
            entry = 16 + field * 12
            key, pos, kind = u32(data, entry), u16(data, entry + 8), data[entry + 11]
            if kind == 6:
                value = string(strings, u32(record, pos))
            elif kind == 1:
                value = region(record, pos, 32).split(b'\0')[0].decode('shift_jis', errors='replace')
            else:
                continue
            values[key] = value
        yield values


def model_inventory(data):
    require(data[:4] == b'J3D2' and data[4:8] in (b'bdl4', b'bmd3'), 'unsupported J3D model version')
    require(u32(data, 8) <= len(data), 'truncated J3D model')
    blocks, offset = {}, 32
    for _ in range(u32(data, 12)):
        kind = region(data, offset, 4).decode('ascii')
        size = u32(data, offset + 4)
        require(size >= 8, 'invalid J3D block size')
        blocks[kind] = (offset, region(data, offset, size))
        offset += size
    materials = u16(blocks['MAT3'][1], 8) if 'MAT3' in blocks else u16(blocks['MDL3'][1], 8)
    shapes = u16(blocks['SHP1'][1], 8)
    info = blocks['INF1'][1]
    pos = u32(info, 20)
    stack, material, pairs = [], None, set()
    while True:
        command, index = u16(info, pos), u16(info, pos + 2)
        pos += 4
        if command == 0:
            break
        if command == 1:
            stack.append(material)
        elif command == 2:
            require(stack, 'unbalanced INF1 hierarchy')
            material = stack.pop()
        elif command == 0x11:
            require(index < materials, 'INF1 material outside table')
            material = index
        elif command == 0x12:
            require(material is not None and index < shapes, 'INF1 shape has no valid material')
            pairs.add((material, index))
    packets = []
    if 'MDL3' in blocks:
        base, block = blocks['MDL3']
        table = u32(block, 12)
        for index in range(u16(block, 8)):
            entry = table + index * 8
            packet_offset, size = entry + u32(block, entry), u32(block, entry + 4)
            packet = region(block, packet_offset, size)
            packets.append({'material': index, 'model_offset': base + packet_offset,
                            'size': size, 'sha256': hashlib.sha256(packet).hexdigest()})
    return {'format': data[4:8].decode(), 'material_count': materials, 'shape_count': shapes,
            'pairs': [{'material': m, 'shape': s} for m, s in sorted(pairs)], 'material_packets': packets,
            'needs_material_generation': 'MDL3' not in blocks}


def collect(files, repo, stages, shared):
    object_paths = {p.stem: p for p in (files / 'ObjectData').glob('*.arc')}
    stage_paths = {p.stem: p for p in (files / 'StageData').glob('*.arc')}
    source = (repo / 'src/Game/NameObj/NameObjFactory.cpp').read_text()
    aliases = dict(re.findall(r'\{\s*"([^"\n]+)"\s*,[^{}]*?,\s*"([^"\n]+)"\s*,?\s*\}', source))
    archive_cache = {}

    def read(path):
        if path not in archive_cache:
            archive_cache[path] = list(archive_files(path.read_bytes()))
        return archive_cache[path]

    result = {'schema': 1, 'kind': 'pipeline-replay-inputs', 'files_root': str(files.resolve()),
              'config_seed_emitted': False, 'stages': {},
              'limitations': ['Static aliases and conservative shared models only; dynamic actor dependencies may be absent.',
                              'All scenario layers included; this is broader than mission 1.',
                              'Particle/layout/procedural draws and runtime material mutations need additional emitters.',
                              'Manifest pairs must still be replayed through native J3D/GX config builder before producing a seed DB.']}
    for stage in stages:
        scenario = files / 'StageData' / stage / (stage + 'Scenario.arc')
        require(scenario.is_file() and stage in stage_paths, 'stage/scenario archive missing: ' + stage)
        queue = [scenario, stage_paths[stage]]
        selected, reasons, objects, unresolved = set(), {}, set(), set()

        def add(path, reason):
            reasons.setdefault(str(path.relative_to(files)), set()).add(reason)
            if path not in selected and path not in queue:
                queue.append(path)

        def add_object(name, reason):
            candidate = aliases.get(name, name)
            found = False
            for suffix in ('', 'Low', 'Middle', 'Bloom', 'Water', 'Indirect'):
                if candidate + suffix in object_paths:
                    add(object_paths[candidate + suffix], reason)
                    found = True
            return found

        for name in sorted(object_paths):
            if name.startswith(('Mario', 'Luigi', 'Kinopio', 'StarPointer', 'StarPiece', 'Coin', 'PowerStar')):
                add(object_paths[name], 'conservative shared model family')
        for name in shared:
            require(add_object(name, 'explicit shared archive'), 'shared archive not found: ' + name)
        if stage == 'AstroGalaxy':
            for name, path in stage_paths.items():
                if name.startswith('AstroDome'):
                    add(path, 'observatory dome transition')
            for name in object_paths:
                if name.startswith('Astro'):
                    add_object(name, 'observatory shared model family')

        for layout in sorted((files / 'LayoutData').glob('*.arc')):
            if layout.stem.startswith(('Galaxy', 'Map', 'Scenario')):
                add(layout, 'galaxy-map/scenario-select shared layout')

        models, nonmodels = [], set()
        while queue:
            path = queue.pop(0)
            if path in selected:
                continue
            selected.add(path)
            for entry, name, data in read(path):
                if name.lower().endswith(('.bcsv', '.csv')) or (path.parent.name == 'StageData' and name.lower().split('.')[0].endswith('info')):
                    try:
                        rows = list(bcsv_strings(data))
                    except ValueError as error:
                        raise ValueError(f'{path}:{name}: {error}') from error
                    for row in rows:
                        for value in row.values():
                            if value in stage_paths:
                                add(stage_paths[value], str(path.relative_to(files)) + ':' + name)
                            if value in object_paths or value in aliases:
                                add_object(value, str(path.relative_to(files)) + ':' + name)
                        actor = row.get(field_hash('name'))
                        if actor:
                            objects.add(actor)
                            if not add_object(actor, 'placement: ' + actor):
                                unresolved.add(actor)
                elif name.lower().endswith(('.bdl', '.bmd')):
                    model = model_inventory(data)
                    model.update(archive=str(path.relative_to(files)), resource=name, entry=entry,
                                 sha256=hashlib.sha256(data).hexdigest(), size=len(data))
                    models.append(model)
                elif name.lower().endswith(('.brlyt', '.jpc', '.bmt')):
                    nonmodels.add(str(path.relative_to(files)) + ':' + name)
        result['stages'][stage] = {
            'archives': [{'path': str(p.relative_to(files)), 'reasons': sorted(reasons.get(str(p.relative_to(files)), {'stage root'}))}
                         for p in sorted(selected)],
            'placed_names': sorted(objects), 'unresolved_placed_names': sorted(unresolved),
            'nonmodel_emitters_needed': sorted(nonmodels), 'models': sorted(models, key=lambda m: (m['archive'], m['entry']))}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files', type=Path, required=True, help='extracted RMGE01/files directory')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--stage', action='append', help='defaults to AstroGalaxy and EggStarGalaxy')
    parser.add_argument('--shared', action='append', default=[], help='additional ObjectData archive basename')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    manifest = collect(args.files, args.repo, args.stage or ['AstroGalaxy', 'EggStarGalaxy'], args.shared)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + '\n')
    for stage, data in manifest['stages'].items():
        print(f'{stage}: {len(data["archives"])} archives, {len(data["models"])} models, '
              f'{sum(len(m["pairs"]) for m in data["models"])} material-shape pairs; '
              f'{len(data["unresolved_placed_names"])} unresolved placement names')
    print('Replay input manifest only; no pipeline cache emitted.')

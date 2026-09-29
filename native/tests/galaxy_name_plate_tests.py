#!/usr/bin/env python3
"""Check GalaxyNamePlate's text-width lookups against the disc's actual text panes."""
import argparse
from pathlib import Path
import re
import struct


def u32(data, offset):
    return struct.unpack_from('>I', data, offset)[0]


def decompress(data):
    if data[:4] != b'Yaz0':
        return data
    size = u32(data, 4)
    output = bytearray()
    cursor = 16
    while len(output) < size:
        mask = data[cursor]
        cursor += 1
        for bit in range(7, -1, -1):
            if len(output) == size:
                break
            if mask & (1 << bit):
                output.append(data[cursor])
                cursor += 1
            else:
                first, second = data[cursor:cursor + 2]
                cursor += 2
                distance = ((first & 15) << 8 | second) + 1
                length = first >> 4
                if length:
                    length += 2
                else:
                    length = data[cursor] + 18
                    cursor += 1
                for _ in range(length):
                    output.append(output[-distance])
    assert len(output) == size
    return bytes(output)


def text_panes(archive):
    data = decompress(archive.read_bytes())
    assert data[:4] == b'RARC'
    header = u32(data, 8)
    payload = header + u32(data, 12)
    entries = header + u32(data, header + 12)
    result = set()
    for i in range(u32(data, header + 8)):
        entry = entries + i * 20
        if data[entry + 4] & 2:
            continue
        offset = payload + u32(data, entry + 8)
        layout = decompress(data[offset:offset + u32(data, entry + 12)])
        if layout[:4] != b'RLYT':
            continue
        block = struct.unpack_from('>H', layout, 12)[0]
        for _ in range(struct.unpack_from('>H', layout, 14)[0]):
            size = u32(layout, block + 4)
            assert size >= 8 and block + size <= len(layout)
            if layout[block:block + 4] == b'txt1':
                result.add(layout[block + 12:block + 28].split(b'\0')[0].decode('ascii'))
            block += size
    assert result, 'archive contains no BRLYT text panes'
    return result


def check(source, archive):
    # Read the lookup variable actually passed to the width adjustment so both
    # orientations remain covered if the local variable gets renamed.
    text = source.read_text()
    lookup = re.search(r'setAnimFrameAndStopAdjustTextWidth\(this,\s*(\w+),\s*2\)', text)
    assert lookup, 'text width adjustment not found'
    assignment = re.search(r'\b' + lookup[1] + r'\s*=\s*\(a3\)\s*\?\s*"([^"]+)"\s*:\s*"([^"]+)"', text)
    assert assignment, 'both name-plate orientation lookups not found'
    panes = text_panes(archive)
    for name in assignment.groups():
        assert name in panes, f'{name!r} is not a text pane in {archive.name}; available: {sorted(panes)}'
    print('GalaxyNamePlate: both text-width lookup panes exist in disc BRLYT')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'src/Game/Screen/GalaxyNamePlate.cpp')
    args = parser.parse_args()
    check(args.source, args.archive)

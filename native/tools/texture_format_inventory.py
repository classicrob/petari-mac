#!/usr/bin/env python3
"""Inventory serialized texture formats and source-level runtime format uses.

Run the full corpus under locked-build.sh. This reads original disc bytes;
it does not launch a GPU or claim dynamic source expressions are resolved.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

from collect_pipeline_seed_inputs import archive_files, decompress, region, require, u16, u32


def bti(data, offset, kind):
    header = region(data, offset, 32)
    return dict(kind=kind, offset=offset, format=header[0], width=u16(header, 2), height=u16(header, 4),
                wrap_s=header[6], wrap_t=header[7], palette_format=header[9], palette_entries=u16(header, 10),
                min_filter=header[20], mag_filter=header[21], anisotropy=header[19], mips=header[24])


def blocks(data, offset, count):
    for _ in range(count):
        size = u32(data, offset + 4)
        require(size >= 8, 'invalid block size')
        yield offset, region(data, offset, size)
        offset += size


def textures(data, name):
    if name.lower().endswith('.bti'):
        yield bti(data, 0, 'BTI')
    elif data[:4] in (b'J3D1', b'J3D2') and data[4:8] in (b'bmd3', b'bdl4', b'bmt3'):
        for base, block in blocks(data, 32, u32(data, 12)):
            if block[:4] == b'TEX1':
                for index in range(u16(block, 8)):
                    offset = u32(block, 12) + index * 32
                    region(block, offset, 32)
                    yield bti(data, base + offset, 'J3D/TEX1')
    elif data[:8] == b'JPAC2-10':
        for base, block in blocks(data, u32(data, 12), u16(data, 10)):
            require(block[:4] == b'TEX1', 'unknown particle texture block')
            region(block, 32, 32)
            yield bti(data, base + 32, 'JPA/TEX1')
    elif data[:4] == b'\x00\x20\xaf\x30':
        count, table = u32(data, 4), u32(data, 8)
        region(data, table, count * 8)
        for index in range(count):
            header, palette = u32(data, table + index * 8), u32(data, table + index * 8 + 4)
            region(data, header, 36)
            yield dict(kind='TPL', offset=header, format=u32(data, header + 4),
                       width=u16(data, header + 2), height=u16(data, header),
                       wrap_s=u32(data, header + 12), wrap_t=u32(data, header + 16),
                       min_filter=u32(data, header + 20), mag_filter=u32(data, header + 24),
                       palette_format=u32(data, palette + 4) if palette else None,
                       palette_entries=u16(data, palette) if palette else 0)
    elif data[:4] == b'RFNT':
        require(u16(data, 4) == 0xfeff, 'unsupported font byte order')
        for base, block in blocks(data, u16(data, 12), u16(data, 14)):
            if block[:4] == b'TGLP':
                region(block, 8, 24)
                yield dict(kind='RFNT/TGLP', offset=base, format=u16(block, 18),
                           width=u16(block, 24), height=u16(block, 26), palette_entries=0,
                           palette_format=None)
    elif Path(name).suffix.lower() in ('.tpl', '.brfnt', '.jpc', '.bdl', '.bmd', '.bmt'):
        raise ValueError('unrecognized texture-bearing resource header: ' + repr(data[:8]))


def resources(data, name, trail='', depth=0):
    require(depth < 8, 'nested archive depth exceeded')
    if data[:4] != b'RARC':
        yield trail or '0', name, data
        return
    for entry, child, contents in archive_files(data):
        yield from resources(contents, child, f'{trail}/{entry}:{child}', depth + 1)


def particle_states(data):
    if data[:8] != b'JPAC2-10': return
    offset = 16
    for _ in range(u16(data, 8)):
        resource, count = u16(data, offset), u16(data, offset + 2)
        offset += 8
        for base, block in blocks(data, offset, count):
            if block[:4] == b'BSP1':
                blend, flags = u16(block, 24), u32(block, 8)
                mode, source, dest, logic = blend & 3, (blend >> 2) & 15, (blend >> 6) & 15, (blend >> 10) & 15
                color_args = (flags >> 15) & 7
                yield dict(resource_id=resource, offset=base, blend_mode_index=mode,
                           source_factor_index=source, dest_factor_index=dest, logic_index=logic,
                           color_args_index=color_args,
                           invalid_table_index=mode > 2 or source > 9 or dest > 9 or color_args > 5,
                           unsupported_logic=mode == 2 and logic not in (0, 2, 4))
            offset = base + len(block)
    require(offset <= u32(data, 12), 'particle resources overlap texture table')


def source_uses(repo):
    uses, calls = [], []
    for folder in ('src/Game', 'src/JSystem', 'src/nw4r'):
        for path in sorted((repo / folder).rglob('*')):
            if path.suffix not in ('.c', '.cpp', '.hpp', '.h'): continue
            text = path.read_text(errors='replace')
            relative = str(path.relative_to(repo))
            for line, value in enumerate(text.splitlines(), 1):
                tokens = re.findall(r'\bGX_(?:TF|CTF|TL)_[A-Z0-9_]+\b', value)
                if tokens: uses.append(dict(file=relative, line=line, formats=sorted(set(tokens)), source=value.strip()))
            for match in re.finditer(r'\b(GXInitTexObj(?:CI)?|GXInitTlutObj|GXSetTexCopyDst)\s*\(', text):
                start, end, depth = match.end(), match.end(), 1
                while end < len(text) and depth:
                    depth += (text[end] == '(') - (text[end] == ')')
                    end += 1
                calls.append(dict(file=relative, line=text.count('\n', 0, match.start()) + 1,
                                  function=match[1], arguments=text[start:end-1].strip()))
    return dict(format_references=uses, calls=calls,
                limitation='Source expressions are recorded, not evaluated. References include sampling, copy and Z-texture uses; they are not interchangeable.')


def inventory(args):
    records, errors, archive_signatures = [], [], {}
    particles, ignored = [], []
    candidates = sorted(path for path in args.files.rglob('*') if path.is_file() and
                        path.suffix.lower() in ('.arc', '.bti', '.tpl', '.brfnt', '.jpc', '.bdl', '.bmd', '.bmt'))
    for index, path in enumerate(candidates):
        relative = str(path.relative_to(args.files))
        try:
            raw = path.read_bytes()
            archive_signatures[relative] = hashlib.sha256(raw).hexdigest()
            data = decompress(raw)
            for entry, name, contents in resources(data, path.name):
                try:
                    if relative == 'AudioRes/Info/JaiMe.arc' and name == 'metable.bmt':
                        ignored.append(dict(archive=relative, entry=entry, resource=name,
                                            reason='Audio ME table; extension collides with J3D material files'))
                        continue
                    for state in particle_states(contents):
                        particles.append(dict(archive=relative, entry=entry, resource=name, **state))
                    for record in textures(contents, name):
                        records.append(dict(archive=relative, entry=entry, resource=name, **record))
                except (ValueError, IndexError) as error:
                    errors.append(dict(archive=relative, entry=entry, resource=name, error=str(error)))
        except (ValueError, IndexError) as error:
            errors.append(dict(archive=relative, error=str(error)))
        if index % 100 == 0: print(f'{index + 1}/{len(candidates)} textures={len(records)} errors={len(errors)}', flush=True)
    result = dict(files=str(args.files.resolve()), inputs=archive_signatures, textures=records, errors=errors,
                  ignored_resources=ignored, particle_states=particles,
                  formats=dict(sorted(Counter(record['format'] for record in records).items())),
                  ci_tlut_pairs=dict(sorted(Counter(f"{record['format']}:{record['palette_format']}" for record in records
                                                     if record['format'] in (8, 9, 10)).items())),
                  runtime=source_uses(args.repo),
                  limitations=['Headers are inventoried; pixel payloads are not replayed by this tool.',
                               'No proof of exhaustive dynamic GX state coverage; runtime expressions need source review.',
                               'Nested RARC is interpreted; other unrecognized containers may hide texture headers.'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(inputs=len(archive_signatures), textures=len(records), errors=len(errors), formats=result['formats'],
                          ci_tlut_pairs=result['ci_tlut_pairs'], particle_states=len(particles),
                          invalid_particle_states=sum(row['invalid_table_index'] or row['unsupported_logic'] for row in particles)), indent=2))
    return bool(errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files', type=Path, default=Path('build/game-data/RMGE01/files'))
    parser.add_argument('--repo', type=Path, default=Path('.'))
    parser.add_argument('--output', type=Path, required=True)
    raise SystemExit(inventory(parser.parse_args()))

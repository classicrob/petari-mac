#!/usr/bin/env python3
"""Join FIFO-ordered producer stacks with per-stage first-use pipeline hashes."""
import argparse
from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3


def report(log, seeds=None, cache=None):
    stacks, configs = {}, []
    present = set()
    if cache:
        with closing(sqlite3.connect(cache.resolve().as_uri() + '?mode=ro', uri=True)) as db:
            present = {f'{key & ((1 << 64)-1):016x}' for key, in db.execute('SELECT hash FROM pipeline_cache WHERE type=1')}
    seed_hashes = {}
    for line in log.read_text(errors='replace').splitlines():
        if '[gx draw owner]' in line:
            fields = dict(re.findall(r'(owner|frame|pc|image_base)=([^\s]+)', line))
            fields['symbol'] = line.split(' symbol=', 1)[-1]
            stacks.setdefault(fields['owner'], []).append(fields)
        elif '[gx stage config]' in line:
            fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
            if fields.get('overlay_tag') != '1': configs.append(fields)
    output = []
    for entry in configs:
        stage = entry['stage']
        if seeds and stage not in seed_hashes:
            path = seeds / (stage + '.db')
            seed_hashes[stage] = None
            if path.is_file():
                with closing(sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)) as db:
                    seed_hashes[stage] = {f'{key & ((1 << 64)-1):016x}' for key, in db.execute('SELECT hash FROM pipeline_cache WHERE type=1')}
        stack = stacks.get(entry.get('owner', '0'), [])
        symbols = '\n'.join(frame['symbol'] for frame in stack)
        family = 'unattributed'
        for token, candidate in [('JPA', 'JPA particles'), ('CharWriter', 'font CharWriter'), ('J3D', 'J3D draw'), ('nw4r', 'layout')]:
            if token in symbols:
                family = candidate; break
        if stack and family == 'unattributed': family = 'other producer (inspect stack)'
        output.append({**entry, 'producer_family': family, 'stack': stack,
                       'captured_config_present': entry['config'] in present if cache else None,
                       'missing_from_seed': entry['config'] not in seed_hashes[stage] if seeds and seed_hashes[stage] is not None else None})
    return {'log': str(log), 'records': output,
            'note': 'First observed producer for each stage/config. Shared states can have additional owners; family is inferred from recorded symbols, not material identity.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--seeds', type=Path)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    value = report(args.log, args.seeds, args.cache)
    args.output.write_text(json.dumps(value, indent=2) + '\n')
    for entry in value['records']:
        if entry['phase'] == 'after_prep' and (entry['covered'] == '0' or entry['missing_from_seed']):
            print(entry['stage'], entry['config'], 'owner=' + entry.get('owner', '0'), entry['producer_family'])

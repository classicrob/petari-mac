#!/usr/bin/env python3
"""Merge explicitly stage-attributed runtime configs into replay and global seeds.

Reapply after regenerating the offline corpus. Observed additions retain their
cache/log provenance and are never labeled as offline replay or exhaustive coverage.
"""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import re
import shutil
import sqlite3


def merge(seeds, stage, cache, logs):
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,96}', stage): raise ValueError('unsafe stage name')
    keys = set()
    for log in logs:
        for line in log.read_text(errors='replace').splitlines():
            if '[gx stage config]' not in line: continue
            fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
            if fields.get('stage') == stage and fields.get('overlay_tag', '0') == '0': keys.add(int(fields['config'], 16))
    if not keys: raise ValueError('no real-stage first-use records for ' + stage)
    with closing(sqlite3.connect(cache.resolve().as_uri() + '?mode=ro', uri=True)) as db:
        rows = [row for row in db.execute('SELECT type,hash,config_version,config_size,config,first_frame_used FROM pipeline_cache WHERE type=1')
                if row[1] & ((1 << 64) - 1) in keys]
    found = {row[1] & ((1 << 64)-1) for row in rows}
    if keys - found: raise ValueError('captured cache lacks logged configs: ' + ','.join(f'{key:016x}' for key in sorted(keys-found)))
    provenance = {'source_stage': stage, 'kind': 'stage-observed', 'cache': str(cache.resolve()),
                  'cache_sha256': hashlib.sha256(cache.read_bytes()).hexdigest(),
                  'logs': {str(log.resolve()): hashlib.sha256(log.read_bytes()).hexdigest() for log in logs},
                  'observed_configs': len(rows), 'offline_material_replay': False}
    results = {}
    for name in (stage, '__global__'):
        target = seeds / (name + '.db')
        if target.resolve() == cache.resolve(): raise ValueError('output would overwrite capture')
        if not target.is_file(): raise ValueError('replay seed missing: ' + str(target))
        metadata = json.loads(target.with_suffix('.json').read_text())
        temporary = target.with_suffix('.merge.tmp.db')
        if temporary.exists(): raise ValueError('temporary output exists: ' + str(temporary))
        shutil.copy2(target, temporary)
        with closing(sqlite3.connect(temporary)) as db, db:
            before = db.execute('SELECT count(*) FROM pipeline_cache').fetchone()[0]
            expected = set(db.execute('SELECT DISTINCT config_version,config_size FROM pipeline_cache WHERE type=1'))
            for row in rows:
                if (row[2], row[3]) not in expected or len(row[4]) != row[3]: raise ValueError('config ABI mismatch')
                existing = db.execute('SELECT config FROM pipeline_cache WHERE type=? AND hash=?', row[:2]).fetchone()
                if existing and existing[0] != row[4]: raise ValueError('same hash with different config bytes')
                db.execute('INSERT INTO pipeline_cache VALUES(?,?,?,?,?,?) ON CONFLICT(type,hash) DO UPDATE SET first_frame_used=MIN(pipeline_cache.first_frame_used,excluded.first_frame_used)', row)
            after = db.execute('SELECT count(*) FROM pipeline_cache').fetchone()[0]
        metadata['configs'] = after
        metadata.setdefault('targeted_observed', []).append({**provenance, 'added_configs': after-before})
        temporary.replace(target)
        target.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
        results[name] = after-before
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seeds', type=Path, required=True)
    parser.add_argument('--stage', required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--log', type=Path, action='append', required=True)
    args = parser.parse_args()
    print(json.dumps(merge(args.seeds, args.stage, args.cache, args.log), indent=2))

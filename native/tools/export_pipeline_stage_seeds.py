#!/usr/bin/env python3
"""Export stage config databases from observed Aurora configs, with provenance.

Without --log every stage gets the conservative shared observed baseline.
With stage-attributed logs, include that stage's observed GX hashes plus clears.
This does not claim material replay or previously unseen configuration coverage.
"""
import argparse
import hashlib
import json
import re
import sqlite3
from pathlib import Path

STAGES = ['AstroGalaxy', 'EggStarGalaxy', 'ScenarioSelect', 'GalaxyMap']
PATTERN = re.compile(r'\[gx stage config\] stage=([A-Za-z0-9_-]+) config=([0-9a-fA-F]{16})\b')


def require(value, message):
    if not value:
        raise ValueError(message)


def export(source, output, stages, logs=()):
    source = source.resolve()
    seen = {}
    for log in logs:
        for stage, key in PATTERN.findall(log.read_text(errors='replace')):
            seen.setdefault(stage, set()).add(int(key, 16))
    with sqlite3.connect(source.as_uri() + '?mode=ro', uri=True) as db:
        schema = db.execute('SELECT value FROM aurora_schema').fetchall()
        rows = db.execute('SELECT type,hash,config_version,config_size,config,first_frame_used FROM pipeline_cache ORDER BY first_frame_used').fetchall()
    require(len(schema) == 1, 'expected one Aurora schema version')
    require(all(len(r[4]) == r[3] for r in rows), 'source contains invalid config sizes')
    output.mkdir(parents=True, exist_ok=True)
    results = {}
    for stage in stages:
        require(re.fullmatch(r'[A-Za-z0-9_-]{1,96}', stage), 'unsafe stage name')
        # ShaderType::Clear = 0; ShaderType::GX = 1. Keep clears for every stage.
        keys = seen.get(stage)
        require(not logs or keys, 'no attributed configs for requested stage: ' + stage)
        selected = [r for r in rows if r[0] == 0 or (r[0] == 1 and (not logs or (r[1] & ((1 << 64) - 1)) in keys))]
        target = output / (stage + '.db')
        require(target.resolve() != source, 'output would overwrite input database')
        temporary = output / (stage + '.db.tmp')
        require(not temporary.exists(), 'temporary output already exists: ' + str(temporary))
        with sqlite3.connect(temporary) as db:
            db.execute('CREATE TABLE aurora_schema(value INTEGER)')
            db.executemany('INSERT INTO aurora_schema VALUES (?)', schema)
            db.execute('CREATE TABLE pipeline_cache(type INTEGER NOT NULL, hash INTEGER NOT NULL, config_version INTEGER NOT NULL, config_size INTEGER NOT NULL, config BLOB NOT NULL, first_frame_used INTEGER NOT NULL, PRIMARY KEY(type,hash))')
            db.executemany('INSERT INTO pipeline_cache VALUES (?,?,?,?,?,?)', selected)
            db.commit()
        temporary.replace(target)
        found = {r[1] & ((1 << 64) - 1) for r in selected if r[0] == 1}
        provenance = {'schema': 1, 'stage': stage, 'kind': 'stage-observed' if logs else 'shared-observed-baseline',
                      'source': str(source), 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                      'logs': [str(p.resolve()) for p in logs], 'rows': len(selected),
                      'config_hashes': [f'{key:016x}' for key in sorted(found)],
                      'missing_logged_hashes': [f'{key:016x}' for key in sorted((keys or set()) - found)],
                      'offline_material_replay': False,
                      'limitations': 'Observed configs only; shared baseline is not evidence that each config belongs to this stage. Runtime target layouts are expanded when preparing.'}
        target.with_suffix('.json').write_text(json.dumps(provenance, indent=2) + '\n')
        results[stage] = len(selected)
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stage', action='append')
    parser.add_argument('--log', action='append', type=Path, default=[])
    args = parser.parse_args()
    for stage, count in export(args.cache, args.output, args.stage or STAGES, args.log).items():
        print(f'{stage}: {count} configs ({"stage-observed" if args.log else "shared-observed-baseline"})')

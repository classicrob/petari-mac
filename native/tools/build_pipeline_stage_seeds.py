#!/usr/bin/env python3
"""Replay disc archives on CPU, then merge stage dependencies with observed baseline."""
import argparse
from contextlib import closing
import hashlib
import json
import sqlite3
import shutil
import re
import subprocess
from pathlib import Path


def rows(path):
    with closing(sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)) as db:
        return db.execute('SELECT type,hash,config_version,config_size,config,first_frame_used FROM pipeline_cache').fetchall()


def run(args):
    args.work.mkdir(parents=True, exist_ok=True)
    binary_hash = hashlib.sha256(args.replay.read_bytes()).hexdigest()
    snapshot = args.work / ('replay-' + binary_hash)
    if not snapshot.exists(): shutil.copy2(args.replay, snapshot)
    archives = sorted(p for directory in ('ObjectData', 'StageData', 'LayoutData') for p in (args.files / directory).rglob('*.arc'))
    reports = {}
    for index, archive in enumerate(archives):
        relative = str(archive.relative_to(args.files))
        key = hashlib.sha256(relative.encode()).hexdigest()[:20]
        target, metadata = args.work / (key + '.db'), args.work / (key + '.json')
        external = [archive.parent / name for name in ('Mario.arc', 'Luigi.arc')] if archive.name == 'MarioAnime.arc' else []
        signature = ':'.join(hashlib.sha256(p.read_bytes()).hexdigest() for p in [archive, *external]) + ':' + binary_hash
        previous = json.loads(metadata.read_text()) if metadata.exists() else {}
        if previous.get('signature') == signature and previous.get('exit') == 0 and target.exists():
            reports[relative] = previous
            continue
        temporary, log = args.work / (key + '.tmp.db'), args.work / (key + '.log')
        if temporary.exists(): temporary.unlink()
        with log.open('w') as output:
            try:
                result = subprocess.run([str(snapshot.resolve()), str(archive.resolve()), str(temporary.resolve())],
                                        stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
        report = {'archive': relative, 'signature': signature, 'exit': code, 'database': str(target.resolve()),
                  'log': str(log.resolve()), 'external_model_archives': [str(p.relative_to(args.files)) for p in external]}
        report['counts'] = {k: int(v) for k, v in re.findall(r'(models|pairs|draws|configs|shader_variants)=(\d+)', log.read_text(errors='replace'))}
        if code == 0:
            temporary.replace(target)
            report['configs'] = len(rows(target))
        metadata.write_text(json.dumps(report, indent=2) + '\n')
        reports[relative] = report
        if code or index % 50 == 0:
            print(f'{index+1}/{len(archives)} {relative}: exit={code} configs={report.get("configs", 0)}', flush=True)
    (args.work / 'corpus.json').write_text(json.dumps(reports, indent=2) + '\n')
    failures = [r for r in reports.values() if r['exit']]
    if failures:
        print(f'archives={len(reports)} failed={len(failures)}; manifests not published', flush=True)
        return 1
    manifest = json.loads(args.inputs.read_text())
    baseline = rows(args.baseline)
    all_layouts = [key for key in reports if key.startswith('LayoutData/')]
    stages = {stage: [entry['path'] for entry in data['archives']] for stage, data in manifest['stages'].items()}
    # UI overlays have explicit layout archive mappings. Broad shared layouts are
    # intentionally included until runtime attribution permits tighter sets.
    for name in ('ScenarioSelect', 'GalaxyMap', 'FileSelect'):
        stages[name] = sorted(set(stages.get(name, [])) | set(all_layouts))
    stages['__global__'] = sorted(reports)
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {}
    for stage, dependencies in stages.items():
        combined = {(r[0], r[1]): r for r in baseline}
        failed, replayed = [], []
        for archive in dependencies:
            report = reports.get(archive)
            if report is None or report['exit']:
                failed.append(archive)
                continue
            replayed.append(archive)
            # Preserve observed first-use ordering; speculative alternatives follow
            # known draws when the runtime orders this manifest for preparation.
            for row in rows(Path(report['database'])):
                combined.setdefault((row[0], row[1]), (*row[:-1], (1 << 32) - 1))
        target = args.output / (stage + '.db')
        temporary = target.with_suffix('.tmp.db')
        if temporary.exists(): temporary.unlink()
        with closing(sqlite3.connect(temporary)) as db, db:
            db.executescript('CREATE TABLE aurora_schema(value INTEGER); INSERT INTO aurora_schema VALUES(1); '
                            'CREATE TABLE pipeline_cache(type INTEGER, hash INTEGER, config_version INTEGER, config_size INTEGER, config BLOB, first_frame_used INTEGER, PRIMARY KEY(type,hash));'
                            'CREATE INDEX pipeline_cache_load_order_idx ON pipeline_cache(first_frame_used);')
            db.executemany('INSERT INTO pipeline_cache VALUES(?,?,?,?,?,?)', combined.values())
        temporary.replace(target)
        provenance = {'stage': stage, 'kind': 'native-display-list-replay-plus-observed', 'configs': len(combined),
                      'baseline_configs': len(baseline), 'archives': replayed, 'failed_archives': failed,
                      'replay_binary_sha256': binary_hash, 'baseline': str(args.baseline.resolve()),
                      'baseline_sha256': hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
                      'input_manifest_sha256': hashlib.sha256(args.inputs.read_bytes()).hexdigest(),
                      'archive_signatures': {name: reports[name]['signature'] for name in replayed},
                      'limitations': ['Static scenario/zone and factory archive mapping; dynamic callbacks may load additional archives.',
                                      'Runtime material mutations, particles, and procedural game draws remain covered only by observed baseline.',
                                      'Shader source generation checks do not compile Metal or establish visual correctness.']}
        target.with_suffix('.json').write_text(json.dumps(provenance, indent=2) + '\n')
        summary[stage] = {'configs': len(combined), 'replayed_archives': len(replayed), 'failed_archives': failed}
    (args.work / 'stages.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'archives={len(reports)} failed={len(failures)} stage_manifests={len(stages)}', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files', type=Path, required=True)
    parser.add_argument('--replay', type=Path, default=Path('build/macos-gx/native/gx/petari_pipeline_replay'))
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--work', type=Path, default=Path('build/pipeline-replay-corpus'))
    parser.add_argument('--output', type=Path, default=Path('build/pipeline-seeds'))
    parser.add_argument('--timeout', type=int, default=60)
    raise SystemExit(run(parser.parse_args()))

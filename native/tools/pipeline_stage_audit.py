#!/usr/bin/env python3
"""Join stage-attributed runtime first uses to retained caches and current seeds.

Cache rows alone are not draws: imported/speculative rows never enter the observed
set without a matching first-use log record. Reports are coverage of recorded
routes, including incomplete runs, not all possible states of a stage.
"""
import argparse
from collections import defaultdict
from contextlib import closing
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sqlite3

MASK = (1 << 64) - 1
FIELDS = re.compile(r'(\w+)=([^\s]+)')


def hashes(path):
    with closing(sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True, timeout=1)) as db:
        return {f'{key & MASK:016x}' for key, in db.execute('SELECT hash FROM pipeline_cache WHERE type=1')}


def stage_records(text):
    stages = defaultdict(lambda: {'all': set(), 'after_prep': set(), 'logged_uncovered': set()})
    for line in text.splitlines():
        if '[gx stage config]' not in line:
            continue
        fields = dict(FIELDS.findall(line))
        if fields.get('overlay_tag', '0') != '0' or fields.get('stage') in (None, 'Unknown'):
            continue
        key = fields.get('config', '').lower()
        if not re.fullmatch('[0-9a-f]{16}', key):
            continue
        entry = stages[fields['stage']]
        entry['all'].add(key)
        if fields.get('phase') == 'after_prep':
            entry['after_prep'].add(key)
        if fields.get('covered') == '0':
            entry['logged_uncovered'].add(key)
    return stages


def audit(seeds, pairs, stage_names=(), corpus=None):
    seed_sets = {}
    seed_errors = {}
    for path in sorted(seeds.glob('*.db')):
        try:
            seed_sets[path.stem] = hashes(path)
        except sqlite3.Error as error:
            seed_errors[path.stem] = str(error)
    metadata_errors = {}

    def metadata(path):
        if not path.exists():
            return {}
        try:
            return json.loads(path.read_text())
        except (OSError, ValueError) as error:
            metadata_errors[str(path)] = str(error)
            return {}

    global_keys = seed_sets.get('__global__', set())
    observed = defaultdict(set)
    after = defaultdict(set)
    sources = defaultdict(list)
    runs, excluded = [], []
    for log, cache in sorted(set(pairs)):
        if not cache.is_file():
            excluded.append({'log': str(log), 'reason': 'no retained pipeline cache'})
            continue
        try:
            before = log.stat()
            contents = log.read_bytes()
            records = stage_records(contents.decode('utf-8', errors='replace'))
            cached = hashes(cache)
            changed = (before.st_size, before.st_mtime_ns) != (log.stat().st_size, log.stat().st_mtime_ns)
        except (OSError, sqlite3.Error) as error:
            excluded.append({'log': str(log), 'reason': str(error)})
            continue
        if not records:
            excluded.append({'log': str(log), 'reason': 'no stage-attributed first-use records'})
            continue
        result_path = log.parent / 'result.json'
        result = metadata(result_path)
        record = {'log': str(log), 'cache': str(cache), 'log_sha256': hashlib.sha256(contents).hexdigest(),
                  'log_changed_during_read': changed, 'smoke_pass': b'PETARI SMOKE RESULT: PASS' in contents,
                  'result_status': result.get('outcome', result.get('smoke_result')), 'stages': {}}
        for stage, entries in records.items():
            captured = entries['all'] & cached
            observed[stage].update(captured)
            after[stage].update(entries['after_prep'] & cached)
            sources[stage].append(str(log))
            stage_keys = seed_sets.get(stage, set())
            record['stages'][stage] = {
                'logged': len(entries['all']), 'cache_confirmed': len(captured),
                'not_in_retained_cache': sorted(entries['all'] - cached),
                'logged_uncovered': len(entries['logged_uncovered']),
                'missing_stage': sorted(captured - stage_keys), 'missing_global': sorted(captured - global_keys)}
        runs.append(record)
    reports = []
    for stage in sorted(set(stage_names) | set(observed)):
        keys = observed.get(stage, set())
        stage_keys = seed_sets.get(stage, set())
        missing = keys - stage_keys
        reports.append({'stage': stage, 'observed': len(keys), 'post_prep_observed': len(after.get(stage, set())),
                        'stage_manifest_exists': stage in seed_sets,
                        'stage_covered': len(keys & stage_keys), 'global_covered': len(keys & global_keys),
                        'stage_coverage_pct': 100 * len(keys & stage_keys) / len(keys) if keys else None,
                        'global_coverage_pct': 100 * len(keys & global_keys) / len(keys) if keys else None,
                        'missing_stage': sorted(missing), 'missing_global': sorted(keys - global_keys),
                        'post_prep_missing_stage': sorted(after.get(stage, set()) - stage_keys),
                        'post_prep_missing_global': sorted(after.get(stage, set()) - global_keys),
                        'logs': sources.get(stage, [])})
    missing_any = {key for stage in reports for key in stage['missing_stage']}
    archive_sources = defaultdict(list)
    if corpus:
        for archive, entry in metadata(corpus).items():
            path = Path(entry.get('database', ''))
            if entry.get('exit') != 0 or not path.is_file():
                continue
            try:
                matching = hashes(path) & missing_any
            except (OSError, sqlite3.Error) as error:
                metadata_errors[str(path)] = str(error)
                continue
            for key in matching:
                archive_sources[key].append(archive)
    for report in reports:
        manifest = seeds / (report['stage'] + '.json')
        provenance = metadata(manifest)
        dependencies = set(provenance.get('archives', []))
        report['missing_stage_replay_sources'] = {key: archive_sources[key] for key in report['missing_stage'] if key in archive_sources}
        report['missing_stage_known_other_replay_archives'] = {
            key: [a for a in archive_sources[key] if a not in dependencies]
            for key in report['missing_stage'] if key in archive_sources}
    return {'generated_at_utc': datetime.now(timezone.utc).isoformat(), 'seed_directory': str(seeds),
            'global_configs': len(global_keys), 'seed_read_errors': seed_errors, 'metadata_errors': metadata_errors, 'runs': runs, 'excluded': excluded,
            'stages': reports, 'below_95': [r['stage'] for r in reports if r['observed'] and
                (r['stage_coverage_pct'] < 95 or r['global_coverage_pct'] < 95)],
            'unobserved': [r['stage'] for r in reports if not r['observed']],
            'note': 'Denominator is unique logged stage/config pairs confirmed in a retained GX cache, all recorded phases. '
                    'Imported cache-only rows are excluded. Missing cache rows and absent instrumentation are reported, '
                    'not counted as coverage. Incomplete runs are observations, not smoke passes. No whole-mission completeness claim.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seeds', type=Path, default=Path('build/pipeline-seeds'))
    parser.add_argument('--sweep-root', type=Path, default=Path('build/stage-sweep'))
    parser.add_argument('--extra', type=Path, nargs=2, action='append', default=[], metavar=('LOG', 'CACHE'))
    parser.add_argument('--corpus', type=Path, default=Path('build/pipeline-replay-corpus/corpus.json'))
    parser.add_argument('--stage-data', type=Path, default=Path('build/game-data/RMGE01/files/StageData'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pairs = [(log, log.parent / 'user/pipeline_cache.db') for log in args.sweep_root.rglob('app.log')]
    pairs += [(log, cache) for log, cache in args.extra]
    names = {p.parent.name for p in args.stage_data.glob('*/*Scenario.arc')}
    result = audit(args.seeds, pairs, names, args.corpus)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"runs={len(result['runs'])} excluded={len(result['excluded'])} observed_stages={sum(bool(x['observed']) for x in result['stages'])} global_configs={result['global_configs']}")
    for entry in result['stages']:
        if entry['stage'] in result['below_95']:
            print(f"{entry['stage']}: n={entry['observed']} stage={entry['stage_coverage_pct']:.2f}% global={entry['global_coverage_pct']:.2f}% stage_missing={len(entry['missing_stage'])} global_missing={len(entry['missing_global'])}")
    print('unobserved:', ', '.join(result['unobserved']))


if __name__ == '__main__':
    main()

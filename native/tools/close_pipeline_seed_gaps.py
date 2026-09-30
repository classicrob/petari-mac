#!/usr/bin/env python3
"""Build an unpublished seed candidate from replay dependencies and PASS evidence."""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import shutil
import sqlite3

from build_pipeline_stage_seeds import rows
from merge_pipeline_observations import merge
from pipeline_stage_audit import audit, hashes, stage_records


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def eligible(result):
    return (result.get('smoke_result') == 'PASS' and result.get('exit_status') == 0
            and result.get('timed_out') is False and not result.get('assisted_inputs')
            and result.get('outcome') in ('PASS', 'PASS_WARN')
            and not result.get('renderer_error_count') and not result.get('crash')
            and not result.get('heap_failure') and not result.get('os_fatal'))


def add_replay_rows(target, additions):
    with closing(sqlite3.connect(target)) as db, db:
        expected = set(db.execute('SELECT DISTINCT config_version,config_size FROM pipeline_cache WHERE type=1'))
        before = db.execute('SELECT count(*) FROM pipeline_cache').fetchone()[0]
        for row in additions:
            if row[0] != 1:
                continue
            if (row[2], row[3]) not in expected or len(row[4]) != row[3]:
                raise ValueError('config ABI mismatch: ' + str(target))
            existing = db.execute('SELECT config FROM pipeline_cache WHERE type=? AND hash=?', row[:2]).fetchone()
            if existing and existing[0] != row[4]:
                raise ValueError('same hash with different bytes')
            db.execute('INSERT OR IGNORE INTO pipeline_cache VALUES(?,?,?,?,?,?)', (*row[:-1], (1 << 32) - 1))
        return db.execute('SELECT count(*) FROM pipeline_cache').fetchone()[0] - before


def close(args):
    if args.output.exists():
        raise ValueError('candidate output already exists; do not overwrite')
    shutil.copytree(args.source, args.output)
    inputs = json.loads(args.inputs.read_text())
    corpus = json.loads(args.corpus.read_text())
    before = json.loads(args.audit.read_text())
    changes = {'dependency_additions': {}, 'observed_merges': [], 'excluded_runs': [],
               'before_audit_sha256': digest(args.audit), 'input_sha256': digest(args.inputs),
               'corpus_sha256': digest(args.corpus), 'published': False}
    for stage, entry in inputs['stages'].items():
        target = args.output / (stage + '.db')
        if not target.is_file():
            raise ValueError('stage seed missing: ' + stage)
        metadata = json.loads(target.with_suffix('.json').read_text())
        previous = set(metadata['archives'])
        extra = [x for x in entry['archives'] if x['path'] not in previous]
        additions, signatures = [], {}
        for archive in extra:
            replay = corpus.get(archive['path'])
            if not replay or replay['exit'] != 0:
                raise ValueError('dependency lacks successful replay: ' + archive['path'])
            additions.extend(rows(Path(replay['database'])))
            signatures[archive['path']] = replay['signature']
        count = add_replay_rows(target, additions)
        add_replay_rows(args.output / '__global__.db', additions)
        metadata['archives'] = sorted(previous | {x['path'] for x in extra})
        metadata.setdefault('archive_signatures', {}).update(signatures)
        metadata['dependency_closure'] = {
            'kind': 'factory-tables-plus-conservative-actor-callback-literals-and-stationed-resources',
            'input_manifest': str(args.inputs.resolve()), 'input_sha256': changes['input_sha256'],
            'source_sha256': inputs['dependency_source_sha256'],
            'added_archives': extra, 'added_configs': count,
            'unresolved_callbacks': entry.get('unresolved_archive_callbacks', []),
            'limitations': ['Conditional archive branches are unioned. Constructed names and indirect calls are not exhaustively evaluated.']}
        with closing(sqlite3.connect(target)) as db:
            metadata['configs'] = db.execute('SELECT count(*) FROM pipeline_cache').fetchone()[0]
        target.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
        changes['dependency_additions'][stage] = {'archives': len(extra), 'configs': count}

    seed_keys = {p.stem: hashes(p) for p in args.output.glob('*.db')}
    passed_pairs = []
    for record in before['runs']:
        log, cache = Path(record['log']), Path(record['cache'])
        result_path = log.parent / 'result.json'
        result = json.loads(result_path.read_text()) if result_path.is_file() else {}
        if not eligible(result):
            changes['excluded_runs'].append({'log': str(log), 'reason': 'requires unassisted PASS smoke, exit0, no render/crash/heap errors', 'outcome': result.get('outcome')})
            continue
        if digest(log) != record['log_sha256']:
            changes['excluded_runs'].append({'log': str(log), 'reason': 'log changed since audit; excluded rather than mixing observations'})
            continue
        passed_pairs.append((log, cache))
        for stage, entry in stage_records(log.read_text(errors='replace')).items():
            if not ((entry['all'] - seed_keys.get(stage, set())) | (entry['all'] - seed_keys['__global__'])):
                continue
            validation = {'result': str(result_path.resolve()), 'result_sha256': digest(result_path),
                          'smoke_result': result['smoke_result'], 'outcome': result['outcome'],
                          'exit_status': result['exit_status'], 'app_sha256': result.get('app_sha256'),
                          'rule': 'PASS smoke; exit0; no timeout, assisted input, renderer errors or crash'}
            counts = merge(args.output, stage, cache, [log], validation=validation)
            changes['observed_merges'].append({'stage': stage, 'log': str(log), 'added': counts})
            seed_keys[stage].update(entry['all'])
            seed_keys['__global__'].update(entry['all'])

    with closing(sqlite3.connect(args.output / '__global__.db')) as db:
        global_rows = {(r[0], r[1]): (r[2], r[3], r[4]) for r in rows(args.output / '__global__.db')}
    for path in args.output.glob('*.db'):
        with closing(sqlite3.connect(path)) as db:
            if db.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise ValueError('integrity failure: ' + str(path))
            for row in rows(path):
                if global_rows.get((row[0], row[1])) != (row[2], row[3], row[4]):
                    raise ValueError('stage row not in global union: ' + str(path))
    all_pairs = [(Path(r['log']), Path(r['cache'])) for r in before['runs']]
    after = audit(args.output, all_pairs, [s['stage'] for s in before['stages']], args.corpus)
    passed = audit(args.output, passed_pairs, corpus=args.corpus)
    if after['seed_read_errors'] or after['metadata_errors'] or passed['excluded']:
        raise ValueError('candidate audit has missing evidence')
    if any(s['missing_stage'] or s['missing_global'] for s in passed['stages']):
        raise ValueError('PASS observations remain uncovered')
    changes['pass_evidence_runs'] = len(passed_pairs)
    changes['global_gx_configs'] = len(seed_keys['__global__'])
    for name, data in [('changes.json', changes), ('after.json', after), ('pass-only-after.json', passed)]:
        (args.output.parent / name).write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps({'global_gx_configs': changes['global_gx_configs'],
                      'pass_runs': len(passed_pairs), 'observed_merges': len(changes['observed_merges']),
                      'below95_after': after['below_95']}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--audit', type=Path, required=True)
    parser.add_argument('--corpus', type=Path, default=Path('build/pipeline-replay-corpus/corpus.json'))
    close(parser.parse_args())

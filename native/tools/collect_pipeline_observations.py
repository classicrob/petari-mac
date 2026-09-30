#!/usr/bin/env python3
"""Snapshot completed PASS survey/sweep evidence for an offline seed merge.

Does not launch the game or modify source runs. Prefix-only executable evidence
is recorded as excluded rather than promoted to a full executable identity.
"""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import re
import sqlite3

from close_pipeline_seed_gaps import eligible
from stage_sweep import analyze


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def survey_identity(entry):
    if not (entry.get('clean_pass') is True and entry.get('exit_status') == 0
            and entry.get('smoke_result') == 'PASS' and entry.get('set_aside') is False
            and entry.get('watchdog_or_abort') is False):
        raise ValueError('survey lacks clean completed PASS evidence')
    frozen = entry.get('app_frozen_copy') or {}
    expected = frozen.get('sha256', '')
    if not re.fullmatch('[0-9a-f]{64}', expected):
        raise ValueError('only historical executable prefix available')
    binary = Path(frozen['path']).read_bytes()
    if hashlib.sha256(binary).hexdigest() != expected:
        raise ValueError('frozen executable SHA256 changed')
    prefix = entry.get('app_sha1_prefix', '')
    if not re.fullmatch('[0-9a-f]{12}', prefix) or not hashlib.sha1(binary).hexdigest().startswith(prefix):
        raise ValueError('frozen executable does not match launch SHA1 prefix')
    return expected


def snapshot(output, name, log, cache, csv, user, stage, result, evidence):
    if not re.fullmatch('[A-Za-z0-9_.-]+', name) or name in ('.', '..'):
        raise ValueError('unsafe snapshot name')
    if not eligible(result):
        raise ValueError('result is not an unassisted renderer-clean PASS')
    if not re.fullmatch('[0-9a-f]{64}', result.get('app_sha256') or ''):
        raise ValueError('full recorded executable SHA256 missing')
    if not cache.is_file():
        raise ValueError('retained cache missing')
    original = log.read_bytes()
    parsed = analyze(log, csv, stage, result['exit_status'], result['timed_out'], user)
    if not eligible(parsed):
        raise ValueError('reparsed log is not an unassisted renderer-clean PASS: ' + parsed['outcome'])
    directory = output / 'runs' / name
    directory.mkdir(parents=True, exist_ok=False)
    (directory / 'user').mkdir()
    with closing(sqlite3.connect(cache.resolve().as_uri() + '?mode=ro', uri=True)) as source:
        with closing(sqlite3.connect(directory / 'user/pipeline_cache.db')) as target:
            source.backup(target)
            if target.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise ValueError('snapshot cache integrity failure')
    if log.read_bytes() != original:
        raise ValueError('log changed during snapshot')
    for path, expected in evidence['source_files_sha256'].items():
        if digest(Path(path)) != expected:
            raise ValueError('source provenance changed during snapshot: ' + path)
    parsed.update(app_sha256=result['app_sha256'], scenario=result.get('scenario'),
                  source_evidence=evidence,
                  original_log=str(log.resolve()), original_log_sha256=hashlib.sha256(original).hexdigest(),
                  original_cache=str(cache.resolve()),
                  snapshot_cache_sha256=digest(directory / 'user/pipeline_cache.db'))
    (directory / 'result.json').write_text(json.dumps(parsed, indent=2) + '\n')
    (directory / 'app.log').write_bytes(original)
    return {'name': name, 'stage': stage, 'result': str(directory / 'result.json'),
            'source_log': str(log), 'app_sha256': result['app_sha256']}


def collect(args):
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'accepted': [], 'excluded': [], 'rule': 'Completed clean PASS; full executable identity; reparsed logs; stable evidence snapshot'}

    def attempt(name, action):
        try:
            report['accepted'].append(action())
        except (ValueError, OSError, sqlite3.Error, KeyError) as error:
            report['excluded'].append({'run': name, 'reason': str(error)})

    manifest_bytes = args.survey.read_bytes()
    manifest = json.loads(manifest_bytes)
    manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
    for entry in manifest['runs']:
        name = 'survey-' + entry['run']

        def survey(entry=entry, name=name):
            identity = survey_identity(entry)
            result = {'smoke_result': 'PASS', 'exit_status': 0, 'timed_out': False,
                      'outcome': 'PASS', 'app_sha256': identity, 'scenario': entry['scenario']}
            evidence = {'kind': 'telemetry-survey', 'manifest_entry': entry,
                        'command_template': manifest['command_template'],
                        'fixed_environment': manifest['fixed_environment'],
                        'source_files_sha256': {str(args.survey.resolve()): manifest_hash,
                                               str(Path(__file__).resolve()): digest(Path(__file__)),
                                               str(Path('native/tools/stage_sweep.py').resolve()): digest(Path('native/tools/stage_sweep.py'))}}
            user = Path(entry['user_dir'])
            return snapshot(args.output, name, Path(entry['log']), user / 'pipeline_cache.db',
                            Path(entry['frames_csv']), user, entry['stage'], result, evidence)

        attempt(name, survey)
    for result_path in sorted(args.sweep_root.glob('runs/*/result.json')):
        name = 'extra-' + result_path.parent.name

        def sweep(result_path=result_path, name=name):
            raw = result_path.read_bytes()
            result = json.loads(raw)
            directory = result_path.parent
            evidence = {'kind': 'stage-sweep', 'recorded_result': result,
                        'source_files_sha256': {str(result_path.resolve()): hashlib.sha256(raw).hexdigest(),
                                               str(Path(__file__).resolve()): digest(Path(__file__)),
                                               str(Path('native/tools/stage_sweep.py').resolve()): digest(Path('native/tools/stage_sweep.py'))}}
            return snapshot(args.output, name, directory / 'app.log', directory / 'user/pipeline_cache.db',
                            directory / 'frames.csv', directory / 'user', result['stage'], result, evidence)

        attempt(name, sweep)
    (args.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'accepted': len(report['accepted']), 'excluded': len(report['excluded'])}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--survey', type=Path, required=True)
    parser.add_argument('--sweep-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    collect(parser.parse_args())

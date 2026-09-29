#!/usr/bin/env python3
"""Capture timestamped pipeline diagnostics and host load for an isolated galaxy smoke.

Run via locked-app.sh -> locked-sweep-lane.sh -> locked-build.sh.
Existing app-only tickets acquire the remaining locks before measurement starts.
The supplied user directory must already be a marked observatory fixture.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import signal
import subprocess
import sys
import threading
import time


def percentile(values, fraction):
    values = sorted(values)
    if not values: return None
    return values[min(len(values) - 1, int((len(values) - 1) * fraction + .5))]


def distribution(values):
    return {'count': len(values), 'total': sum(values), 'p50': percentile(values, .5),
            'p95': percentile(values, .95), 'p99': percentile(values, .99), 'max': max(values, default=0)}


def summarize(events, loads):
    compiles, blocking, stages = [], [], []
    stage = 'startup'
    for event in events:
        line = event['line']
        fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
        if '[gx stage prep]' in line:
            stage = fields.get('stage', stage)
        if '[gx pipeline compile]' in line:
            compiles.append(dict(fields, elapsed_s=event['elapsed_s'], stage=stage))
        if '[gx stage prep]' in line or '[gx stage ready]' in line or '[gx warmup]' in line or '[gx global prep]' in line:
            stages.append(event)
        if '[gx pipeline] blocking resolve' in line:
            match = re.search(r'blocking resolve ([\d.]+) ms', line)
            if match: blocking.append({'stage': stage, 'elapsed_s': event['elapsed_s'], 'ms': float(match[1]), 'line': line})
    return {'stage_events': stages, 'blocking_resolves': blocking,
            'blocking_ms': distribution([entry['ms'] for entry in blocking]),
            'compile_ms': {field: distribution([float(entry[field]) for entry in compiles if field in entry])
                           for field in ['queue_ms', 'requested_queue_ms', 'build_ms', 'wgsl_ms', 'module_ms', 'pipeline_api_ms']},
            'compiles': compiles, 'load_1m': distribution([entry['load'][0] for entry in loads])}


def require_quiet_locks():
    ancestors = set()
    pid = os.getppid()
    while pid > 1 and pid not in ancestors:
        ancestors.add(pid)
        parent = subprocess.check_output(['ps', '-o', 'ppid=', '-p', str(pid)], text=True).strip()
        if not parent: break
        pid = int(parent)
    def held(name):
        path = Path('build') / name / 'pid'
        return path.is_file() and int(path.read_text()) in ancestors
    if not held('.petari-app.lock'):
        raise SystemExit('Timing runs must hold locked-app.sh first')
    command = [sys.executable, *sys.argv]
    if not held('.petari-sweep-lane.lock'):
        if held('.macos-gx.lock'):
            raise SystemExit('Wrong lock order: app -> sweep lane -> build required')
        command = ['build/locked-sweep-lane.sh', 'pipeline-quiet',
                   'build/locked-build.sh', 'pipeline-quiet', *command]
    elif not held('.macos-gx.lock'):
        command = ['build/locked-build.sh', 'pipeline-quiet', *command]
    else:
        return
    os.execv(command[0], command)


def run(args):
    require_quiet_locks()
    if args.previous_result:
        previous = json.loads(args.previous_result.read_text())
        if previous['exit'] or previous['timeout'] or previous.get('aborted') or not previous.get('smoke_pass'):
            raise SystemExit('Previous cold run did not pass; refusing to label a rerun warm')
    if (args.user / '.petari-test-fixture').read_text().strip() != 'observatory': raise SystemExit('Expected observatory fixture')
    args.output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.update(PETARI_SMOKE='galaxy', PETARI_SMOKE_FRAMES='108000', PETARI_TRACE_BOOT='1',
               PETARI_AUDIO_DIAG='1', PETARI_PIPELINE_DIAG='1', PETARI_FRAME_CSV=str((args.output / 'frames.csv').resolve()))
    env.pop('PETARI_PIPELINE_SEED_DIR', None)
    exe = args.app / 'Contents/MacOS/Petari'
    command = [str(exe.resolve()), '--disc', str(args.disc.resolve()), '--user', str(args.user.resolve()), '--test-fixture', 'observatory']
    (args.output / 'run.json').write_text(json.dumps({'command': command, 'binary_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'app_cache_files_before': [str(p.relative_to(args.user)) for p in args.user.rglob('*') if p.is_file() and 'cache' in p.name],
        'lock_protocol': 'app -> sweep lane -> build',
        'environment': {key: value for key, value in env.items() if key.startswith('PETARI_')},
        'note': 'OS Metal cache is not cleared; app caches are isolated. Compile stage is temporal context, not draw ownership.'}, indent=2))
    started = time.monotonic(); events, loads = [], []; done = threading.Event(); expired = []
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1)
    def sample():
        with (args.output / 'host-load.jsonl').open('w') as output:
            while not done.is_set():
                elapsed = time.monotonic() - started
                item = {'elapsed_s': elapsed, 'load': os.getloadavg()}
                loads.append(item); output.write(json.dumps(item) + '\n'); output.flush()
                if elapsed > args.timeout and process.poll() is None:
                    expired.append(True); process.terminate()
                    if not done.wait(5) and process.poll() is None: process.kill()
                done.wait(2)
    sampler = threading.Thread(target=sample); sampler.start()
    with (args.output / 'app.log').open('w') as raw, (args.output / 'events.jsonl').open('w') as timed:
        for line in process.stdout:
            raw.write(line); raw.flush()
            event = {'elapsed_s': time.monotonic() - started, 'line': line.rstrip('\n')}
            timed.write(json.dumps(event) + '\n'); timed.flush(); events.append(event)
    code = process.wait(); done.set(); sampler.join()
    result = summarize(events, loads)
    result.update(exit=code, timeout=bool(expired), elapsed_s=time.monotonic()-started)
    result['smoke_pass'] = any('PETARI SMOKE RESULT: PASS' in event['line'] for event in events)
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    result['child_cpu_s'] = usage.ru_utime + usage.ru_stime
    result['child_maxrss_native_units'] = usage.ru_maxrss
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key not in ('compiles', 'blocking_resolves')}, indent=2))
    return code if code else (1 if expired or not result['smoke_pass'] else 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--user', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disc', type=Path, default=Path('build/game-data/RMGE01'))
    parser.add_argument('--timeout', type=int, default=1200)
    parser.add_argument('--previous-result', type=Path, help='require a successful prior cold run before launching warm')
    raise SystemExit(run(parser.parse_args()))

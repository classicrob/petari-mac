#!/usr/bin/env python3
"""Bounded real-window playtest launcher. Input comes only from macOS/CUA.

Prepare once, then run --session 1 (and --session 2 to reuse the isolated save).
The script always acquires locked-app.sh before launching Petari.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
import re
from pathlib import Path
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/cu-playtest'
FIXTURE = OUT / 'user'
BINARY = ROOT / 'build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari'


def utc():
    return datetime.now(timezone.utc).isoformat()


def hashes():
    return {str(p.relative_to(FIXTURE)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((FIXTURE / 'NAND').rglob('*')) if p.is_file()}


def summarize():
    results = []
    for metadata in sorted(OUT.glob('session-[0-9]*.json')):
        if not re.fullmatch(r'session-\d+\.json', metadata.name):
            continue
        record = json.loads(metadata.read_text())
        prefix = metadata.with_suffix('')
        log = prefix.with_suffix('.log').read_text(errors='replace')
        csv_path = prefix.with_suffix('.csv')
        rows = list(csv.DictReader(csv_path.open())) if csv_path.exists() else []
        stats = {}
        for phase in sorted({row['phase'] for row in rows}):
            values = sorted(float(row['interval_ms']) for row in rows if row['phase'] == phase)
            stats[phase] = {'frames': len(values), 'p50_ms': values[len(values)//2],
                            'p95_ms': values[int(len(values)*.95)], 'max_ms': max(values),
                            'over_33_3_ms': sum(value > 33.3 for value in values)}
        result = {'session': prefix.name, 'metadata': record, 'frames': len(rows), 'phase_stats': stats,
                  'audio_reports': log.count('[audio] level'),
                  'audio_underrun_frames': sum(map(int, re.findall(r'underrun (\d+) frames', log))),
                  'audio_replayed_blocks': sum(map(int, re.findall(r'AI replayed (\d+) blocks', log))),
                  'missing_layout_lines': sum('[layout] missing ' in line for line in log.splitlines()),
                  'missing_sound_lines': sum('[sound] missing ' in line for line in log.splitlines()),
                  'nand_unchanged': record.get('nand_before') == record.get('nand_after')}
        results.append(result)
    (OUT / 'analysis.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--summarize', action='store_true', help='Summarize recorded sessions without launching')
    parser.add_argument('--app', type=Path, default=BINARY.parents[2], help='Frozen .app bundle to run')
    parser.add_argument('--session', type=int, default=1)
    parser.add_argument('--seconds', type=int, default=1200)
    parser.add_argument('--inside-lock', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--inside-sweep', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 1200 or args.session < 1:
        parser.error('session must be positive; duration must be 1..1200 seconds')
    OUT.mkdir(exist_ok=True)
    if args.summarize:
        summarize()
        return
    if args.prepare:
        subprocess.run([sys.executable, str(ROOT / 'native/tools/create_observatory_fixture.py'),
                        '--source', str(ROOT / 'build/observatory-user-2'), '--output', str(FIXTURE)], check=True)
        return
    if not FIXTURE.is_dir() or (FIXTURE / '.petari-test-fixture').read_text().strip() != 'observatory':
        parser.error('run --prepare first; an explicitly marked isolated fixture is required')
    if not args.inside_lock:
        env = dict(os.environ, PETARI_LOCK_TIMEOUT='2700')
        result = subprocess.run([str(ROOT / 'build/locked-app.sh'), 'cu-playtest', sys.executable,
                                 str(Path(__file__).resolve()), '--inside-lock', '--session', str(args.session),
                                 '--seconds', str(args.seconds), '--app', str(args.app.resolve())], cwd=ROOT, env=env)
        raise SystemExit(result.returncode)
    lock = ROOT / 'build/.petari-app.lock'
    if (lock / 'owner').read_text().strip() != 'cu-playtest':
        parser.error('inside-lock mode requires the cu-playtest app lock')
    if not args.inside_sweep:
        if int((lock / 'pid').read_text()) != os.getppid():
            parser.error('inside-lock mode requires this launcher’s locked-app parent')
        result = subprocess.run([str(ROOT / 'build/locked-sweep-lane.sh'), 'cu-playtest', sys.executable,
                                 str(Path(__file__).resolve()), '--inside-lock', '--inside-sweep',
                                 '--session', str(args.session), '--seconds', str(args.seconds),
                                 '--app', str(args.app.resolve())], cwd=ROOT)
        raise SystemExit(result.returncode)
    lane = ROOT / 'build/.petari-sweep-lane.lock'
    if (lane / 'owner').read_text().strip() != 'cu-playtest' or int((lane / 'pid').read_text()) != os.getppid():
        parser.error('inside-sweep mode requires this launcher’s sweep-lane parent')
    prefix = OUT / f'session-{args.session}'
    log = prefix.with_suffix('.log')
    if log.exists():
        parser.error('session log already exists; choose the next session number')
    env = {k: v for k, v in os.environ.items() if not k.startswith('PETARI_SMOKE')}
    env.update(PETARI_TRACE_BOOT='1', PETARI_AUDIO_DIAG='1', PETARI_BATON_DIAG='1',
               PETARI_BATON_SAMPLE='0', PETARI_PIPELINE_GLOBAL_PRECOMPILE='0', PETARI_FRAME_CSV=str(prefix.with_suffix('.csv')))
    binary = args.app.resolve() / 'Contents/MacOS/Petari'
    command = [str(binary), '--disc', str(ROOT / 'build/game-data/RMGE01'),
               '--user', str(FIXTURE), '--test-fixture', 'observatory']
    record = {'started_utc': utc(), 'command': command, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'app': str(args.app.resolve()), 'diagnostics': {k:v for k,v in env.items() if k in ('PETARI_TRACE_BOOT','PETARI_AUDIO_DIAG','PETARI_BATON_DIAG','PETARI_BATON_SAMPLE','PETARI_PIPELINE_GLOBAL_PRECOMPILE','PETARI_FRAME_CSV')},
              'nand_before': hashes(), 'internal_smoke_driver': False, 'deadline_seconds': args.seconds}
    started = time.monotonic()
    with log.open('x') as output:
        child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        record['pid'] = child.pid
        (OUT / 'active-session.json').write_text(json.dumps(record, indent=2) + '\n')
        prefix.with_suffix('.json').write_text(json.dumps(record, indent=2) + '\n')
        stopped = False
        def stop(signum, frame):
            nonlocal stopped
            stopped = True
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        sample_taken = False
        last_size, unchanged_since = 0, time.monotonic()
        try:
            while child.poll() is None:
                now = time.monotonic()
                size = log.stat().st_size
                if size != last_size:
                    last_size, unchanged_since = size, now
                if now - unchanged_since > 45 and not sample_taken:
                    subprocess.run(['/usr/bin/sample', str(child.pid), '3', '-file', str(prefix) + '-stall-sample.txt'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=8)
                    sample_taken = True
                if stopped or now - started >= args.seconds:
                    record['forced_stop'] = 'signal' if stopped else '20-minute deadline'
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                    break
                time.sleep(0.5)
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGKILL)
            record.update(exit_code=child.wait(), ended_utc=utc(), elapsed_seconds=round(time.monotonic()-started, 3), nand_after=hashes())
            prefix.with_suffix('.json').write_text(json.dumps(record, indent=2) + '\n')
            (OUT / 'active-session.json').unlink(missing_ok=True)
    print(json.dumps(record, indent=2))
    raise SystemExit(0 if record['exit_code'] == 0 and 'forced_stop' not in record else 1)


if __name__ == '__main__':
    main()

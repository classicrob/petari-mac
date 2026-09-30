#!/usr/bin/env python3
"""Estimate stage target completion from compile logs and their exact seed DBs.

This deliberately refuses ambiguous/missing runtime variants. Timestamps are
log receipt times, not exact compiler publication times. Additive overlay unions
are reported separately rather than silently folded into a stage gate.
"""
import argparse
from collections import defaultdict
from contextlib import closing
import json
from pathlib import Path
import re
import sqlite3


def report(result_path):
    data = json.loads(result_path.read_text())
    compiled = defaultdict(list)
    for row in data['compiles']:
        compiled[row['config']].append(row)
    clear = {key for key, rows in compiled.items() if rows[0]['type'] == '0'}
    output = []
    for event in data['stage_events']:
        if '[gx stage prep]' not in event['line']: continue
        fields = dict(re.findall(r'(\w+)=([^\s]+)', event['line']))
        if 'variants' not in fields: continue
        item = dict(stage=fields['stage'], source=fields['source'], estimate_seconds=None)
        path = Path(fields['source'])
        if not path.is_file():
            output.append(dict(item, reason='manifest unavailable or shared-memory fallback'))
            continue
        with closing(sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)) as db:
            keys = {f'{row[0] & ((1 << 64) - 1):016x}' for row in db.execute('SELECT hash FROM pipeline_cache')}
        keys |= clear
        if (len(clear) != 8 or len(keys) != int(fields['variants']) or
                any(len(compiled[key]) != 1 or compiled[key][0].get('status') != 'ready' for key in keys)):
            output.append(dict(item, reason='incomplete, failed or ambiguous runtime target set'))
            continue
        begin = event['elapsed_s'] - float(fields['enqueue_ms']) / 1000
        latest = max(compiled[key][0]['elapsed_s'] for key in keys)
        item.update(estimate_seconds=max(0, latest - begin), targets=len(keys),
                    pending_at_enqueue=int(fields['pending']), enqueue_ms=float(fields['enqueue_ms']))
        output.append(item)
    return dict(result=str(result_path), stages=output,
                note='Frozen seed hashes plus eight clear masks; exactly one successful runtime variant per target required. Log receipt adds timing uncertainty; extra overlay target unions excluded.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(report(args.result), indent=2) + '\n')

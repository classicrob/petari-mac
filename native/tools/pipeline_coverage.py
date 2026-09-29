#!/usr/bin/env python3
"""Report unique post-preparation first-use coverage from existing playtest logs."""
import argparse
from contextlib import closing
from datetime import datetime, timezone
import json
import re
import sqlite3
from pathlib import Path


def report(paths, seeds=None):
    output = []
    for path in paths:
        stages = {}
        for line in path.read_text(errors='replace').splitlines():
            if '[gx stage config]' not in line:
                continue
            fields = dict(re.findall(r'(\w+)=([^\s]+)', line))
            if fields.get('phase') != 'after_prep' or fields.get('overlay_tag', '0') != '0':
                continue
            stage = stages.setdefault(fields['stage'], {'seen': set(), 'uncovered': set(), 'stage_uncovered': set()})
            key = fields['config']
            stage['seen'].add(key)
            if fields.get('covered') == '0': stage['uncovered'].add(key)
            if fields.get('stage_covered') == '0': stage['stage_uncovered'].add(key)
        for name, stage in stages.items():
            if seeds and (seeds / (name + '.db')).is_file():
                with closing(sqlite3.connect((seeds / (name + '.db')).resolve().as_uri() + '?mode=ro', uri=True)) as db:
                    hashes = {f'{row[0] & ((1 << 64) - 1):016x}' for row in db.execute('SELECT hash FROM pipeline_cache WHERE type=1')}
                predicted = sorted(stage['seen'] - hashes)
            else:
                predicted = None
            output.append({'log': str(path), 'stage': name, 'post_prep_first_use': len(stage['seen']),
                           'uncovered': len(stage['uncovered']), 'uncovered_hashes': sorted(stage['uncovered']),
                           'stage_uncovered': len(stage['stage_uncovered']), 'missing_from_supplied_seed': predicted})
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='*', type=Path)
    parser.add_argument('--seeds', type=Path, help='also compare hashes against these newly generated manifests')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    paths = args.logs or sorted(set(Path('build').glob('observatory-*.log')) | set(Path('build/stage-sweep').rglob('*.log')))
    result = report(paths, args.seeds)
    text = json.dumps({'reports': result, 'logs_scanned': len(paths), 'generated_at_utc': datetime.now(timezone.utc).isoformat(),
                       'note': 'Logs without stage first-use instrumentation cannot establish coverage. Overlay-only records excluded.'}, indent=2) + '\n'
    if args.output: args.output.write_text(text)
    for entry in result:
        print(f'{entry["log"]}: {entry["stage"]}: {entry["uncovered"]}/{entry["post_prep_first_use"]} post-prep first-use configs uncovered')

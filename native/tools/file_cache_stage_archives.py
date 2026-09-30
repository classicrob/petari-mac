#!/usr/bin/env python3
"""Write the per-stage scene archive lists for native_file_cache_demand.

Each output line is `<name> <archive> <archive> ...` (archives as `ObjectData/Kuribo`), in the
order the projection places them: the stage's zone archives (StageData, mounted first by
StageFileLoader), then object archives largest first, so a large archive meets the fullest file
cache (the worst case for the file cache's remaining room).

Stage lists are the offline superset from collect_pipeline_seed_inputs.py (placement of every
scenario layer, static archive aliases, conservative shared families) minus the stationed
archives (StationedFileInfo.cpp), which never go to the file cache. The superset is larger than
any one scenario's real load (Good Egg 1: 18.6 MB listed, 14.5 MB loaded) but misses dynamic
dependencies (break variants, parts); being larger, it drives placement to the 3% limit, the
state the file cache's reserve must cover.

--live NAME=LOG adds the exact list of the last stage load in a PETARI_TRACE_BOOT app log
([heap-arc] lines, HeapMemoryWatcher::noteArchiveMounted), in mount order.
"""
import argparse
import json
import re
from pathlib import Path


def stationed(repo):
    source = (repo / 'src/Game/System/StationedFileInfo.cpp').read_text()
    return {m.rsplit('.arc', 1)[0] for m in re.findall(r'"/((?:ObjectData|LayoutData|StageData)/[^"]+\.arc)"', source)}


def live_list(log):
    segment, previous = [], None
    for line in log.read_text(errors='replace').splitlines():
        match = re.match(r'\[heap-arc\] /?(\S+)\.arc -> ([^,]+), (\d+) bytes; file cache free (-?\d+)', line)
        if not match:
            continue
        free = int(match.group(4))
        if previous is not None and free > previous + 1000000:  # a new file cache: a new stage load
            segment = []
        segment.append(match.group(1))
        previous = free
    return [name for name in segment if name.startswith(('ObjectData/', 'StageData/'))]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--files', type=Path, required=True, help='extracted RMGE01/files directory')
    parser.add_argument('--manifest', type=Path, required=True, help='collect_pipeline_seed_inputs.py --all-stages output')
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--live', action='append', default=[], metavar='NAME=LOG')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    skip = stationed(args.repo)
    stages = json.loads(args.manifest.read_text())['stages']
    lines = []
    for stage in sorted(stages):
        names = [a['path'].rsplit('.arc', 1)[0] for a in stages[stage]['archives']]
        names = [n for n in names if n.count('/') == 1 and n.startswith(('ObjectData/', 'StageData/')) and n not in skip]
        size = lambda n: (args.files / (n + '.arc')).stat().st_size
        zones = [n for n in names if n.startswith('StageData/')]
        objects = sorted((n for n in names if n.startswith('ObjectData/')), key=lambda n: (-size(n), n))
        lines.append(' '.join([stage] + zones + objects))
    for item in args.live:
        name, log = item.split('=', 1)
        lines.append(' '.join([name] + live_list(Path(log))))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(lines) + '\n')
    print(f'{len(lines)} stage lists -> {args.output}')


if __name__ == '__main__':
    main()

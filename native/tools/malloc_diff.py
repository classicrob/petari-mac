#!/usr/bin/env python3
"""Diff two `malloc_history <pid> -allBySize` reports of the same process: live bytes per allocation stack.

    python3 native/tools/malloc_diff.py before.txt after.txt [--top 25] [--frames 8]

Stacks are keyed by their frames without addresses (the executable is the same process, so the
same call path has the same symbols). Prints the stacks whose live bytes grew most, with counts,
and the net change. Used by the soak (native/tools/soak.py) to find what grows per cycle.
"""
import argparse
import re
from collections import defaultdict

LINE = re.compile(r"^(\d+) calls? for (\d+) bytes: (.*)$")
ADDRESS = re.compile(r"0x[0-9a-f]+ ")


def parse(path, frames):
    totals = defaultdict(lambda: [0, 0])
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            parts = [ADDRESS.sub("", p).strip() for p in m[3].split("|")]
            # Innermost frames last in malloc_history; keep the ones nearest the allocation.
            key = " | ".join(parts[-frames:])
            totals[key][0] += int(m[1])
            totals[key][1] += int(m[2])
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--frames", type=int, default=10, help="innermost frames that identify a stack")
    args = parser.parse_args()
    before, after = parse(args.before, args.frames), parse(args.after, args.frames)
    keys = set(before) | set(after)
    deltas = sorted(((after.get(k, [0, 0])[1] - before.get(k, [0, 0])[1],
                      after.get(k, [0, 0])[0] - before.get(k, [0, 0])[0], k) for k in keys), reverse=True)
    net = sum(v[1] for v in after.values()) - sum(v[1] for v in before.values())
    print(f"net live bytes {net / 1048576:+.2f} MB over {len(keys)} stacks")
    for delta, count, key in deltas[:args.top]:
        if delta <= 0:
            break
        print(f"\n{delta / 1024:+10.1f} KB  {count:+6d} allocations")
        for frame in key.split(" | ")[::-1][:args.frames]:
            print(f"    {frame[:170]}")


if __name__ == "__main__":
    main()

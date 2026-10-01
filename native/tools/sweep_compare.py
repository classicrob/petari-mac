#!/usr/bin/env python3
"""Compare two stage sweeps scenario by scenario (regression insurance).

usage: sweep_compare.py OLD NEW [--out report.md]   (sweep names under build/stage-sweep or paths)

Per scenario: outcome (PASS < PASS_WARN < NON_GAMEPLAY/expected < anything worse), warning check
names, audio problems (choppy, underruns, replays, DSP holds), renderer errors, compile failures, heap
warnings, the ready position (a behaviour change shows here), load frames and p99 frame time. Prints
and writes a markdown report listing: worse outcomes, new warnings, new audio/renderer/heap problems,
position changes, and scenarios only in one sweep. Frame-time and audio numbers from concurrent runs
are noise-prone: they are flagged only above generous thresholds.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "build/stage-sweep"
RANK = {"PASS": 0, "PASS_WARN": 1, "NON_GAMEPLAY": 1}


def load(name):
    base = Path(name) if Path(name).exists() else ROOT / name
    runs = {}
    for path in sorted((base / "runs").glob("*/result.json")):
        result = json.loads(path.read_text())
        runs[path.parent.name] = (result, path.parent)
    return runs


def ready_position(directory):
    log = directory / "app.log"
    if not log.is_file():
        return None
    match = re.search(r"stage ready: frame \d+, Mario at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", log.read_text(errors="replace"))
    return tuple(float(v) for v in match.groups()) if match else None


def facts(result, directory):
    warns = sorted({c["name"] for c in result.get("checks", []) if c["status"] == "warn"})
    audio = result.get("audio") or {}
    return {
        "outcome": result["outcome"], "signature": result.get("signature", ""),
        "warns": warns,
        "choppy": bool(audio.get("choppy")), "underrun": audio.get("underrun_frames", 0) or 0,
        "replayed": audio.get("replayed_blocks", 0) or 0, "holds": audio.get("dsp_holds", 0) or 0,
        "renderer_errors": result.get("renderer_error_count", 0) or 0,
        "compile_failures": (result.get("shaders") or {}).get("compile_failures", 0) or 0,
        "heap_warnings": bool(result.get("heap_warnings")),
        "assisted": result.get("assisted_inputs", 0) or 0,
        "ready": ready_position(directory),
        "load_frames": result.get("load_frames"),
        "p99": (result.get("frame_times") or {}).get("p99_ms"),
        "concurrent": bool((result.get("concurrent_apps") or {}).get("seen")),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("old")
    parser.add_argument("new")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    old, new = load(args.old), load(args.new)
    lines, worse, warns, problems, moved, only = [], [], [], [], [], []
    for name in sorted(set(old) | set(new)):
        if name not in old or name not in new:
            only.append(f"{name}: only in {'old' if name in old else 'new'}")
            continue
        a, b = facts(*old[name]), facts(*new[name])
        if RANK.get(b["outcome"], 9) > RANK.get(a["outcome"], 9):
            worse.append(f"{name}: {a['outcome']} -> {b['outcome']} {b['signature']}")
        elif a["outcome"] != b["outcome"]:
            lines.append(f"{name}: outcome {a['outcome']} -> {b['outcome']}")
        added = sorted(set(b["warns"]) - set(a["warns"]))
        if added:
            warns.append(f"{name}: new warnings {added} (was {a['warns']})")
        gone = sorted(set(a["warns"]) - set(b["warns"]))
        if gone:
            lines.append(f"{name}: warnings gone {gone}")
        for key in ("renderer_errors", "compile_failures", "assisted"):
            if b[key] > a[key]:
                problems.append(f"{name}: {key} {a[key]} -> {b[key]}")
        if b["heap_warnings"] and not a["heap_warnings"]:
            problems.append(f"{name}: new heap warnings")
        if b["choppy"] and not a["choppy"]:
            problems.append(f"{name}: audio now choppy (underrun {b['underrun']}, replayed {b['replayed']}, DSP holds {b['holds']}; "
                            f"old {a['underrun']}/{a['replayed']}/{a['holds']}; concurrent new={b['concurrent']} old={a['concurrent']})")
        elif b["underrun"] > a["underrun"] + 1000 or b["replayed"] > a["replayed"] + 20:
            problems.append(f"{name}: audio degraded (underrun {a['underrun']} -> {b['underrun']}, replayed {a['replayed']} -> {b['replayed']})")
        if a["ready"] and b["ready"]:
            d = sum((x - y) ** 2 for x, y in zip(a["ready"], b["ready"])) ** 0.5
            if d > 1.0:
                moved.append(f"{name}: ready position moved {d:.1f} units {a['ready']} -> {b['ready']}")
        elif bool(a["ready"]) != bool(b["ready"]):
            moved.append(f"{name}: ready {'lost' if a['ready'] else 'gained'}")
        if a["load_frames"] and b["load_frames"] and abs(a["load_frames"] - b["load_frames"]) > 60:
            lines.append(f"{name}: load frames {a['load_frames']} -> {b['load_frames']}")
        if a["p99"] and b["p99"] and b["p99"] > max(60.0, 2.0 * a["p99"]) and not b["concurrent"]:
            lines.append(f"{name}: p99 frame {a['p99']} -> {b['p99']} ms (quiet new run)")
    total = len(set(old) & set(new))
    sections = [("Worse outcomes", worse), ("New warnings", warns), ("New renderer/heap/audio/assist problems", problems),
                ("Ready-position changes (> 1 unit)", moved), ("Other differences", lines), ("Only in one sweep", only)]
    out = [f"# {args.old} vs {args.new}: {total} scenarios compared", ""]
    for title, items in sections:
        out.append(f"## {title} ({len(items)})")
        out += [f"- {i}" for i in items] or ["- none"]
        out.append("")
    text = "\n".join(out)
    print(text)
    if args.out:
        args.out.write_text(text + "\n")


if __name__ == "__main__":
    main()

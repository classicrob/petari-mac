#!/usr/bin/env python3
"""Long-session soak: run the app for hours in a loop and find what degrades.

    python3 native/tools/soak.py run --name soak1 --minutes 90
    python3 native/tools/soak.py analyze --name soak1

run: freezes a private copy of the app bundle (build/soak/<name>/app), makes a
fresh stage fixture (build/soak/<name>/user, NAND copied from --source), and
runs PETARI_SMOKE=soak (native/app/smoke_soak.hpp) on the sweep app lane
(build/locked-sweep-lane.sh, owner "soak") with a lock timeout of at most
7500 s. Each cycle: stage (rotating through --stages), pause "Back to the
Comet Observatory", observatory, pause "End Game" with a save, title, reload.
Telemetry (native/app/soak_telemetry.hpp): soak.csv every --interval seconds,
soak.csv.heaps.csv (every JKR heap), frames.csv (every frame, written at
exit), app.log (with [audio] lines each second).

analyze: reads those and writes build/soak/<name>/{summary.md, trends.csv,
cycles.csv, windows.csv, *.png}. Growth is judged two ways: a least-squares
slope per hour after --warmup minutes, and cycle-aligned values (the first
sample of each cycle's observatory phase, so every point is the same scene and
state). A metric is flagged when both grow (monotonic across cycles, and by
more than its noise floor).
"""
import argparse
import csv
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
DEFAULT_APP = BUILD / "macos-gx/native/app/Petari.app"
DEFAULT_STAGES = "EggStarGalaxy:1"

# Metrics checked for growth: (column, unit, noise floor per hour in that unit).
GROWTH_METRICS = [
    ("footprint_mb", "MB", 20.0),
    ("resident_mb", "MB", 20.0),
    ("malloc_in_use_mb", "MB", 10.0),
    ("metal_mb", "MB", 10.0),
    ("threads", "threads", 2.0),
    ("fds", "fds", 2.0),
    ("heap_count", "heaps", 2.0),
]
# Metrics checked for decline (free memory).
DECLINE_METRICS = [
    ("heap_total_free_mb", "MB", 1.0),
    ("root_free_mb", "MB", 1.0),
    ("root_max_free_mb", "MB", 1.0),
]


def run(args):
    out = BUILD / "soak" / args.name
    if out.exists() and not args.force:
        sys.exit(f"{out} exists (use --force to replace it)")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    app = out / "app" / "Petari.app"
    shutil.copytree(args.app, app, symlinks=True)
    user = out / "user"
    subprocess.run([sys.executable, str(ROOT / "native/tools/create_observatory_fixture.py"), "--source", str(args.source),
                    "--output", str(user), "--kind", "stage"], check=True, stdout=subprocess.DEVNULL)
    stages = args.stages.split(",")
    first, _, scenario = stages[0].partition(":")
    frames = int(args.minutes * 60 * 62) + 36000
    env = dict(os.environ)
    env.update({
        "PETARI_SMOKE": "soak",
        "PETARI_SOAK_STAGES": args.stages,
        "PETARI_SOAK_MINUTES": str(args.minutes),
        "PETARI_SOAK_CSV": str(out / "soak.csv"),
        "PETARI_SOAK_INTERVAL": str(args.interval),
        "PETARI_STAGE": first,
        "PETARI_SCENARIO": scenario or "1",
        "PETARI_FRAME_CSV": str(out / "frames.csv"),
        "PETARI_FRAME_CSV_FRAMES": str(frames),
        "PETARI_AUDIO_DIAG": "1",
        "PETARI_TRACE_BOOT": "1",
        "PETARI_PIPELINE_GLOBAL_PRECOMPILE": "0",
        "PETARI_LOCK_TIMEOUT": str(min(7500, int(args.minutes * 60) + 900)),
    })
    command = [str(BUILD / "locked-sweep-lane.sh"), "soak", str(app / "Contents/MacOS/Petari"), "--disc",
               str(BUILD / "game-data/RMGE01"), "--user", str(user), "--test-fixture", "stage"]
    (out / "command.json").write_text(json.dumps({"command": command, "env": {k: env[k] for k in env if k.startswith("PETARI_")},
                                                  "started": time.strftime("%Y-%m-%d %H:%M:%S")}, indent=2))
    print(f"soak {args.name}: {args.minutes} min over {args.stages}; log {out / 'app.log'}")
    with open(out / "app.log", "wb") as log:
        status = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    (out / "exit_status").write_text(f"{status}\n")
    print(f"exit status {status}")
    analyze(argparse.Namespace(name=args.name, warmup=args.warmup))


def read_csv(path):
    if not path.exists():
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def slope_per_hour(times, values):
    points = [(t, v) for t, v in zip(times, values) if not math.isnan(v)]
    if len(points) < 3:
        return math.nan
    n = len(points)
    mt = sum(t for t, _ in points) / n
    mv = sum(v for _, v in points) / n
    var = sum((t - mt) ** 2 for t, _ in points)
    if var == 0:
        return math.nan
    return sum((t - mt) * (v - mv) for t, v in points) / var * 3600.0


def monotonic_fraction(values):
    """Fraction of consecutive steps that do not decrease (1.0 = never shrinks)."""
    steps = [b - a for a, b in zip(values, values[1:]) if not (math.isnan(a) or math.isnan(b))]
    if not steps:
        return math.nan
    return sum(1 for s in steps if s >= 0) / len(steps)


def parse_log(path):
    info = {"audio": [], "result": None, "cycles": [], "alloc": [], "baton": [], "crash": [], "timeouts": []}
    if not path.exists():
        return info
    audio = re.compile(r"^\[audio\] level (\d+)/(\d+) frames, largest request (\d+), underrun (\d+) frames, AI replayed (\d+) blocks")
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\n")
            m = audio.match(line)
            if m:
                info["audio"].append({"level": int(m[1]), "capacity": int(m[2]), "underrun": int(m[4]), "replayed": int(m[5])})
            elif "PETARI SMOKE RESULT" in line:
                info["result"] = line
            elif "PETARI SMOKE TIMEOUT" in line:
                info["timeouts"].append(line)
            elif "]: cycle " in line and "scenario" in line:
                info["cycles"].append(line)
            elif line.startswith("[alloc]"):
                info["alloc"].append(line)
            elif line.startswith("[baton]"):
                info["baton"].append(line)
            elif "fatal signal" in line.lower() or line.startswith("OS fatal") or "Petari crash" in line:
                info["crash"].append(line)
    return info


def cycle_aligned(rows):
    """First sample of each cycle's observatory phase (same scene and state every cycle)."""
    picked = {}
    for row in rows:
        if row.get("phase") == "soak: in the observatory":
            cycle = int(number(row["cycle"]))
            picked.setdefault(cycle, row)
    return [picked[c] for c in sorted(picked)]


def persistent_heaps(heap_rows, warmup_s):
    """Heaps at the same address, type and depth in the first observatory sample of every cycle
    after warm-up: the same scene each time, so free space is comparable (scene heaps are
    recreated at the same address for other stages)."""
    firsts = {}
    for r in heap_rows:
        t = number(r["t_s"])
        if t < warmup_s or r.get("phase") != "soak: in the observatory":
            continue
        firsts.setdefault(int(number(r["cycle"])), t)
    samples = {}
    for r in heap_rows:
        cycle = int(number(r["cycle"]))
        if firsts.get(cycle) != number(r["t_s"]):
            continue
        key = (r["start"], r["type"], r["depth"])  # sizes change (JKRExpHeap::adjustSize)
        samples.setdefault(key, []).append((number(r["t_s"]), number(r["total_free"]), number(r["max_free"]), number(r["size"])))
    keep = {k: v for k, v in samples.items() if len(v) >= max(3, 0.9 * len(firsts))}
    return keep, sorted(firsts.values())


def analyze(args):
    out = BUILD / "soak" / args.name
    rows = read_csv(out / "soak.csv")
    heap_rows = read_csv(out / "soak.csv.heaps.csv")
    log = parse_log(out / "app.log")
    status = (out / "exit_status").read_text().strip() if (out / "exit_status").exists() else "?"
    if not rows:
        sys.exit(f"no telemetry in {out / 'soak.csv'}")
    warmup_s = args.warmup * 60
    t = [number(r["t_s"]) for r in rows]
    steady = [r for r in rows if number(r["t_s"]) >= warmup_s] or rows
    ts = [number(r["t_s"]) for r in steady]
    aligned = cycle_aligned(rows)
    aligned_steady = [r for r in aligned if number(r["t_s"]) >= warmup_s] or aligned

    findings = []
    trends = []
    for column, unit, floor in GROWTH_METRICS + DECLINE_METRICS:
        values = [number(r[column]) for r in steady]
        slope = slope_per_hour(ts, values)
        series = [number(r[column]) for r in aligned_steady]
        mono = monotonic_fraction(series)
        first = next((v for v in series if not math.isnan(v)), math.nan)
        last = next((v for v in reversed(series) if not math.isnan(v)), math.nan)
        change = last - first if not (math.isnan(first) or math.isnan(last)) else math.nan
        declining = (column, unit, floor) in DECLINE_METRICS
        sign = -1 if declining else 1
        flagged = (not math.isnan(slope) and sign * slope > floor and not math.isnan(change) and sign * change > 0
                   and not math.isnan(mono) and (mono >= 0.7 if not declining else mono <= 0.3))
        trends.append({"metric": column, "unit": unit, "slope_per_hour": slope, "cycle_first": first, "cycle_last": last,
                       "cycle_change": change, "cycles": len(series), "nondecreasing_steps": mono, "flag": flagged})
        if flagged:
            findings.append(f"**{column}** {'falls' if declining else 'grows'} {abs(slope):.1f} {unit}/h "
                            f"(cycle-aligned {first:.1f} → {last:.1f} over {len(series)} cycles, "
                            f"{mono:.0%} of steps non-decreasing)")

    # Persistent JKR heaps: free space and largest block over time.
    heaps, heap_times = persistent_heaps(heap_rows, warmup_s)
    heap_trends = []
    for key, samples in heaps.items():
        times = [s[0] for s in samples]
        free = [s[1] / 1024.0 for s in samples]
        largest = [s[2] / 1024.0 for s in samples]
        free_slope = slope_per_hour(times, free)
        largest_slope = slope_per_hour(times, largest)
        heap_trends.append({"start": key[0], "size_kb": samples[-1][3] / 1024.0, "type": key[1], "depth": key[2],
                            "min_free_kb": min(free), "free_slope_kb_h": free_slope,
                            "min_largest_kb": min(largest), "largest_slope_kb_h": largest_slope,
                            "fragmentation_end": 1 - largest[-1] / free[-1] if free[-1] > 0 else 0.0})
        if not math.isnan(free_slope) and free_slope < -64 and free[-1] < free[0]:
            findings.append(f"JKR heap {key[1]} at {key[0]} ({samples[-1][3] / 1024:.0f} KB): free space falls "
                            f"{-free_slope:.0f} KB/h ({free[0]:.0f} → {free[-1]:.0f} KB)")
        if not math.isnan(largest_slope) and largest_slope < -64 and largest[-1] < largest[0]:
            findings.append(f"JKR heap {key[1]} at {key[0]}: largest free block falls {-largest_slope:.0f} KB/h "
                            f"({largest[0]:.0f} → {largest[-1]:.0f} KB; fragmentation)")

    # Frame times per 5-minute window, from the telemetry windows.
    windows = []
    for start in range(0, int(max(t)) + 1, 300):
        w = [r for r in rows if start <= number(r["t_s"]) < start + 300 and number(r["frames"]) > 0]
        if not w:
            continue
        windows.append({"minute": start / 60, "fps": sum(number(r["fps"]) for r in w) / len(w),
                        "p95_ms": max(number(r["ft_p95_ms"]) for r in w), "p99_ms": max(number(r["ft_p99_ms"]) for r in w),
                        "max_ms": max(number(r["ft_max_ms"]) for r in w),
                        "over_33ms": sum(number(r["frames_over_33ms"]) for r in w)})
    if len(windows) >= 4:
        early = [w["p95_ms"] for w in windows[1:3]]
        late = [w["p95_ms"] for w in windows[-2:]]
        if sum(late) / len(late) > 1.25 * sum(early) / len(early) + 1:
            findings.append(f"frame-time p95 rises from {sum(early) / len(early):.1f} to {sum(late) / len(late):.1f} ms")

    # Drift: VI retraces and OS time against the wall clock, audio frames.
    first, last = steady[0], steady[-1]
    span = number(last["t_s"]) - number(first["t_s"])
    drift = {}
    if span > 60:
        drift["vi_rate_hz"] = (number(last["vi_retraces"]) - number(first["vi_retraces"])) / span
        drift["os_time_ratio"] = (number(last["os_time_s"]) - number(first["os_time_s"])) / span
        drift["audio_frames_per_s"] = (number(last["audio_submitted_frames"]) - number(first["audio_submitted_frames"])) / span
        drift["audio_replays"] = number(last["audio_replayed"]) - number(first["audio_replayed"])
    underruns = sum(a["underrun"] for a in log["audio"])
    replays = sum(a["replayed"] for a in log["audio"])

    # Outputs.
    with open(out / "trends.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(trends[0].keys()))
        writer.writeheader()
        writer.writerows(trends)
    if heap_trends:
        with open(out / "heaps_trends.csv", "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(heap_trends[0].keys()))
            writer.writeheader()
            writer.writerows(heap_trends)
    with open(out / "windows.csv", "w", newline="") as f:
        if windows:
            writer = csv.DictWriter(f, fieldnames=list(windows[0].keys()))
            writer.writeheader()
            writer.writerows(windows)
    with open(out / "cycles.csv", "w", newline="") as f:
        if aligned:
            writer = csv.DictWriter(f, fieldnames=list(aligned[0].keys()))
            writer.writeheader()
            writer.writerows(aligned)
    plot(out, rows, aligned, windows, heaps)

    cycles = max((int(number(r["cycle"])) for r in rows), default=0)
    lines = [f"# Soak {args.name}", "",
             f"- Duration {max(t) / 60:.1f} min, {cycles} cycles, exit status {status}",
             f"- Result: {log['result'] or 'none (see app.log)'}"]
    for timeout in log["timeouts"]:
        lines.append(f"- {timeout}")
    for crash in log["crash"][:5]:
        lines.append(f"- crash: {crash}")
    lines += [f"- Warm-up excluded from trends: first {args.warmup} min", "",
              "## Findings", ""]
    lines += [f"- {f}" for f in findings] or ["- nothing grew or degraded beyond the noise floors"]
    lines += ["", "## Trends (after warm-up; cycle-aligned = first observatory sample of each cycle)", "",
              "| metric | slope/h | cycle first | cycle last | cycles | non-decreasing steps | flag |",
              "| --- | ---: | ---: | ---: | ---: | ---: | --- |"]
    for tr in trends:
        lines.append(f"| {tr['metric']} | {tr['slope_per_hour']:.2f} {tr['unit']} | {tr['cycle_first']:.1f} | "
                     f"{tr['cycle_last']:.1f} | {tr['cycles']} | {tr['nondecreasing_steps']:.0%} | "
                     f"{'**yes**' if tr['flag'] else ''} |")
    lines += ["", "## Drift and audio", ""]
    if drift:
        lines += [f"- VI retraces {drift['vi_rate_hz']:.4f} Hz (NTSC 59.94)",
                  f"- OSGetTime / wall clock {drift['os_time_ratio']:.6f}",
                  f"- audio frames submitted {drift['audio_frames_per_s']:.1f} /s; AI replays after warm-up "
                  f"{drift['audio_replays']:.0f}"]
    lines.append(f"- [audio] lines: {len(log['audio'])} s, underrun frames {underruns}, replayed blocks {replays}")
    lines += ["", "## Frame times per 5 minutes", "", "| minute | fps | p95 ms | p99 ms | max ms | frames > 33 ms |",
              "| ---: | ---: | ---: | ---: | ---: | ---: |"]
    for w in windows:
        lines.append(f"| {w['minute']:.0f} | {w['fps']:.1f} | {w['p95_ms']:.1f} | {w['p99_ms']:.1f} | {w['max_ms']:.0f} | "
                     f"{w['over_33ms']:.0f} |")
    if heap_trends:
        lines += ["", "## Persistent JKR heaps (present through the run)", "",
                  "| type | start | size KB | min free KB | free slope KB/h | min largest KB | largest slope KB/h | fragmentation at end |",
                  "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for h in sorted(heap_trends, key=lambda h: -h["size_kb"])[:20]:
            lines.append(f"| {h['type']} | {h['start']} | {h['size_kb']:.0f} | {h['min_free_kb']:.0f} | "
                         f"{h['free_slope_kb_h']:.0f} | {h['min_largest_kb']:.0f} | {h['largest_slope_kb_h']:.0f} | "
                         f"{h['fragmentation_end']:.0%} |")
    lines += ["", "## Detector summaries at exit", ""] + [f"    {l}" for l in log["alloc"] + log["baton"]][-20:]
    lines += ["", "Graphs: " + ", ".join(sorted(p.name for p in out.glob("*.png")))]
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print((out / "summary.md").read_text())


def plot(out, rows, aligned, windows, heaps):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    minutes = [number(r["t_s"]) / 60 for r in rows]
    panels = [("footprint_mb", "footprint MB"), ("resident_mb", "resident MB"), ("malloc_in_use_mb", "malloc in use MB"),
              ("metal_mb", "Metal allocated MB"), ("threads", "threads"), ("fds", "file descriptors"),
              ("heap_total_free_mb", "JKR free MB (all heaps)"), ("root_max_free_mb", "root heap largest free MB")]
    fig, axes = plt.subplots(len(panels), 1, figsize=(11, 2.2 * len(panels)), sharex=True)
    amin = [number(r["t_s"]) / 60 for r in aligned]
    for ax, (column, label) in zip(axes, panels):
        ax.plot(minutes, [number(r[column]) for r in rows], lw=0.8, color="#4c72b0", label="every sample")
        if aligned:
            ax.plot(amin, [number(r[column]) for r in aligned], "o-", ms=3, lw=1, color="#dd8452", label="cycle-aligned")
        ax.set_ylabel(label, fontsize=8)
        ax.grid(alpha=0.3)
    axes[0].legend(fontsize=8, loc="upper left")
    axes[-1].set_xlabel("minutes")
    fig.tight_layout()
    fig.savefig(out / "memory.png", dpi=110)
    plt.close(fig)

    if windows:
        fig, ax = plt.subplots(figsize=(11, 3.5))
        wm = [w["minute"] for w in windows]
        ax.plot(wm, [w["p95_ms"] for w in windows], "o-", label="p95 (worst 10 s window)")
        ax.plot(wm, [w["p99_ms"] for w in windows], "o-", label="p99")
        ax.set_ylabel("frame time ms")
        ax.set_xlabel("minutes (5-minute windows)")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(out / "frametimes.png", dpi=110)
        plt.close(fig)

    if heaps:
        fig, ax = plt.subplots(figsize=(11, 4))
        for key, samples in sorted(heaps.items(), key=lambda kv: -kv[1][-1][3])[:8]:
            ax.plot([s[0] / 60 for s in samples], [s[1] / 1024 for s in samples], lw=0.9,
                    label=f"{key[1]} {key[0]} ({samples[-1][3] / 1048576:.1f} MB)")
        ax.set_ylabel("free KB")
        ax.set_xlabel("minutes")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=7)
        fig.tight_layout()
        fig.savefig(out / "heaps.png", dpi=110)
        plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("run")
    r.add_argument("--name", required=True)
    r.add_argument("--minutes", type=float, default=60)
    r.add_argument("--stages", default=DEFAULT_STAGES, help="Stage:scenario,... rotated per cycle")
    r.add_argument("--interval", type=int, default=10, help="telemetry seconds")
    r.add_argument("--warmup", type=float, default=10, help="minutes excluded from trends")
    r.add_argument("--source", type=Path, default=BUILD / "observatory-user-2", help="user directory whose NAND is copied")
    r.add_argument("--app", type=Path, default=DEFAULT_APP)
    r.add_argument("--force", action="store_true")
    a = sub.add_parser("analyze")
    a.add_argument("--name", required=True)
    a.add_argument("--warmup", type=float, default=10)
    args = parser.parse_args()
    {"run": run, "analyze": analyze}[args.command](args)


if __name__ == "__main__":
    main()

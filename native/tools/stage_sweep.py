#!/usr/bin/env python3
"""Whole-game stage sweep: crash/hang/heap/shader/perf coverage per galaxy and mission.

Each run launches the app once (through the single app lock, build/locked-app.sh)
with --test-fixture stage and PETARI_SMOKE=stage (native/app/smoke_stage.hpp):
the saved file loads, the game's own after-loading galaxy move enters the stage
and scenario, and the driver idles, walks, jumps, spins, turns the camera and
pauses. SYNTHETIC ENTRY: a PASS says the stage loads and runs under basic
input; it says nothing about progression, unlocks or completing the mission.

  stage_sweep.py list [--domes 1,2] [--json]
  stage_sweep.py run --domes 1,2 --scenarios 1-3 [--name batch1]
  stage_sweep.py run --stages EggStarGalaxy --scenarios 2 --name repro --rerun
  stage_sweep.py summarize --name batch1

Galaxies and scenarios come from the disc: StageData/<Stage>/<Stage>Scenario.arc
(scenariodata.bcsv). Domes: the AstroDome placement layers (LayerA..F hold the
Mini<Galaxy> objects of AstroDome scenarios 1..6), plus the hidden galaxies'
Appear<Galaxy> flags (SpecialStarGrand<N>) in src/Game/System/GameEventFlagTable.cpp.

Output: build/stage-sweep/<name>/runs/<Stage>-s<n>/{app.log,frames.csv,result.json,user/},
build/stage-sweep/<name>/{results.json,results.csv,summary.md}.
"""
import argparse
import csv
import concurrent.futures
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shutil
import signal
import sqlite3
import statistics
import struct
import subprocess
import sys
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
from collect_pipeline_seed_inputs import archive_files, decompress, field_hash, string, u16, u32  # noqa: E402

DOME_NAMES = {1: "Terrace", 2: "Fountain", 3: "Kitchen", 4: "Bedroom", 5: "Engine Room", 6: "Garage"}
NON_GAMEPLAY_STAGES = {"EpilogueDemoStage"}  # the ending movie stage: no controllable gameplay
HEAP_WARN_PCT = 10.0
AUDIO_CHOPPY_HOLDS_PER_S = 1.0  # sustained DSP holds above this, over the stage window, is AUDIO-CHOPPY  # a heap with less free at any report after the stage entry is a WARN
LATE_MS = 1000.0 / 60.0 * 1.25  # the frame-stats "late" threshold (1.25 VI fields)
FIELDS = ["ScenarioNo", "ScenarioName", "PowerStarId", "AppearPowerStarObj", "Comet", "IsHidden", "ZoneName"]
HASHES = {field_hash(name): name for name in FIELDS}


# --- disc enumeration ---------------------------------------------------------------------------

def bcsv_rows(data):
    count, fields, offset, stride = (u32(data, i) for i in (0, 4, 8, 12))
    strings = data[offset + count * stride:]
    for row in range(count):
        record = data[offset + row * stride:offset + (row + 1) * stride]
        values = {}
        for field in range(fields):
            entry = 16 + field * 12
            key, mask, pos, shift, kind = u32(data, entry), u32(data, entry + 4), u16(data, entry + 8), data[entry + 10], data[entry + 11]
            name = HASHES.get(key, key)
            if kind == 6:
                values[name] = string(strings, u32(record, pos))
            elif kind == 0:
                value = (u32(record, pos) & mask) >> shift
                values[name] = value - (1 << 32) if value >= 1 << 31 else value
            elif kind == 4:
                values[name] = (u16(record, pos) & mask) >> shift
            elif kind == 5:
                values[name] = (record[pos] & mask) >> shift
            elif kind == 2:
                values[name] = struct.unpack(">f", record[pos:pos + 4])[0]
            elif kind == 1:
                values[name] = record[pos:pos + 32].split(b"\0")[0].decode("shift_jis", errors="replace")
        yield values


def archive_tree(data):
    """(path, bytes) for every file of a RARC, with directory paths."""
    data = decompress(data)
    header = u32(data, 8)
    payload = header + u32(data, 12)
    dirs, entries = header + u32(data, header + 4), header + u32(data, header + 12)
    strings = header + u32(data, header + 20)

    def walk(index, path):
        node = dirs + index * 16
        count, first = u16(data, node + 10), u32(data, node + 12)
        for i in range(first, first + count):
            entry = entries + i * 20
            flags, name = data[entry + 4], string(data, strings + int.from_bytes(data[entry + 5:entry + 8], "big"))
            if name in (".", ".."):
                continue
            if flags & 2:
                yield from walk(u32(data, entry + 8), f"{path}/{name}")
            else:
                offset, length = u32(data, entry + 8), u32(data, entry + 12)
                yield f"{path}/{name}".lower(), decompress(data[payload + offset:payload + offset + length])

    yield from walk(0, "")


def resolve_placement(files, stage, scenario, selector):
    """Resolve a root-zone placement in the selected scenario, never a disabled layer.

    selector: object name, optionally :FIELD=integer and @zero-based-match-index.
    Subzone coordinates are deliberately not treated as world coordinates.
    """
    match = re.fullmatch(r"([^:@]+)(?::([A-Za-z_0-9]+)=(-?\d+))?(?:@(\d+))?", selector)
    if not match:
        raise ValueError("placement must be NAME[:FIELD=integer][@index]")
    name, field, value, index = match.groups()
    scenario_file = files / "StageData" / stage / f"{stage}Scenario.arc"
    mask = None
    for path, data in archive_tree(scenario_file.read_bytes()):
        if path.endswith("/scenariodata.bcsv"):
            for row in bcsv_rows(data):
                if row.get("ScenarioNo") == scenario:
                    mask = row.get(field_hash(stage))
    if mask is None:
        raise ValueError(f"no root-zone layer mask for {stage} scenario {scenario}")
    candidates = []
    archive = files / "StageData" / f"{stage}.arc"
    for path, data in archive_tree(archive.read_bytes()):
        layer = re.search(r"/(?:placement|generalpos)/(common|layer[a-z])/", path)
        if not layer:
            continue
        if layer[1] != "common" and not mask & (1 << (ord(layer[1][-1]) - ord("a"))):
            continue
        for row_number, row in enumerate(bcsv_rows(data)):
            if row.get(field_hash("name")) != name and row.get(field_hash("PosName")) != name:
                continue
            if field and row.get(field_hash(field)) != int(value):
                continue
            xyz = [row.get(field_hash("pos_" + axis)) for axis in "xyz"]
            if any(v is None for v in xyz):
                raise ValueError(f"placement lacks coordinates: {path} row {row_number}")
            candidates.append({"archive": str(archive), "path": path, "row": row_number,
                               "position": xyz, "selector": selector, "layer_mask": mask})
    if not candidates:
        raise ValueError(f"{selector}: no active root-zone matches")
    if index is None and len(candidates) != 1:
        raise ValueError(f"{selector}: {len(candidates)} active root-zone matches; specify @index for multiple matches")
    chosen = int(index or 0)
    if chosen >= len(candidates):
        raise ValueError(f"{selector}: index {chosen} outside {len(candidates)} active root-zone matches")
    return candidates[chosen]


def dome_map(files):
    domes = {}
    dome_arc = files / "StageData/AstroDome.arc"
    if dome_arc.is_file():
        for path, data in archive_tree(dome_arc.read_bytes()):
            match = re.search(r"/placement/layer([a-f])/objinfo$", path)
            if not match:
                continue
            for row in bcsv_rows(data):
                for value in row.values():
                    if isinstance(value, str) and value.startswith("Mini") and value.endswith("Galaxy"):
                        domes[value[4:]] = "abcdef".index(match.group(1)) + 1
    table = REPO / "src/Game/System/GameEventFlagTable.cpp"
    if table.is_file():
        for galaxy, grand in re.findall(r'\{"Appear(\w+Galaxy)",\s*GameEventFlag::Type_EventFlag,[^}]*"SpecialStarGrand(\d)"',
                                        table.read_text(errors="replace")):
            domes.setdefault(galaxy, int(grand))
    return domes


def enumerate_stages(files):
    domes = dome_map(files)
    stages = []
    for directory in sorted((files / "StageData").iterdir()):
        scenario_arc = directory / f"{directory.name}Scenario.arc"
        if not directory.is_dir() or not scenario_arc.is_file():
            continue
        if directory.name == "FileSelect":
            continue  # the title/file-select stage, not a place the fixture can enter
        scenarios = []
        for name, data in ((n, d) for _, n, d in archive_files(scenario_arc.read_bytes())):
            if name.lower() != "scenariodata.bcsv":
                continue
            for row in bcsv_rows(data):
                scenarios.append({
                    "scenario": int(row.get("ScenarioNo", 0)),
                    "name": row.get("ScenarioName", ""),
                    "power_star_id": row.get("PowerStarId"),
                    "comet": row.get("Comet", ""),
                    "hidden": bool(row.get("IsHidden", 0)),
                })
        scenarios.sort(key=lambda s: s["scenario"])
        dome = domes.get(directory.name)
        stages.append({"stage": directory.name, "dome": dome, "dome_name": DOME_NAMES.get(dome, "other"),
                       "scenarios": scenarios})
    return stages


def parse_range(text):
    values = set()
    for part in text.split(","):
        if "-" in part:
            low, high = part.split("-")
            values.update(range(int(low), int(high) + 1))
        elif part:
            values.add(int(part))
    return values


def select(stages, args):
    domes = parse_range(args.domes) if args.domes else None
    names = set(args.stages.split(",")) if args.stages else None
    scenarios = parse_range(args.scenarios) if args.scenarios else None
    chosen = []
    for stage in stages:
        if names is not None and stage["stage"] not in names:
            continue
        if domes is not None and stage["dome"] not in domes:
            continue
        if names is None and domes is None and not args.all:
            continue
        for scenario in stage["scenarios"]:
            if scenarios is None or scenario["scenario"] in scenarios:
                chosen.append((stage, scenario))
    if names:
        missing = names - {s["stage"] for s in stages}
        if missing:
            raise SystemExit(f"stage_sweep: no scenario data for {', '.join(sorted(missing))}")
    return chosen


# --- one run -----------------------------------------------------------------------------------

def load_fixture_module():
    spec = importlib.util.spec_from_file_location("fixture", TOOLS / "create_observatory_fixture.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def snapshot_sqlite(source, target):
    """A consistent single-file copy of a (possibly WAL, possibly in-use) SQLite database."""
    src = sqlite3.connect(f"file:{source}?mode=ro", uri=True)
    dst = sqlite3.connect(target)
    with dst:
        src.backup(dst)
    src.close()
    dst.close()


def prepare_baseline(out, cache_source):
    baseline = out / "baseline"
    if baseline.is_dir():
        return baseline
    baseline.mkdir(parents=True)
    note = {"source": str(cache_source) if cache_source else None, "files": []}
    if cache_source:
        for name in ("pipeline_cache.db", "dawn_cache.db"):
            if (cache_source / name).is_file():
                snapshot_sqlite(cache_source / name, baseline / name)
                note["files"].append(name)
    (baseline / "baseline.json").write_text(json.dumps(note, indent=2) + "\n")
    return baseline


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))]


def frame_summary(csv_path, first, last):
    if not csv_path.is_file() or not first or not last or last <= first:
        return None
    intervals, captures, unfocused = [], 0, 0
    with csv_path.open() as handle:
        for row in csv.DictReader(handle):
            frame = int(row["frame"])
            if first < frame <= last:
                intervals.append(float(row["interval_ms"]))
                captures += int(row.get("efb_captures") or 0)
                unfocused += row.get("unfocused") == "1"
    if not intervals:
        return None
    return {
        "frames": len(intervals),
        "mean_ms": round(statistics.fmean(intervals), 2),
        "p50_ms": round(percentile(intervals, 0.50), 2),
        "p95_ms": round(percentile(intervals, 0.95), 2),
        "p99_ms": round(percentile(intervals, 0.99), 2),
        "max_ms": round(max(intervals), 2),
        "late": sum(1 for v in intervals if v > LATE_MS),
        "over_33ms": sum(1 for v in intervals if v > 1000.0 / 30.0),
        "over_100ms": sum(1 for v in intervals if v > 100.0),
        "efb_captures": captures,
        # Frames with the window unfocused (another app in front): their timing is not comparable.
        "unfocused": unfocused,
    }


def load_seconds(csv_path, first, last):
    if not csv_path.is_file() or not first or not last or last <= first:
        return None
    total = 0.0
    with csv_path.open() as handle:
        for row in csv.DictReader(handle):
            if first < int(row["frame"]) <= last:
                total += float(row["interval_ms"])
    return round(total / 1000.0, 2)


BACKTRACE = re.compile(r"^\d+\s+(\S+)\s+0x[0-9a-f]+\s+(.*?)(?:\s+\+\s+\d+)?$")
NOISE_FRAMES = ("libsystem_", "libdyld", "Crash::", "crashHandler", "signalHandler", "_sigtramp", "abort", "__pthread",
                "libc++abi", "libobjc", "libc++.", "terminate")


def normalize(text):
    text = re.sub(r"0x[0-9a-fA-F]+", "0x?", text)
    text = re.sub(r"\d+(\.\d+)?", "N", text)
    return text.strip()


def analyze(log_path, csv_path, stage, exit_status, timed_out, user):
    text = log_path.read_text(errors="replace") if log_path.is_file() else ""
    lines = text.splitlines()
    result = {"exit_status": exit_status, "timed_out": timed_out,
              "background_smoke": "PETARI SMOKE BACKGROUND: enabled;" in text}

    match = re.findall(r"PETARI SMOKE RESULT: (\w+) \((.*)\); pressing the power button", text)
    result["smoke_result"], result["smoke_reason"] = match[-1] if match else (None, None)
    result["checks"] = [{"name": n, "status": s, "detail": d} for n, s, d in
                        re.findall(r"stage check (\S+): (ok|warn|done|fail) (.*)", text)]
    frames = {}
    for key, pattern in (("load_start", r"load start (\d+)"), ("ready", r", ready (\d+), end"), ("end", r", end (\d+), checks")):
        found = re.findall(r"stage summary: .*?" + pattern, text)
        frames[key] = int(found[-1]) if found else None
    if frames["ready"] is None:
        found = re.findall(r"stage ready: frame (\d+)", text)
        frames["ready"] = int(found[-1]) if found else None
    last_frame = re.findall(r"PETARI SMOKE \[frame (\d+)\]", text)
    if frames["end"] is None and last_frame:
        frames["end"] = int(last_frame[-1])
    result["frames"] = frames
    loaded = re.findall(r"stage loaded: .* after (\d+) frames", text)
    result["load_frames"] = int(loaded[-1]) if loaded else None
    if frames["load_start"] and frames["ready"]:
        result["load_to_ready_seconds"] = load_seconds(csv_path, frames["load_start"], frames["ready"])
    result["frame_times"] = frame_summary(csv_path, frames["ready"], frames["end"])

    # Crashes, panics, heap.
    crash = "==== Petari native crash report ====" in text
    result["crash"] = None
    if crash:
        start = text.index("==== Petari native crash report ====")
        report = text[start:].splitlines()
        signal_line = next((l for l in report if l.startswith("signal:")), "")
        panic = next((l[len("last panic:"):].strip() for l in report if l.startswith("last panic:")), "")
        frames_seen = []
        for line in report:
            m = BACKTRACE.match(line.strip())
            if m and not any(n in m.group(1) + m.group(2) for n in NOISE_FRAMES):
                frames_seen.append(m.group(2))
        uncaught = re.findall(r"terminating due to uncaught exception of type (\S+?)(?:: |$)", text, re.M)
        result["crash"] = {"signal": signal_line, "panic": panic, "top_frames": frames_seen[:8],
                           "uncaught_exception": uncaught[-1] if uncaught else None}
    result["heap_failure"] = "Native heap allocation failed" in text
    result["os_fatal"] = [l for l in lines if l.startswith("OSFatal:")][:5]
    hang_samples = sorted(str(p) for p in (user / "Crashes").glob("petari-hang-*.sample.txt")) if user.is_dir() else []
    result["hang_samples"] = hang_samples
    result["hang_lines"] = [l for l in lines if l.startswith("[hang]") or "watchdog" in l.lower()][:20]

    # Shaders.
    configs = re.findall(r"^\[gx stage config\] stage=(\S+) config=(\w+) phase=(\w+) covered=(\d)", text, re.M)
    mine = [c for c in configs if c[0] == stage]
    compile_status = re.findall(r"^\[gx pipeline compile\] .*status=(\w+)", text, re.M)
    queue_ms = [float(v) for v in re.findall(r"^\[gx pipeline compile\] .*? queue_ms=([\d.]+)", text, re.M)]
    build_ms = [float(v) for v in re.findall(r"^\[gx pipeline compile\] .*? build_ms=([\d.]+)", text, re.M)]
    policy = re.findall(r"^\[gx pipeline\] policy=(\w+)", text, re.M)
    seed_line = re.findall(r"^PETARI SMOKE SHADER SEED: (.*)$", text, re.M)
    resolves = [float(v) for v in re.findall(r"^\[gx pipeline\] blocking resolve ([\d.]+) ms", text, re.M)]
    ready = re.findall(r"^\[gx stage ready\] stage=" + re.escape(stage) + r" residual_wait_ms=([\d.]+) pending=(\d+) failed=(\d+) result=(\w+)", text, re.M)
    result["shaders"] = {
        "stage_first_use_configs": len({c[1] for c in mine}),
        "stage_first_use_uncovered": len({c[1] for c in mine if c[3] == "0"}),
        "all_first_use_uncovered": len({c[1] for c in configs if c[3] == "0"}),
        "compiles": len(compile_status),
        "compile_failures": sum(1 for s in compile_status if s != "ready"),
        # Compile-queue pressure (MTLCompilerService contention shows up as queue/build time).
        "compile_queue_ms_p50": percentile(queue_ms, 0.5) if queue_ms else None,
        "compile_queue_ms_p90": percentile(queue_ms, 0.9) if queue_ms else None,
        "compile_queue_ms_total": round(sum(queue_ms), 1),
        "compile_build_ms_total": round(sum(build_ms), 1),
        "compile_cache_hits": sum(1 for v in build_ms if v < 20.0),
        "shader_seed": seed_line[-1] if seed_line else None,
        "pipeline_policy": policy[-1] if policy else None,
        "blocking_resolves": len(resolves),
        "blocking_resolve_max_ms": max(resolves) if resolves else 0.0,
        "stage_gate": [{"residual_wait_ms": float(a), "pending": int(b), "failed": int(c), "result": d} for a, b, c, d in ready],
    }
    # Heap headroom on the stage, per heap over every [heap] report after the
    # entry (HeapMemoryWatcher reports at scene initialization): the minimum
    # free bytes/percent and the last report. Under 10% free is a WARN.
    heaps = {}
    after = text.rfind("synthetic stage entry:")
    for name, size, used, free, pct, maxfree in re.findall(
            r"^\[heap\] (.+?): size (\d+), used (\d+), free (\d+) \(([\d.]+)%\), max free (\d+)", text[after:] if after >= 0 else "", re.M):
        heap = heaps.setdefault(name, {"reports": 0, "min_free": int(free), "min_free_pct": float(pct)})
        heap["reports"] += 1
        heap["min_free"] = min(heap["min_free"], int(free))
        heap["min_free_pct"] = min(heap["min_free_pct"], float(pct))
        heap.update({"size": int(size), "free": int(free), "free_pct": float(pct), "max_free": int(maxfree)})
    result["heaps"] = heaps
    # File-cache demand (8c4c107cf onward, with PETARI_TRACE_BOOT): the last
    # archive summary after the entry, and the archives of this stage's load
    # that were mounted in scene GDDR (spilled from the file cache).
    stage_text = text[after:] if after >= 0 else ""
    archives = re.findall(r"^\[heap\] file cache archives: (\d+) resident, (\d+) bytes; other use \([^)]*\) (\d+) bytes",
                          stage_text, re.M)
    result["file_cache_archives"] = ({"resident": int(archives[-1][0]), "bytes": int(archives[-1][1]),
                                      "other_use": int(archives[-1][2])} if archives else None)
    mounts = re.findall(r"^\[heap-arc\] (\S+) -> ([^,]+), (\d+) bytes", stage_text, re.M)
    result["archive_mounts"] = {"total": len(mounts),
                                "scene_gddr": [m[0] for m in mounts if m[1].strip() == "scene GDDR"]}
    result["heap_warnings"] = [f"{name} {h['min_free_pct']}% ({h['min_free']} bytes) free"
                               for name, h in heaps.items() if h["min_free_pct"] < HEAP_WARN_PCT]
    # A stage without a per-stage seed file: the renderer logs the absent
    # manifest as "manifest read failed: unable to open database file" and
    # falls back to shared-observed-memory (PIPELINE_SEEDING.md). Expected.
    missing_seed = re.compile(r"^\[gx stage prep\] stage=\S+ manifest read failed: unable to open database file$")
    sources = re.findall(r"^\[gx stage prep\] stage=" + re.escape(stage) + r" source=(\S+)", text, re.M)
    result["shaders"]["seed_source"] = sources[-1] if sources else None
    # Zero-valued counters (failed=0, failed_compiles=0, errors=0.000) are
    # summaries, not errors: drop them before looking for error words.
    zero_counter = re.compile(r"\b\w+=0(?:\.0+)?(?=\s|$|[,;)])")
    errors = [l for l in lines if re.match(r"^\[(aurora|gx[^\]]*|dawn|webgpu)\]", l, re.I)
              and re.search(r"(?i)\b(error|invalid|validation|lost)\b|fail", zero_counter.sub("", l))
              and not missing_seed.match(l)]
    result["renderer_errors"] = errors[:20]
    result["renderer_error_count"] = len(errors)
    # Asset guards (native/tools/LAYOUT_REFERENCE_GUARDS.md): each missing
    # (kind, layout, name) is logged once; the seam turns a PASS into FAIL.
    result["missing_assets"] = sorted({f"{area} {kind} {layout}/{name}" for area, kind, layout, name in re.findall(
        r'^\[(layout|sound)\] missing (\w+) layout="([^"]*)" name="([^"]*)"', text, re.M)} | {
        f"{area} {rest.strip()}" for area, rest in re.findall(r'^\[(layout|sound)\] missing (?!\w+ layout=)(.*)', text, re.M)})
    # Audio (PETARI_AUDIO_DIAG): per-second reports after the stage entry. A DSP hold is a
    # mix frame the DSP could not finish in time (audible as a stutter).
    audio_text = text[after:] if after >= 0 else ""
    levels = re.findall(r"^\[audio\] level .*?underrun (\d+) frames, AI replayed (\d+) blocks \(since previous report, ([\d.]+) s",
                        audio_text, re.M)
    holds = [int(h) for h in re.findall(r"^\[audio-dma\] .*DSP holds (\d+)", audio_text, re.M)]
    if levels or holds:
        seconds = sum(float(l[2]) for l in levels) or float(len(holds))
        rate = sum(holds) / seconds if seconds else 0.0
        result["audio"] = {"reports": len(levels), "seconds": round(seconds, 1),
                           "underrun_frames": sum(int(l[0]) for l in levels),
                           "replayed_blocks": sum(int(l[1]) for l in levels),
                           "dsp_holds": sum(holds), "dsp_holds_per_s": round(rate, 2),
                           "max_dsp_holds_in_a_report": max(holds, default=0),
                           "reports_with_holds": sum(1 for h in holds if h > 0),
                           "choppy": rate > AUDIO_CHOPPY_HOLDS_PER_S}
    else:
        result["audio"] = None
    storage = re.search(r"PETARI SMOKE STORAGE: user=(.*?) cache=(.*?) metal=(.*?) namespace=(.*?) temp=(.*?)(?: temp_namespace=(.*))?$", text, re.M)
    result["storage"] = dict(zip(("user", "cache", "metal", "namespace", "temp", "temp_namespace"), storage.groups())) if storage else None
    result["instrumented"] = "PETARI ISOLATION MONITOR: enabled;" in text
    result["silent_audio"] = "PETARI SMOKE AUDIO: output gain 0;" in text
    result["assisted_inputs"] = len(re.findall(r"physical input while", text))
    result["fixture_lines"] = [l for l in lines if l.startswith("PETARI FIXTURE")]
    traced = re.findall(r"^Petari trace: frame \d+.*? at ([\d.]+) s", text, re.M)
    result["app_seconds"] = float(traced[-1]) if traced else None  # game time, excluding app-lock waits

    result["stage"] = stage
    result["outcome"], result["signature"] = classify(result)
    return result


def classify(r):
    reason = r["smoke_reason"] or ""
    if r.get("infra"):
        return "INFRA", r["infra"]
    if r["crash"]:
        if r["heap_failure"]:
            return "HEAP", "heap: " + normalize(r["crash"]["panic"])
        key = r["crash"]["panic"] or (r["crash"]["top_frames"][0] if r["crash"]["top_frames"] else r["crash"]["signal"])
        if not r["crash"]["panic"] and r["crash"].get("uncaught_exception"):
            key = "uncaught " + r["crash"]["uncaught_exception"] + " in " + key
        # Keep symbol names readable: strip only addresses and "+ offset" suffixes.
        key = re.sub(r"0x[0-9a-fA-F]+", "0x?", key)
        return "CRASH", "crash: " + key.strip()
    if r["heap_failure"]:
        return "HEAP", "heap failure (no crash report)"
    if r["timed_out"]:
        return "TIMEOUT", "runner timeout"
    status = r["exit_status"]
    if status == 124:
        return "HANG", "watchdog: no frame for the stall limit"
    if status == 125:
        return "SHUTDOWN_HANG", "watchdog: no exit after the power button"
    if status is not None and (status < 0 or status >= 128):
        return "CRASH", f"killed by signal {abs(status) if status < 0 else status - 128} (no crash report)"
    result = r["smoke_result"]
    if result is None:
        return "NO_RESULT", f"exit {status} without a smoke result"
    if result == "PASS":
        if status != 0:
            return "EXIT_MISMATCH", f"PASS but exit {status}"
        if r["shaders"]["compile_failures"] or r["renderer_error_count"]:
            return "PASS_RENDER_ERRORS", "renderer errors during a passing run"
        warned = (any(c["status"] == "warn" for c in r["checks"]) or r.get("heap_warnings")
                  or (r.get("audio") or {}).get("choppy"))
        return ("PASS_WARN" if warned else "PASS"), ""
    if result == "ASSISTED":
        return "ASSISTED", "physical input"
    if result == "BLOCKED":
        return "BLOCKED", normalize(reason.split(": ", 1)[-1])
    body = reason.split(": ", 1)[-1]
    if "FAIL: missing asset references" in reason or (r.get("missing_assets") and result == "FAIL"):
        return "MISSING_ASSET", "; ".join(r.get("missing_assets") or []) or normalize(reason)
    # Stages with no gameplay (the ending movie stage): gameplay never becomes
    # ready by design. Loading and running to the ready limit without a crash is
    # all such a run can show; it is reported separately, never as PASS.
    if r.get("stage") in NON_GAMEPLAY_STAGES and body.startswith("not ready: gameplay never became ready") \
            and r.get("load_frames") is not None:
        return "NON_GAMEPLAY", "loaded and ran without gameplay (expected for this stage)"
    for prefix, outcome in (("died:", "DIED"), ("not ready:", "NOT_READY"), ("left the stage", "LEFT_STAGE"),
                            ("the stage fixture entry", "ENTRY_FAIL"), ("scenario mismatch", "ENTRY_FAIL"),
                            ("pausing", "PAUSE_FAIL"), ("no PauseMenu", "PAUSE_FAIL"), ("Mario moved", "PAUSE_FAIL"),
                            ("frame limit", "FRAME_LIMIT"), ("the player vanished", "PLAYER_VANISHED")):
        if body.startswith(prefix):
            return outcome, normalize(body)
    if reason.startswith("boot"):
        return "BOOT_FAIL", normalize(reason)
    return "FAIL", normalize(body)


def app_command(args, user, stage, scenario):
    return [str(args.app), "--disc", str(args.disc), "--user", str(user), "--test-fixture", "stage"], {
        "PETARI_SMOKE": "stage", "PETARI_STAGE": stage, "PETARI_SCENARIO": str(scenario),
        "PETARI_SMOKE_FRAMES": str(args.frames), "PETARI_STAGE_IDLE_FRAMES": str(args.idle_frames),
        "PETARI_TRACE_BOOT": "1",
        # Per-second audio reports ([audio] underruns/replays, [audio-dma] DSP holds): audio coverage.
        "PETARI_AUDIO_DIAG": "1",
        # The global shader precompile (default on when __global__.db exists)
        # can block startup for ~20 min on a cold cache and competes with the
        # measured gameplay; off until the pipeline worker has evaluated it.
        "PETARI_PIPELINE_GLOBAL_PRECOMPILE": os.environ.get("PETARI_PIPELINE_GLOBAL_PRECOMPILE", "0"),
        # Renderer-coverage evidence (EFB captures, first-use/uncovered counts, blocking
        # resolves) needs blocking draws: the default policy skips draws whose pipeline is
        # still compiling after ~6 ms, which can hide renderer failures. "default" leaves
        # the app's own default (functional pass/fail only). Recorded per run.
        **({} if getattr(args, "pipeline_policy", "blocking") == "default"
           else {"PETARI_PIPELINE_POLICY": args.pipeline_policy}),
    }


class ConcurrencyWatcher:
    """While this run's app is alive, samples every 5 s for OTHER Petari apps
    (any process running a Petari.app executable whose command line does not
    name this run's --user directory). Frame times from a run that overlapped
    another app are not performance evidence."""

    def __init__(self, user):
        import threading
        self.user = str(user)
        self.samples = self.own_samples = 0
        self.max_others = 0
        self.others = set()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.wait(5.0):
            try:
                listing = subprocess.run(["ps", "-axo", "pid=,command="], capture_output=True, text=True, timeout=10).stdout
            except (OSError, subprocess.SubprocessError):
                continue
            # Match the executable, not an --app argument in a queued Python runner
            # or a shell command. Those are waiters, not concurrently running games.
            apps = [line.strip() for line in listing.splitlines()
                    if len(line.split(None, 2)) >= 2
                    and line.split(None, 2)[1].endswith("Petari.app/Contents/MacOS/Petari")]
            own = [a for a in apps if self.user in a]
            if not own:
                continue  # waiting for the lock, or already exited: not the measured window
            self.samples += 1
            others = [a for a in apps if self.user not in a]
            self.max_others = max(self.max_others, len(others))
            for other in others:
                match = re.search(r"--user (\S+)", other)
                self.others.add(match.group(1) if match else other.split(None, 1)[0])

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=15)

    def summary(self):
        return {"seen": self.max_others > 0, "samples_while_running": self.samples,
                "max_other_apps": self.max_others, "other_user_dirs": sorted(self.others)[:10]}


def write_result(path, result):
    temporary = path.with_suffix(".json.partial")
    temporary.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def run_selected(chosen, jobs, action, completed):
    if jobs == 1:
        for stage, scenario in chosen:
            action(stage, scenario)
            completed()
        return
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        remaining = iter(chosen)
        pending = set()
        def submit_next():
            item = next(remaining, None)
            if item is not None:
                pending.add(pool.submit(action, *item))
        for _ in range(jobs):
            submit_next()
        try:
            while pending:
                done, pending = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
                # Check the whole completed batch before starting more work.
                for future in done:
                    future.result()
                for _ in done:
                    completed()
                    submit_next()
        except BaseException:
            # Already-running apps retain their bounded alarms and are joined.
            for future in pending:
                future.cancel()
            raise


def run_one(args, out, baseline, fixture, stage, scenario):
    name = f"{stage['stage']}-s{scenario['scenario']}"
    run_dir = out / "runs" / name
    if (run_dir / "result.json").is_file() and not args.rerun:
        print(f"[skip] {name}: result exists (use --rerun)")
        return json.loads((run_dir / "result.json").read_text())
    for other in ([] if args.env or args.warp_placement else args.reuse_from):
        # The same run, finished by another sweep on the identical app binary
        # and without extra environment: copy it in (log, frames, result,
        # pipeline DB, crash reports) instead of running it again.
        source = args.out_root / other / "runs" / name
        try:
            previous = json.loads((source / "result.json").read_text())
        except (OSError, ValueError):
            continue
        if previous.get("app_sha256") != args.app_sha256 or previous.get("extra_env") or previous.get("outcome") == "INFRA":
            continue
        if run_dir.exists():
            shutil.rmtree(run_dir)
        shutil.copytree(source, run_dir, symlinks=True)
        previous["reused_from"] = str(source)
        previous["log"] = str(run_dir / "app.log")
        write_result(run_dir / "result.json", previous)
        print(f"[reuse] {name}: {previous['outcome']} from {other}", flush=True)
        return previous
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    user = run_dir / "user"
    fixture.create(args.source, user, "stage")
    for db in baseline.glob("*.db"):
        shutil.copy2(db, user / db.name)
    command, env_extra = app_command(args, user, stage["stage"], scenario["scenario"])
    env_extra["PETARI_FRAME_CSV"] = str(run_dir / "frames.csv")
    extra = dict(item.split("=", 1) for item in args.env)
    env_extra.update(extra)
    if args.warp_placement:
        if "PETARI_STAGE_WARP" in env_extra or "PETARI_STAGE_WARP" in os.environ:
            raise SystemExit("stage_sweep: do not combine --warp-placement with PETARI_STAGE_WARP")
        placement = resolve_placement(args.disc / "files", stage["stage"], scenario["scenario"], args.warp_placement)
        env_extra["PETARI_STAGE_WARP"] = ",".join(str(v) for v in placement["position"])
        extra["PETARI_STAGE_WARP"] = env_extra["PETARI_STAGE_WARP"]
        (run_dir / "warp-placement.json").write_text(json.dumps(placement, indent=2) + "\n")
    if args.frozen_seeds:
        env_extra["PETARI_PIPELINE_SEED_DIR"] = str(args.frozen_seeds)
    if args.shader_seed_usable:
        # Read-only canonical Metal cache for this exact build; the app clones it per instance.
        env_extra["PETARI_SHADER_SEED_DIR"] = str(shader_seed_path(args))
    env = dict(os.environ, **env_extra)
    env["PETARI_LOCK_CLASS"] = args.lock_class
    # Diagnostic destinations inherited from another run must not be shared.
    for variable, filename in (("PETARI_SOAK_CSV", "soak.csv"), ("PETARI_SPIKE_PROFILE", "spikes.json")):
        if env.get(variable):
            env[variable] = str(run_dir / filename)
    # The alarm starts inside the lock (perl keeps it across exec), so waiting
    # for another worker's app does not count against this run.
    full = [str(REPO / args.lock_script), args.owner, "/usr/bin/perl", "-e", "alarm shift; exec @ARGV or die",
            str(args.timeout)] + command
    repro = " ".join(f"{k}={v}" for k, v in env_extra.items()) + " " + " ".join(command)
    (run_dir / "command.txt").write_text(repro + "\n")
    print(f"[run ] {name} ({stage['dome_name']}): {scenario['name']}", flush=True)
    started = time.time()
    log_path = run_dir / "app.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(full, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        watcher = ConcurrencyWatcher(user)
        watcher.start()
        timed_out = False
        try:
            # Generous outer bound: lock waits plus the in-lock alarm.
            status = process.wait(timeout=args.timeout + args.lock_wait)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                status = process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                status = process.wait()
    watcher.stop()
    # locked-app.sh exits with the app's status; 128+14 is the in-lock alarm.
    if status == 128 + signal.SIGALRM:
        timed_out = True
    result = analyze(log_path, run_dir / "frames.csv", stage["stage"], status, timed_out, user)
    result["concurrent_apps"] = watcher.summary()
    result["performance_evidence"] = args.lock_class == "quiet" and args.jobs == 1 and not result["concurrent_apps"]["seen"] and not extra and not result.get("instrumented")
    if result["concurrent_apps"]["seen"] and result.get("frame_times"):
        result["frame_times"]["concurrent"] = True
    if result["outcome"] == "TIMEOUT" and (args.jobs > 1 or result["concurrent_apps"]["seen"]):
        # The runner alarm fired while sharing the machine (shader-compiler and CPU
        # contention). Not evidence about the game: rerun quiet or with --jobs 1.
        result["outcome"], result["signature"] = "INFRA_TIMEOUT", "runner timeout under concurrent load (not a game failure; rerun quiet)"
    if "Died at -e line" in log_path.read_text(errors="replace")[:4096]:
        # The app could not be executed at all: not a result for this stage.
        result["infra"] = "the app could not be executed"
        result["outcome"], result["signature"] = classify(result)
    result.update({
        "run": name, "stage": stage["stage"], "dome": stage["dome"], "dome_name": stage["dome_name"],
        "scenario": scenario["scenario"], "scenario_name": scenario["name"], "comet": scenario["comet"],
        "hidden": scenario["hidden"], "wall_seconds": round(time.time() - started, 1), "log": str(log_path),
        "repro": repro, "synthetic_entry": True, "app_sha256": args.app_sha256,
        # Extra environment (--env), e.g. diagnostics that make timing incomparable.
        "extra_env": extra, "jobs": args.jobs, "lock_class": args.lock_class,
    })
    if result["outcome"] == "INFRA":
        # No result.json: a later invocation retries this run.
        (run_dir / "infra.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
        print(f"[infra] {name}: {result['signature']}; stopping the sweep", flush=True)
        raise SystemExit(3)
    if args.publish_shader_seed and result["outcome"] in ("PASS", "PASS_WARN"):
        publish_shader_seed(args, user / "cache/metal/dev.petari.Petari")
    # Preserve the existing corpus layout while the app writes its private cache.
    if (user / "cache/pipeline_cache.db").is_file():
        snapshot_sqlite(user / "cache/pipeline_cache.db", user / "pipeline_cache.db")
    write_result(run_dir / "result.json", result)
    # Keep the run's pipeline_cache.db (the captured configs map uncovered
    # shader hashes to their draw owners), crash reports and hang samples;
    # the Dawn blob cache is large and only a compile cache.
    if not args.keep_user:
        for directory in (user, user / "cache"):
            for db in directory.glob("dawn_cache.db*"):
                db.unlink()
        storage = result.get("storage") or {}
        if storage.get("cache") == str(user / "cache"):
            for key, target in (("namespace", user / "cache/metal"), ("temp_namespace", user / "cache/tmp")):
                alias = Path(storage[key]) if storage.get(key) else None
                if alias and alias.is_symlink() and alias.resolve() == target.resolve():
                    alias.unlink()
            shutil.rmtree(user / "cache/metal", ignore_errors=True)
    print(f"[done] {name}: {result['outcome']} {result['signature']} ({result['wall_seconds']} s)", flush=True)
    return result


def shader_seed_path(args):
    return args.shader_seed_dir / args.app_sha256[:16]


def shader_seed_valid(args):
    try:
        recorded = json.loads((shader_seed_path(args) / "seed.json").read_text()).get("exe_sha256")
    except (OSError, ValueError):
        return False
    return recorded == args.app_sha256 and (shader_seed_path(args) / "metal/dev.petari.Petari").is_dir()


def publish_shader_seed(args, metal):
    """Publish a finished clean run's private Metal cache as the canonical seed for this
    build hash (first finisher wins; atomic rename; APFS clone so it is cheap)."""
    final = shader_seed_path(args)
    if final.exists() or not metal.is_dir():
        return
    args.shader_seed_dir.mkdir(parents=True, exist_ok=True)
    partial = final.with_name(final.name + f".partial-{os.getpid()}")
    shutil.rmtree(partial, ignore_errors=True)
    (partial / "metal").mkdir(parents=True)
    subprocess.run(["cp", "-cR", str(metal), str(partial / "metal/dev.petari.Petari")], check=True)
    (partial / "seed.json").write_text(json.dumps({"exe_sha256": args.app_sha256, "published": time.time(),
                                                   "from_sweep": str(args.name)}, indent=2) + "\n")
    try:
        partial.rename(final)
        print(f"[seed] published canonical shader seed {final.name}", flush=True)
    except OSError:
        shutil.rmtree(partial, ignore_errors=True)  # another run won the race


def claim_sweep(out):
    """One runner per sweep directory: a second one would rerun the same stages
    and double the app-lock time. The claim is released at exit."""
    marker = out / ".sweep.pid"
    try:
        fd = os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        try:
            pid = int(marker.read_text().split()[0])
            os.kill(pid, 0)
            raise SystemExit(f"stage_sweep: {out} is already being run by PID {pid}")
        except (ValueError, IndexError, ProcessLookupError):
            marker.unlink(missing_ok=True)
            return claim_sweep(out)
    os.write(fd, f"{os.getpid()} {' '.join(sys.argv)}\n".encode())
    os.close(fd)
    import atexit
    atexit.register(lambda: marker.unlink(missing_ok=True))


def freeze_app(args, out):
    """Runs use a private copy of the app bundle: other workers rebuild the
    shared build tree, and a sweep must not mix binaries (or lose the app)."""
    bundle = args.app.resolve()
    while bundle.suffix != ".app" and bundle != bundle.parent:
        bundle = bundle.parent
    if bundle.suffix != ".app":
        raise SystemExit(f"stage_sweep: {args.app} is not inside an .app bundle")
    frozen = out / "app" / bundle.name
    if args.refreeze_app and frozen.exists():
        shutil.rmtree(frozen)
    if not frozen.exists():
        tmp = out / "app" / (bundle.name + ".partial")
        if tmp.exists():
            shutil.rmtree(tmp)
        shutil.copytree(bundle, tmp, symlinks=True)
        tmp.rename(frozen)
    args.app = frozen / args.app.resolve().relative_to(bundle)
    args.app_sha256 = hashlib.sha256(args.app.read_bytes()).hexdigest()
    (out / "app" / "app.json").write_text(json.dumps({"source": str(bundle), "executable": str(args.app),
                                                     "sha256": args.app_sha256}, indent=2) + "\n")
    print(f"app: {args.app} sha256 {args.app_sha256[:16]}")


# --- summary -----------------------------------------------------------------------------------

def fmt(value, suffix=""):
    return "" if value is None else f"{value}{suffix}"


def summarize(out):
    results = []
    for path in sorted((out / "runs").glob("*/result.json")):
        results.append(json.loads(path.read_text()))
    results.sort(key=lambda r: (r.get("dome") or 99, r["stage"], r["scenario"]))
    (out / "results.json").write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n")
    columns = ["run", "dome_name", "stage", "scenario", "scenario_name", "outcome", "signature", "exit_status",
               "load_to_ready_seconds", "p50_ms", "p95_ms", "p99_ms", "max_ms", "late", "over_100ms",
               "stage_first_use_configs", "stage_first_use_uncovered", "compiles", "compile_failures",
               "blocking_resolves", "blocking_resolve_max_ms", "min_heap_free_pct", "file_cache_archives", "file_cache_archive_bytes", "file_cache_other_use",
               "spilled_to_scene_gddr", "audio_seconds", "audio_underrun_frames", "audio_replayed_blocks",
               "dsp_holds", "dsp_holds_per_s", "audio_choppy", "warn_checks", "wall_seconds", "app_sha256", "log", "jobs", "lock_class", "performance_evidence", "silent_audio"]
    with (out / "results.csv").open("w", newline="") as handle:
        heap_names = sorted({name for r in results for name in (r.get("heaps") or {})})
        columns = columns + [f"heap_min_free[{n}]" for n in heap_names] + [f"heap_min_free_pct[{n}]" for n in heap_names]
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for r in results:
            ft = r.get("frame_times") or {}
            sh = r.get("shaders") or {}
            heaps = r.get("heaps") or {}
            writer.writerow({
                **{k: r.get(k) for k in ("run", "dome_name", "stage", "scenario", "scenario_name", "outcome",
                                         "signature", "exit_status", "load_to_ready_seconds", "wall_seconds", "app_sha256", "log", "jobs", "lock_class", "performance_evidence", "silent_audio")},
                **{k: ft.get(k) for k in ("p50_ms", "p95_ms", "p99_ms", "max_ms", "late", "over_100ms")},
                **{k: sh.get(k) for k in ("stage_first_use_configs", "stage_first_use_uncovered", "compiles",
                                          "compile_failures", "blocking_resolves", "blocking_resolve_max_ms")},
                "file_cache_archives": (r.get("file_cache_archives") or {}).get("resident"),
                "file_cache_archive_bytes": (r.get("file_cache_archives") or {}).get("bytes"),
                "file_cache_other_use": (r.get("file_cache_archives") or {}).get("other_use"),
                "audio_seconds": (r.get("audio") or {}).get("seconds"),
                "audio_underrun_frames": (r.get("audio") or {}).get("underrun_frames"),
                "audio_replayed_blocks": (r.get("audio") or {}).get("replayed_blocks"),
                "dsp_holds": (r.get("audio") or {}).get("dsp_holds"),
                "dsp_holds_per_s": (r.get("audio") or {}).get("dsp_holds_per_s"),
                "audio_choppy": (r.get("audio") or {}).get("choppy"),
                "spilled_to_scene_gddr": len((r.get("archive_mounts") or {}).get("scene_gddr", [])),
                "min_heap_free_pct": min((h.get("min_free_pct", h["free_pct"]) for h in heaps.values()), default=None),
                **{f"heap_min_free[{name}]": h.get("min_free", h["free"]) for name, h in heaps.items()},
                **{f"heap_min_free_pct[{name}]": h.get("min_free_pct", h["free_pct"]) for name, h in heaps.items()},
                "warn_checks": " ".join(c["name"] for c in r.get("checks", []) if c["status"] == "warn"),
            })

    counts = {}
    for r in results:
        counts[r["outcome"]] = counts.get(r["outcome"], 0) + 1
    lines = [
        f"# Stage sweep: {out.name}",
        "",
        f"Generated {datetime.datetime.now().isoformat(timespec='seconds')} from {len(results)} runs.",
        "",
        "**Synthetic entry.** Each run loads the saved file and the game's after-loading galaxy move enters the",
        "stage and scenario directly (`--test-fixture stage`). A PASS means the stage loaded, became playable,",
        "and survived idle/walk/jump/spin/camera/pause input without a crash, hang or heap failure. It makes no",
        "claim about progression, unlock conditions or completing the mission. Movement/camera checks are",
        "reported as warnings (stage starts differ), not failures.",
        "",
        "Frame times are only performance evidence when no other Petari app ran at the same time: runs that",
        "overlapped another app are marked \"concurrent run\", runs from before that was recorded \"concurrency not recorded\".",
        "",
        "Outcomes: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())),
        "",
        "| Dome | Stage | Sc | Outcome | Load→ready s | p50/p95/p99/max ms | late | unfocused | first-use (uncovered) | blocking resolves (max ms) | min heap free (per heap) | warnings |",
        "| --- | --- | --: | --- | --: | --- | --: | --: | --- | --- | --: | --- |",
    ]
    for r in results:
        ft = r.get("frame_times") or {}
        sh = r.get("shaders") or {}
        heaps = r.get("heaps") or {}
        heapcell = "<br>".join(f"{n} {h.get('min_free_pct', h['free_pct'])}% ({h.get('min_free', h['free']) // 1024} KiB)"
                               + (" **WARN**" if h.get("min_free_pct", h["free_pct"]) < HEAP_WARN_PCT else "")
                               for n, h in heaps.items())
        audio = r.get("audio") or {}
        warns = ", ".join([c["name"] for c in r.get("checks", []) if c["status"] == "warn"] +
                          ["heap " + w for w in r.get("heap_warnings", [])] +
                          ([f"**AUDIO-CHOPPY** ({audio['dsp_holds_per_s']} DSP holds/s)"] if audio.get("choppy") else []))
        perf = f"{ft.get('p50_ms')}/{ft.get('p95_ms')}/{ft.get('p99_ms')}/{ft.get('max_ms')}" if ft else ""
        if ft and (r.get("concurrent_apps") or {}).get("seen"):
            perf += " (concurrent run — not performance evidence)"
        elif ft and r.get("jobs", 1) > 1:
            perf += " (parallel batch — not performance evidence)"
        elif ft and r.get("instrumented"):
            perf += " (instrumented — not performance evidence)"
        elif ft and r.get("lock_class") == "functional":
            perf += " (functional run — not performance evidence)"
        elif ft and "concurrent_apps" not in r:
            perf += " (concurrency not recorded)"
        lines.append(
            f"| {r['dome_name']} | {r['stage']} | {r['scenario']} | {r['outcome']} | {fmt(r.get('load_to_ready_seconds'))} | "
            f"{perf} | {fmt(ft.get('late'))} | {fmt(ft.get('unfocused'))} | {fmt(sh.get('stage_first_use_configs'))} ({fmt(sh.get('stage_first_use_uncovered'))}) | "
            f"{fmt(sh.get('blocking_resolves'))} ({fmt(round(sh.get('blocking_resolve_max_ms', 0)))}) | "
            f"{heapcell} | {warns} |")
    groups = {}
    for r in results:
        if r["outcome"] in ("PASS", "PASS_WARN"):
            continue
        groups.setdefault((r["outcome"], r["signature"]), []).append(r)
    lines += ["", "## Distinct non-pass signatures", ""]
    if not groups:
        lines.append("None.")
    for (outcome, signature), members in sorted(groups.items()):
        first = members[0]
        lines += [f"### {outcome}: {signature or '(no detail)'}", "",
                  "Runs: " + ", ".join(m["run"] for m in members), "",
                  f"Reason: {first.get('smoke_reason') or ''}", ""]
        if first.get("crash"):
            lines += ["```", first["crash"]["signal"], "panic: " + first["crash"]["panic"]] + first["crash"]["top_frames"] + ["```", ""]
        if first.get("hang_samples"):
            lines.append("Hang samples: " + ", ".join(first["hang_samples"]))
        lines += [f"Log: `{first['log']}`", "", "Repro:", "```", first["repro"], "```", ""]
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print(f"wrote {out / 'summary.md'}, {out / 'results.csv'}, {out / 'results.json'}")
    return results


# --- CLI ---------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("list", "run", "summarize", "reanalyze"):
        p = sub.add_parser(name)
        p.add_argument("--disc", type=Path, default=REPO / "build/game-data/RMGE01")
        p.add_argument("--out-root", type=Path, default=REPO / "build/stage-sweep")
        p.add_argument("--name", default="default", help="sweep name: build/stage-sweep/<name>")
        p.add_argument("--domes", help="dome numbers, e.g. 1,2 (1 Terrace, 2 Fountain, ...)")
        p.add_argument("--stages", help="comma-separated stage names")
        p.add_argument("--scenarios", help="scenario numbers, e.g. 1-3")
        p.add_argument("--all", action="store_true", help="every stage with scenario data")
        p.add_argument("--json", action="store_true")
    run = sub.choices["run"]
    run.add_argument("--app", type=Path, default=REPO / "build/macos-gx/native/app/Petari.app/Contents/MacOS/Petari")
    run.add_argument("--source", type=Path, default=REPO / "build/observatory-user-2",
                     help="user directory with the saved file (only read)")
    run.add_argument("--pipeline-cache-from", type=Path, default=REPO / "build/observatory-user-2",
                     help="user directory whose pipeline/dawn caches every run starts from (snapshotted once)")
    run.add_argument("--cold", action="store_true", help="start every run with no pipeline cache")
    run.add_argument("--seed-dir", default=None,
                     help="PETARI_PIPELINE_SEED_DIR, snapshotted once per sweep (default: the frozen app's own "
                          "Resources/pipeline-seeds; shared seed directories are regenerated by other workers)")
    run.add_argument("--owner", default="stage-sweep")
    run.add_argument("--lock-script", default="build/locked-app.sh",
                     help="per-run app slot lock (default: build/locked-app.sh)")
    run.add_argument("--jobs", type=int, default=1, help="concurrent scenarios; every app still acquires a slot")
    run.add_argument("--lock-class", choices=("functional", "quiet"), default="functional",
                     help="functional slot or exclusive quiet run")
    run.add_argument("--timeout", type=int, default=600, help="seconds per app run, counted inside the app lock")
    run.add_argument("--lock-wait", type=int, default=7200, help="extra seconds allowed for waiting on the app lock")
    run.add_argument("--frames", type=int, default=12000, help="PETARI_SMOKE_FRAMES")
    run.add_argument("--idle-frames", type=int, default=600, help="PETARI_STAGE_IDLE_FRAMES")
    run.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                     help="extra app environment for every run (recorded per run; e.g. PETARI_PIPELINE_OWNERS=1 "
                          "for draw-owner stacks; such runs are not timing data)")
    run.add_argument("--reuse-from", type=lambda v: [x for x in v.split(",") if x], default=[],
                     help="comma-separated sweep names whose finished runs on the identical app sha are copied in")
    run.add_argument("--warp-placement", metavar="NAME[:FIELD=integer][@index]",
                     help="warp to an active root-zone placement; records source archive, row and coordinates")
    run.add_argument("--rerun", action="store_true")
    run.add_argument("--refreeze-app", action="store_true",
                     help="copy the app bundle again (default: reuse this sweep's frozen copy)")
    run.add_argument("--shader-seed-dir", type=Path, default=REPO / "build/shader-seed",
                     help="canonical Metal shader seeds, one directory per app sha (read-only for runs; "
                          "stale or missing seeds fall back to the shared cache)")
    run.add_argument("--pipeline-policy", choices=("blocking", "default"), default="blocking",
                     help="PETARI_PIPELINE_POLICY for every run (default blocking: skipped draws cannot hide "
                          "renderer failures; 'default' = the app's own policy, functional verdicts only)")
    run.add_argument("--publish-shader-seed", action="store_true",
                     help="publish the first clean PASS run's private Metal cache as this build's canonical seed")
    run.add_argument("--keep-user", action="store_true", help="also keep each run's Dawn blob cache")
    args = parser.parse_args()

    files = args.disc / "files"
    out = args.out_root / args.name
    if args.command == "summarize":
        summarize(out)
        return
    if args.command == "reanalyze":
        # Re-derive every result from its log with the current parser.
        for path in sorted((out / "runs").glob("*/result.json")):
            old = json.loads(path.read_text())
            old["log"] = str(path.parent / "app.log")
            new = analyze(path.parent / "app.log", path.parent / "frames.csv", old["stage"], old["exit_status"],
                          old["timed_out"], path.parent / "user")
            keep = {k: old[k] for k in ("run", "stage", "dome", "dome_name", "scenario", "scenario_name", "comet",
                                        "hidden", "wall_seconds", "log", "repro", "synthetic_entry", "app_sha256",
                                        "extra_env", "reused_from", "concurrent_apps", "jobs", "lock_class", "performance_evidence")
                    if k in old}
            if (keep.get("concurrent_apps") or {}).get("seen") and new.get("frame_times"):
                new["frame_times"]["concurrent"] = True
            if new.get("instrumented"):
                keep["performance_evidence"] = False
            path.write_text(json.dumps({**new, **keep}, indent=2, ensure_ascii=False) + "\n")
        summarize(out)
        return
    stages = enumerate_stages(files)
    if args.command == "list":
        if not (args.domes or args.stages or args.all):
            args.all = True
        chosen = select(stages, args)
        if args.json:
            print(json.dumps([{"stage": s["stage"], "dome": s["dome"], "dome_name": s["dome_name"], **c} for s, c in chosen],
                             ensure_ascii=False, indent=2))
        else:
            for s, c in chosen:
                print(f"{s['dome_name']:<12} {s['stage']:<28} {c['scenario']:>2} {'hidden ' if c['hidden'] else ''}"
                      f"{('comet ' + c['comet'] + ' ') if c['comet'] else ''}{c['name']}")
            print(f"{len(chosen)} runs across {len({s['stage'] for s, _ in chosen})} stages")
        return
    if args.jobs < 1 or args.jobs > 3:
        raise SystemExit("stage_sweep: --jobs must be 1..3")
    if args.lock_class == "quiet" and args.jobs != 1:
        raise SystemExit("stage_sweep: quiet runs require --jobs 1")
    chosen = select(stages, args)
    if not chosen:
        raise SystemExit("stage_sweep: nothing selected (use --domes, --stages or --all)")
    if not args.app.is_file():
        raise SystemExit(f"stage_sweep: no app at {args.app}")
    out.mkdir(parents=True, exist_ok=True)
    claim_sweep(out)
    args.source = args.source.resolve()
    args.disc = args.disc.resolve()
    baseline = prepare_baseline(out, None if args.cold else args.pipeline_cache_from.resolve())
    fixture = load_fixture_module()
    freeze_app(args, out)
    args.frozen_seeds = None
    args.shader_seed_dir = args.shader_seed_dir.resolve()
    args.shader_seed_usable = shader_seed_valid(args)
    print(f"shader seed: {'canonical ' + shader_seed_path(args).name if args.shader_seed_usable else 'none for this build (fallback: shared cache)'}")
    if args.seed_dir:
        seeds = out / "pipeline-seeds"
        if not seeds.is_dir():
            tmp = out / "pipeline-seeds.partial"
            shutil.rmtree(tmp, ignore_errors=True)
            tmp.mkdir()
            for item in Path(args.seed_dir).iterdir():
                if item.suffix == ".db":
                    snapshot_sqlite(item, tmp / item.name)
                elif item.is_file():
                    shutil.copy2(item, tmp / item.name)
            tmp.rename(seeds)
        args.frozen_seeds = seeds.resolve()
    (out / "selection.json").write_text(json.dumps([{"stage": s["stage"], "scenario": c["scenario"]} for s, c in chosen], indent=2) + "\n")
    print(f"{len(chosen)} runs; output {out}")
    run_selected(chosen, args.jobs,
                 lambda stage, scenario: run_one(args, out, baseline, fixture, stage, scenario),
                 lambda: summarize(out))


if __name__ == "__main__":
    main()

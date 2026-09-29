#!/usr/bin/env python3
"""Whole-game stage sweep: crash/hang/heap/shader/perf coverage per galaxy and mission.

Each run launches the app once (through the sweep app lane, build/locked-sweep-lane.sh)
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
HEAP_WARN_PCT = 10.0  # a heap with less free at any report after the stage entry is a WARN
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
NOISE_FRAMES = ("libsystem_", "libdyld", "Crash::", "crashHandler", "signalHandler", "_sigtramp", "abort", "__pthread")


def normalize(text):
    text = re.sub(r"0x[0-9a-fA-F]+", "0x?", text)
    text = re.sub(r"\d+(\.\d+)?", "N", text)
    return text.strip()


def analyze(log_path, csv_path, stage, exit_status, timed_out, user):
    text = log_path.read_text(errors="replace") if log_path.is_file() else ""
    lines = text.splitlines()
    result = {"exit_status": exit_status, "timed_out": timed_out}

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
        result["crash"] = {"signal": signal_line, "panic": panic, "top_frames": frames_seen[:8]}
    result["heap_failure"] = "Native heap allocation failed" in text
    result["os_fatal"] = [l for l in lines if l.startswith("OSFatal:")][:5]
    hang_samples = sorted(str(p) for p in (user / "Crashes").glob("petari-hang-*.sample.txt")) if user.is_dir() else []
    result["hang_samples"] = hang_samples
    result["hang_lines"] = [l for l in lines if l.startswith("[hang]") or "watchdog" in l.lower()][:20]

    # Shaders.
    configs = re.findall(r"^\[gx stage config\] stage=(\S+) config=(\w+) phase=(\w+) covered=(\d)", text, re.M)
    mine = [c for c in configs if c[0] == stage]
    compile_status = re.findall(r"^\[gx pipeline compile\] .*status=(\w+)", text, re.M)
    resolves = [float(v) for v in re.findall(r"^\[gx pipeline\] blocking resolve ([\d.]+) ms", text, re.M)]
    ready = re.findall(r"^\[gx stage ready\] stage=" + re.escape(stage) + r" residual_wait_ms=([\d.]+) pending=(\d+) failed=(\d+) result=(\w+)", text, re.M)
    result["shaders"] = {
        "stage_first_use_configs": len({c[1] for c in mine}),
        "stage_first_use_uncovered": len({c[1] for c in mine if c[3] == "0"}),
        "all_first_use_uncovered": len({c[1] for c in configs if c[3] == "0"}),
        "compiles": len(compile_status),
        "compile_failures": sum(1 for s in compile_status if s != "ready"),
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
    result["heap_warnings"] = [f"{name} {h['min_free_pct']}% ({h['min_free']} bytes) free"
                               for name, h in heaps.items() if h["min_free_pct"] < HEAP_WARN_PCT]
    # A stage without a per-stage seed file: the renderer logs the absent
    # manifest as "manifest read failed: unable to open database file" and
    # falls back to shared-observed-memory (PIPELINE_SEEDING.md). Expected.
    missing_seed = re.compile(r"^\[gx stage prep\] stage=\S+ manifest read failed: unable to open database file$")
    sources = re.findall(r"^\[gx stage prep\] stage=" + re.escape(stage) + r" source=(\S+)", text, re.M)
    result["shaders"]["seed_source"] = sources[-1] if sources else None
    errors = [l for l in lines if re.match(r"^\[(aurora|gx[^\]]*|dawn|webgpu)\]", l, re.I)
              and re.search(r"(?i)\b(error|invalid|validation|lost)\b|failed(?!=0)", l)
              and "failed=0" not in l and not missing_seed.match(l)]
    result["renderer_errors"] = errors[:20]
    result["renderer_error_count"] = len(errors)
    # Asset guards (native/tools/LAYOUT_REFERENCE_GUARDS.md): each missing
    # (kind, layout, name) is logged once; the seam turns a PASS into FAIL.
    result["missing_assets"] = sorted({f"{area} {kind} {layout}/{name}" for area, kind, layout, name in re.findall(
        r'^\[(layout|sound)\] missing (\w+) layout="([^"]*)" name="([^"]*)"', text, re.M)} | {
        f"{area} {rest.strip()}" for area, rest in re.findall(r'^\[(layout|sound)\] missing (?!\w+ layout=)(.*)', text, re.M)})
    result["assisted_inputs"] = len(re.findall(r"physical input while", text))
    result["fixture_lines"] = [l for l in lines if l.startswith("PETARI FIXTURE")]
    traced = re.findall(r"^Petari trace: frame \d+.*? at ([\d.]+) s", text, re.M)
    result["app_seconds"] = float(traced[-1]) if traced else None  # game time, excluding app-lock waits

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
        return "CRASH", "crash: " + normalize(key)
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
        warned = any(c["status"] == "warn" for c in r["checks"]) or r.get("heap_warnings")
        return ("PASS_WARN" if warned else "PASS"), ""
    if result == "ASSISTED":
        return "ASSISTED", "physical input"
    if result == "BLOCKED":
        return "BLOCKED", normalize(reason.split(": ", 1)[-1])
    body = reason.split(": ", 1)[-1]
    if "FAIL: missing asset references" in reason or (r.get("missing_assets") and result == "FAIL"):
        return "MISSING_ASSET", "; ".join(r.get("missing_assets") or []) or normalize(reason)
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
        # The global shader precompile (default on when __global__.db exists)
        # can block startup for ~20 min on a cold cache and competes with the
        # measured gameplay; off until the pipeline worker has evaluated it.
        "PETARI_PIPELINE_GLOBAL_PRECOMPILE": os.environ.get("PETARI_PIPELINE_GLOBAL_PRECOMPILE", "0"),
    }


def run_one(args, out, baseline, fixture, stage, scenario):
    name = f"{stage['stage']}-s{scenario['scenario']}"
    run_dir = out / "runs" / name
    if (run_dir / "result.json").is_file() and not args.rerun:
        print(f"[skip] {name}: result exists (use --rerun)")
        return json.loads((run_dir / "result.json").read_text())
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    user = run_dir / "user"
    fixture.create(args.source, user, "stage")
    for db in baseline.glob("*.db"):
        shutil.copy2(db, user / db.name)
    command, env_extra = app_command(args, user, stage["stage"], scenario["scenario"])
    env_extra["PETARI_FRAME_CSV"] = str(run_dir / "frames.csv")
    if args.frozen_seeds:
        env_extra["PETARI_PIPELINE_SEED_DIR"] = str(args.frozen_seeds)
    env = dict(os.environ, **env_extra)
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
    # locked-app.sh exits with the app's status; 128+14 is the in-lock alarm.
    if status == 128 + signal.SIGALRM:
        timed_out = True
    result = analyze(log_path, run_dir / "frames.csv", stage["stage"], status, timed_out, user)
    if "Died at -e line" in log_path.read_text(errors="replace")[:4096]:
        # The app could not be executed at all: not a result for this stage.
        result["infra"] = "the app could not be executed"
        result["outcome"], result["signature"] = classify(result)
    result.update({
        "run": name, "stage": stage["stage"], "dome": stage["dome"], "dome_name": stage["dome_name"],
        "scenario": scenario["scenario"], "scenario_name": scenario["name"], "comet": scenario["comet"],
        "hidden": scenario["hidden"], "wall_seconds": round(time.time() - started, 1), "log": str(log_path),
        "repro": repro, "synthetic_entry": True, "app_sha256": args.app_sha256,
    })
    if result["outcome"] == "INFRA":
        # No result.json: a later invocation retries this run.
        (run_dir / "infra.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
        print(f"[infra] {name}: {result['signature']}; stopping the sweep", flush=True)
        raise SystemExit(3)
    (run_dir / "result.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    # Keep the run's pipeline_cache.db (the captured configs map uncovered
    # shader hashes to their draw owners), crash reports and hang samples;
    # the Dawn blob cache is large and only a compile cache.
    if not args.keep_user:
        for db in user.glob("dawn_cache.db*"):
            db.unlink()
    print(f"[done] {name}: {result['outcome']} {result['signature']} ({result['wall_seconds']} s)", flush=True)
    return result


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
               "blocking_resolves", "blocking_resolve_max_ms", "min_heap_free_pct", "warn_checks", "wall_seconds", "log"]
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
                                         "signature", "exit_status", "load_to_ready_seconds", "wall_seconds", "log")},
                **{k: ft.get(k) for k in ("p50_ms", "p95_ms", "p99_ms", "max_ms", "late", "over_100ms")},
                **{k: sh.get(k) for k in ("stage_first_use_configs", "stage_first_use_uncovered", "compiles",
                                          "compile_failures", "blocking_resolves", "blocking_resolve_max_ms")},
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
        warns = ", ".join([c["name"] for c in r.get("checks", []) if c["status"] == "warn"] +
                          ["heap " + w for w in r.get("heap_warnings", [])])
        perf = f"{ft.get('p50_ms')}/{ft.get('p95_ms')}/{ft.get('p99_ms')}/{ft.get('max_ms')}" if ft else ""
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
    run.add_argument("--lock-script", default="build/locked-sweep-lane.sh",
                     help="per-run app lock: the sweep lane (one sweep app beside one other app); "
                          "build/locked-app.sh for the single app lock")
    run.add_argument("--timeout", type=int, default=600, help="seconds per app run, counted inside the app lock")
    run.add_argument("--lock-wait", type=int, default=7200, help="extra seconds allowed for waiting on the app lock")
    run.add_argument("--frames", type=int, default=12000, help="PETARI_SMOKE_FRAMES")
    run.add_argument("--idle-frames", type=int, default=600, help="PETARI_STAGE_IDLE_FRAMES")
    run.add_argument("--rerun", action="store_true")
    run.add_argument("--refreeze-app", action="store_true",
                     help="copy the app bundle again (default: reuse this sweep's frozen copy)")
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
                                        "hidden", "wall_seconds", "log", "repro", "synthetic_entry", "app_sha256")
                    if k in old}
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
    for stage, scenario in chosen:
        run_one(args, out, baseline, fixture, stage, scenario)
        summarize(out)


if __name__ == "__main__":
    main()

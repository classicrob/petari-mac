#!/usr/bin/env python3
"""Run the dome tour (PETARI_SMOKE=domes, native/app/smoke_domes.hpp) per dome and
tabulate every galaxy visit.

Each dome runs in its own bounded app session on a fresh copy of an unlocked
save (default build/saves/all-missions; the published save is only read):
file select -> observatory -> walk to the dome -> Blue Star -> each galaxy on
its map -> mission (1, plus PETARI_DOME_MISSIONS extras) -> ready -> movement
check -> pause, Back to the Comet Observatory -> next galaxy.

Per visit: PASS needs the smoke's "DOMES VISIT ... PASS" (selected through the
UI, the requested stage and scenario loaded and ready), and no [layout]/[sound]
missing reference logged between that galaxy's selection and its return. A
session that crashes (crash report, signal) or hangs (watchdog exit 124,
petari-hang-*.sample.txt kept) fails the visit in progress.

In the shared checkout run it under the app lock:
  build/locked-app.sh NAME python3 native/tools/dome_tour.py --app APP --output DIR
"""
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOMES = {1: "Terrace", 2: "Fountain", 3: "Kitchen", 4: "Bedroom", 5: "Engine Room", 6: "Garden"}
VISIT = re.compile(r"DOMES VISIT dome (\d) galaxy (\S+) scenario (\d+): (PASS|FAIL.*?) \(load (\d+) frames, ready after (\d+), moved ([\d.]+)\)")
MISSING = re.compile(r"^\[(layout|sound)\] missing")


def run_dome(dome, args, output):
    user = output / f"dome{dome}-user"
    if user.exists():
        shutil.rmtree(user)
    user.mkdir(parents=True)
    shutil.copytree(args.save / "NAND", user / "NAND")
    log = output / f"dome{dome}.log"
    env = dict(__import__("os").environ, PETARI_SMOKE="domes", PETARI_DOME=str(dome), PETARI_TRACE_BOOT="1",
               PETARI_SMOKE_FRAMES=str(args.frames), TMPDIR=str(output))
    if args.missions:
        env["PETARI_DOME_MISSIONS"] = args.missions
    command = [str(args.app), "--disc", str(args.disc), "--user", str(user)]
    started = time.time()
    with log.open("w") as stream:
        try:
            status = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, env=env, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            status = "timeout"
    return log, status, time.time() - started, user


def parse(dome, log, status):
    text = log.read_text(errors="replace").splitlines()
    visits, current, missing, result = [], None, [], None
    for line in text:
        body = line.split("]: ", 1)[1] if line.startswith("PETARI SMOKE [frame") else line
        if body.startswith("tap A: Galaxy.") and not body.startswith("tap A: Galaxy.Start"):
            current = {"galaxy": body.split("Galaxy.", 1)[1].split()[0], "missing": []}
        m = VISIT.search(body)
        if m:
            visit = {"dome": int(m[1]), "galaxy": m[2], "scenario": int(m[3]), "smoke": m[4], "load": int(m[5]),
                     "ready": int(m[6]), "moved": float(m[7]), "missing": current["missing"] if current else []}
            visits.append(visit)
            current = None
        if MISSING.match(line):
            (current["missing"] if current else missing).append(line.strip())
        if line.startswith("PETARI SMOKE RESULT:"):
            result = line.split(":", 1)[1].strip()
        if line.startswith("PETARI SMOKE [frame") and "DOMES MAP" in line:
            map_line = body
    open_galaxy = current["galaxy"] if current else None
    return {"visits": visits, "unattributed_missing": missing, "result": result, "status": status,
            "in_progress": open_galaxy, "map": next((l.split("]: ", 1)[1] for l in text if "DOMES MAP" in l), None)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--disc", type=Path, default=ROOT / "build/game-data/RMGE01")
    parser.add_argument("--save", type=Path, default=ROOT / "build/saves/all-missions")
    parser.add_argument("--output", type=Path, required=True, help="new or existing evidence directory")
    parser.add_argument("--domes", default="1,2,3,4,5,6")
    parser.add_argument("--missions", default="", help="PETARI_DOME_MISSIONS, e.g. EggStarGalaxy:5")
    parser.add_argument("--frames", type=int, default=72000)
    parser.add_argument("--timeout", type=float, default=1200, help="seconds per dome session (<= 20 min)")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {}
    for dome in [int(d) for d in args.domes.split(",") if d]:
        log, status, seconds, user = run_dome(dome, args, args.output)
        parsed = parse(dome, log, status)
        parsed["seconds"] = round(seconds)
        parsed["hang_samples"] = [str(p) for p in args.output.glob("petari-hang-*.sample.txt")]
        parsed["crash_reports"] = [str(p) for p in (user / "Crashes").glob("*")] if (user / "Crashes").exists() else []
        report[dome] = parsed
        print(f"dome {dome} ({DOMES[dome]}): exit {status}, {seconds:.0f} s, {parsed['result']}")
        for v in parsed["visits"]:
            ok = v["smoke"] == "PASS" and not v["missing"]
            print(f"  {v['galaxy']} scenario {v['scenario']}: {'PASS' if ok else 'FAIL'}"
                  f" (smoke {v['smoke'][:80]}, missing refs {len(v['missing'])}, load {v['load']}, ready {v['ready']}, moved {v['moved']})")
        if parsed["in_progress"]:
            print(f"  {parsed['in_progress']}: FAIL (in progress when the session ended)")
    (args.output / "dome-tour.json").write_text(json.dumps(report, indent=2) + "\n")
    rows = ["| Dome | Galaxy | Mission | Result | Load (frames) | Ready after (frames) | Moved | Missing refs | Notes |",
            "|---|---|---|---|---|---|---|---|---|"]
    for dome, parsed in report.items():
        for v in parsed["visits"]:
            ok = v["smoke"] == "PASS" and not v["missing"]
            rows.append(f"| {dome} {DOMES[dome]} | {v['galaxy']} | {v['scenario']} | {'PASS' if ok else 'FAIL'} | {v['load']} | "
                        f"{v['ready']} | {v['moved']:.0f} | {len(v['missing'])} | {'' if v['smoke'] == 'PASS' else v['smoke']} |")
        if parsed["in_progress"]:
            rows.append(f"| {dome} {DOMES[dome]} | {parsed['in_progress']} | - | FAIL | | | | | session ended: exit {parsed['status']}, {parsed['result']} |")
        if not parsed["visits"] and not parsed["in_progress"]:
            rows.append(f"| {dome} {DOMES[dome]} | (none reached) | | FAIL | | | | | exit {parsed['status']}, {parsed['result']} |")
    (args.output / "dome-tour.md").write_text("\n".join(rows) + "\n")
    print("\n".join(rows))


if __name__ == "__main__":
    main()

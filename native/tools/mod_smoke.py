#!/usr/bin/env python3
"""Live smoke for the disc-file mod folder (native/MODS.md).

Builds a test mod from a copy of a disc file at run time (no Nintendo data is stored):
EggStarGalaxy's Yaz0 archive is decompressed, 4096 zero bytes are appended and the RARC header's size
field is raised to match (a valid, larger archive: the game must still load the stage), then installed twice,
as an uncompressed RARC and as a Yaz0 stream (literal-only encoder). Four launches of the stage
smoke on EggStarGalaxy scenario 1, each in its own fixture user directory:

  1. mod present, not enabled    -> no overlay; the stage archive loads at its disc size
  2. enabled, uncompressed RARC  -> overlay applied; the archive loads 4096 bytes larger
  3. enabled, Yaz0 replacement   -> overlay applied; the archive loads 4096 bytes larger
  4. enabled, corrupt archive    -> refused ("is not a valid archive; using the disc file"); the disc file loads

Pass/fail is read from the logs: "[mods] disc overlay applied", "[mods] game opened ...",
"[heap-arc] /StageData/EggStarGalaxy.arc -> file cache, N bytes" (the size the game mounted) and the
stage smoke's own PASS. (Moving Mario's start point is NOT a usable probe: in the synthetic stage entry
the start does not come from these rows. A corrupted archive crashes the game, which also proves its
bytes are the ones read; that is how this was checked.) One app at a time through build/locked-app.sh.

usage: mod_smoke.py --app Petari.app/Contents/MacOS/Petari --output build/mod-smoke [--owner modfolder]
"""
import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stage_sweep as sweep  # noqa: E402
from collect_pipeline_seed_inputs import decompress  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
STAGE, SCENARIO = "EggStarGalaxy", 1
PAD = 4096  # bytes appended to the archive


def yaz0_literal(data):
    """A valid Yaz0 stream made only of literals: loaders must accept it like any compressed file."""
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + bytes(8))
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append((0xFF << (8 - len(chunk))) & 0xFF)
        out += chunk
    return bytes(out)


def patched_archive(disc):
    raw = (disc / "files/StageData" / f"{STAGE}.arc").read_bytes()
    plain = bytearray(decompress(raw)) + bytes(PAD)
    plain[4:8] = struct.pack(">I", len(plain))  # the RARC header's own size field: the game mounts this many bytes
    return bytes(plain)


def run(args, name, user, mod_state):
    log = args.output / f"{name}.log"
    command, env_extra = sweep.app_command(args, user, STAGE, SCENARIO)
    import os
    env = dict(os.environ, **env_extra)
    env["PETARI_LOCK_CLASS"] = "functional"
    full = [str(ROOT / "build/locked-app.sh"), args.owner, "/usr/bin/perl", "-e", "alarm shift; exec @ARGV or die",
            str(args.timeout)] + command
    with log.open("wb") as stream:
        status = subprocess.run(full, cwd=ROOT, env=env, stdout=stream, stderr=subprocess.STDOUT).returncode
    text = log.read_text(errors="replace")
    ready = re.search(r"stage ready: frame \d+, Mario at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", text)
    return {
        "name": name, "status": status, "log": str(log), "mod_state": mod_state,
        "ready": [float(v) for v in ready.groups()] if ready else None,
        "mounted": [int(n) for n in re.findall(rf"\[heap-arc\] /StageData/{STAGE}\.arc -> file cache, (\d+) bytes", text)],
        "overlay": re.search(r"\[mods\] disc overlay applied: (\d+) files replaced, (\d+) added", text),
        "opened": re.findall(r"\[mods\] game opened (\S+) from mod (\S+) \((\d+) bytes\)", text),
        "rejected": re.findall(r"\[mods\] (\S+: \S+ is not a valid \w+; using .*)", text),
        "result": (re.findall(r"PETARI SMOKE RESULT: (\w+)", text) or [None])[-1],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--disc", type=Path, default=ROOT / "build/game-data/RMGE01")
    parser.add_argument("--source", type=Path, default=ROOT / "build/observatory-user-2")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--owner", default="modfolder")
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--frames", type=int, default=12000)
    parser.add_argument("--idle-frames", type=int, default=300)
    args = parser.parse_args()
    args.pipeline_policy = "default"  # functional check; the app's own pipeline policy
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = sweep.load_fixture_module()
    plain = patched_archive(args.disc)
    variants = {"uncompressed": plain, "yaz0": yaz0_literal(plain), "corrupt": b"XARC" + plain[4:]}

    results = []
    plan = [("1-present-not-enabled", None), ("2-enabled-uncompressed", "uncompressed"), ("3-enabled-yaz0", "yaz0"),
            ("4-enabled-corrupt", "corrupt")]
    for name, variant in plan:
        user = args.output / f"{name}-user"
        shutil.rmtree(user, ignore_errors=True)
        fixture.create(args.source, user, "stage")
        mod = user / "mods/TestMod"
        (mod / "files/StageData").mkdir(parents=True)
        (mod / "mod.txt").write_text("name=Test mod\ndescription=Moves the EggStarGalaxy start point\npriority=1\n")
        data = variants[variant or "uncompressed"]
        (mod / "files/StageData" / f"{STAGE}.arc").write_bytes(data)
        if variant:
            (user / "mods.txt").write_text("Folder.TestMod=on\n")
        results.append(run(args, name, user, variant or "off (present, not enabled)"))

    base, uncompressed, yaz0, corrupt = results
    failures = []
    disc_size = base["mounted"][0] if base["mounted"] else None
    if base["ready"] is None or base["overlay"] or base["opened"] or disc_size is None:
        failures.append("run 1: a mod that is present but not enabled must change nothing, and the archive must mount")
    for r in (uncompressed, yaz0):
        if r["ready"] is None or r["result"] != "PASS":
            failures.append(f"{r['name']}: the stage did not load and pass with the mod")
        if not r["overlay"] or r["overlay"].groups() != ("1", "0"):
            failures.append(f"{r['name']}: expected 'disc overlay applied: 1 files replaced, 0 added'")
        if not any(p == f"/StageData/{STAGE}.arc" and m == "TestMod" for p, m, _ in r["opened"]):
            failures.append(f"{r['name']}: the game never opened the mod's {STAGE}.arc")
        if disc_size is not None and r["mounted"][:1] != [disc_size + PAD]:
            failures.append(f"{r['name']}: the archive mounted {r['mounted'][:1]} bytes, expected {disc_size + PAD}")
    # A broken archive must be refused at the mod folder (validated headers), never crash the game.
    if corrupt["result"] != "PASS" or corrupt["ready"] is None:
        failures.append("4-enabled-corrupt: the game must start normally with the disc file when the mod archive is corrupt")
    if corrupt["overlay"] or corrupt["opened"] or not any("is not a valid archive; using the disc file" in line for line in corrupt["rejected"]):
        failures.append("4-enabled-corrupt: expected the 'is not a valid archive; using the disc file' log and no overlay")
    if disc_size is not None and corrupt["mounted"][:1] != [disc_size]:
        failures.append(f"4-enabled-corrupt: the archive mounted {corrupt['mounted'][:1]} bytes, expected the disc's {disc_size}")
    for r in results:
        print(f"{r['name']}: exit {r['status']} result {r['result']} overlay "
              f"{r['overlay'].groups() if r['overlay'] else None} opened {r['opened']} mounted {r['mounted'][:1]} rejected {r['rejected']}")
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures))
        return 1
    print(f"PASS: default off ({disc_size} bytes); an enabled mod replaced {STAGE}.arc as an uncompressed RARC and as a Yaz0 "
          f"stream, and the game mounted {disc_size + PAD} bytes each time and passed the stage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

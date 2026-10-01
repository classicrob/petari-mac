#!/usr/bin/env python3
"""seedpack.py: synthetic round trip, ensure() policies, and the guard that no provenance/paths/hashes are tracked."""
import glob
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "native", "tools"))
import seedpack  # noqa: E402


def make_db(path, rows):
    db = sqlite3.connect(path)
    db.executescript(seedpack.SCHEMA)
    db.execute("insert into aurora_schema values(1)")
    db.executemany("insert into pipeline_cache values(?,?,?,?,?,?)", rows)
    db.commit()
    db.close()


def row(t, h, ffu, fill):
    blob = bytes([fill]) * (8 if t == 0 else 40)
    return (t, h, 5 if t == 0 else 13, len(blob), blob, ffu)


def make_seeds(directory, extra=0):
    os.makedirs(directory)
    a, b, c = row(1, -(2 ** 62) - 5, 3, 7), row(1, 2 ** 62, 0xFFFFFFFF, 9), row(0, 4, 0, 1)
    rows = [a, b, c] + [row(1, 100 + i, i, i % 250) for i in range(extra)]
    make_db(os.path.join(directory, "__global__.db"), rows)
    make_db(os.path.join(directory, "StageA.db"), [a, b])
    make_db(os.path.join(directory, "StageB.db"), [c, a])
    # provenance JSON must never end up in a pack or expansion
    with open(os.path.join(directory, "StageA.json"), "w") as stream:
        json.dump({"path": "/Users/someone/disc", "sha": "0" * 64}, stream)


def expect(condition, message):
    if not condition:
        print("FAIL:", message)
        sys.exit(1)


def main():
    temp = tempfile.mkdtemp(prefix="seedpack-test-")
    try:
        seeds = os.path.join(temp, "seeds")
        make_seeds(seeds)
        pack_dir = os.path.join(temp, "data")
        os.makedirs(pack_dir)
        pack_path = os.path.join(pack_dir, "pipeline-seeds.v13.xz")
        info = seedpack.pack(seeds, pack_path)
        expect(info["configs"] == 3 and info["stages"] == 2, f"pack info {info}")
        first = open(pack_path, "rb").read()
        seedpack.pack(seeds, pack_path)
        expect(first == open(pack_path, "rb").read(), "pack is not deterministic")
        expect(not seedpack.check(pack_path), f"check: {seedpack.check(pack_path)}")

        out = os.path.join(temp, "out")
        seedpack.expand(pack_path, out)
        expect(not glob.glob(os.path.join(out, "*.json")), "expansion contains json")
        expect(seedpack.verify(seeds, out) == 0, "round trip differs")

        # ensure(): absent -> expand; current -> no-op; local (no marker) -> untouched
        target = os.path.join(temp, "build-seeds")
        seedpack.ensure(pack_dir, target)
        expect(seedpack.verify(seeds, target) == 0, "ensure did not expand")
        stamp = os.path.getmtime(os.path.join(target, "__global__.db"))
        seedpack.ensure(pack_dir, target)
        expect(os.path.getmtime(os.path.join(target, "__global__.db")) == stamp, "up-to-date seeds were rewritten")
        local = os.path.join(temp, "local")
        make_seeds(local, extra=5)
        before = open(os.path.join(local, "__global__.db"), "rb").read()
        seedpack.ensure(pack_dir, local)
        expect(open(os.path.join(local, "__global__.db"), "rb").read() == before and not os.path.exists(os.path.join(local, seedpack.MARKER)),
               "local seeds were overwritten")

        # a newer pack replaces an untouched expansion, but not a modified one
        make_seeds(os.path.join(temp, "newer"), extra=4)
        seedpack.pack(os.path.join(temp, "newer"), os.path.join(pack_dir, "pipeline-seeds.v14.xz"))
        modified = os.path.join(temp, "modified")
        shutil.copytree(target, modified)
        seedpack.ensure(pack_dir, target)
        expect(len(seedpack.database_rows(os.path.join(target, "__global__.db"))) == 7, "newer pack not applied")
        db = sqlite3.connect(os.path.join(modified, "__global__.db"))
        db.execute("delete from pipeline_cache where type=0")
        db.commit()
        db.close()
        seedpack.ensure(pack_dir, modified)
        expect(len(seedpack.database_rows(os.path.join(modified, "__global__.db"))) == 2, "modified seeds were overwritten")

        # broken / tampered packs never raise and never touch the output
        os.remove(os.path.join(pack_dir, "pipeline-seeds.v14.xz"))
        os.remove(os.path.join(pack_dir, "pipeline-seeds.v14.xz.sha256"))
        with open(pack_path, "ab") as stream:
            stream.write(b"x")
        fresh = os.path.join(temp, "fresh")
        expect(seedpack.ensure(pack_dir, fresh) == 0 and not os.path.exists(fresh), "tampered pack was expanded")
        expect(seedpack.ensure(os.path.join(temp, "nonexistent"), fresh) == 0, "missing pack failed")
    finally:
        shutil.rmtree(temp, ignore_errors=True)

    # the committed pack: valid, and nothing provenance-like is tracked
    packs = glob.glob(os.path.join(ROOT, "native", "data", "pipeline-seeds.v*.xz"))
    for path in packs:
        problems = seedpack.check(path)
        expect(not problems, f"{path}: {problems}")
    for extra in glob.glob(os.path.join(ROOT, "native", "data", "*")):
        expect(extra.endswith((".xz", ".sha256")), f"unexpected file in native/data: {extra}")
    git = subprocess.run(["git", "-C", ROOT, "ls-files", "--", "*pipeline-seeds*", "*.db"], capture_output=True, text=True)
    if git.returncode == 0:
        tracked = [p for p in git.stdout.split() if not p.startswith("native/data/pipeline-seeds.v") or p.endswith(".json")]
        expect(not tracked, f"tracked seed files other than the pack: {tracked}")
        ignored = subprocess.run(["git", "-C", ROOT, "check-ignore", "-q", "build/pipeline-seeds/Stage.json"])
        expect(ignored.returncode == 0, "build/pipeline-seeds/*.json is not git-ignored")
    print("seedpack tests passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Mod-off movement guard (no app needed).

1. native/tools/movement_baseline.py: parsing and comparison of the movement
   harness's MOVEMENT lines, and the recorded Galaxy/SMO expectations.
2. Every OdysseyMovement hook in the game sources sits in a PETARI_NATIVE block
   that checks the mod (or the mod's own state, which only the mod sets), so an
   unmodified game runs Galaxy's code unchanged.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "native/tools"))
import movement_baseline as mb  # noqa: E402

checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        print(f"FAIL: {what}")
        sys.exit(1)


SAMPLE = """PETARI SMOKE [frame 1529]: MOVEMENT standstill held jump: apex 260.000 after 20 frames, take-off speed 0.000
PETARI SMOKE [frame 1591]: MOVEMENT standstill tap jump: apex 156.400 after 13 frames, take-off speed 0.000
PETARI SMOKE [frame 1709]: MOVEMENT ground-pound jump: apex 260.000 after 20 frames, take-off speed 0.000
PETARI SMOKE [frame 1760]: MOVEMENT run: max speed 11.968 u/f, frame 46 reached 95%; every 5th frame: 0.000
PETARI SMOKE [frame 1804]: MOVEMENT running jump 1: apex 260.000 after 20 frames, take-off speed 11.968
PETARI SMOKE [frame 1859]: MOVEMENT chain jump 2: apex 345.200 after 22 frames, take-off speed 12.153
PETARI SMOKE [frame 1945]: MOVEMENT chain jump 3: apex 738.000 after 40 frames, take-off speed 12.307
PETARI SMOKE [frame 1986]: MOVEMENT run before the long jump: max speed 12.966 u/f, frame 2 reached 95%
PETARI SMOKE [frame 2224]: MOVEMENT backflip: apex 585.000 after 37 frames, take-off speed 0.000
"""


def test_baseline():
    found = mb.parse(SAMPLE)
    check(found["run"]["speed"] == 11.968, "the first run line is the run (not the run before the long jump)")
    check(found["chain jump 3"]["apex"] == 738.0, "jump heights parsed")
    check(mb.compare(found, mb.EXPECTED["off"]) == [], "Galaxy's measured movement matches the mod-off baseline")
    check(mb.compare(found, mb.EXPECTED["on"]) != [], "and not the mod's")
    changed = SAMPLE.replace("apex 156.400", "apex 150.000")
    problems = mb.compare(mb.parse(changed), mb.EXPECTED["off"])
    check(len(problems) == 1 and "standstill tap jump" in problems[0], "a changed jump height is reported")
    missing = mb.compare(mb.parse(SAMPLE.replace("backflip", "flip")), mb.EXPECTED["off"])
    check(any("backflip: not measured" in p for p in missing), "a missing move is reported")
    for mods in ("off", "on"):
        names = set(mb.EXPECTED[mods])
        check(all(any(part in name for part in mb.ONLY.split(",")) for name in names - {"run"}),
              f"every {mods} move is in the harness's task filter")


HOOK = re.compile(r"Odyssey|odyssey|petari_mod_enabled\(2\)")
GATE = re.compile(r"odysseyOn\(\)|petari_mod_enabled\((2|kModOdysseyMovement)\)|sOdyssey\.active|Live::roll\.rolling|"
                  r"Live::rollStart|Live::rollBoost|\bodyssey\b|odysseyJump|sOdysseyGroundPoundJump|groundPoundJumpWindow")
FILES = ["src/Game/Player/MarioJump.cpp", "src/Game/Player/MarioWalk.cpp", "src/Game/Player/MarioActor.cpp",
         "src/Game/Player/MarioWall.cpp"]


def native_blocks(text):
    """(first line, lines) of each '#ifdef PETARI_NATIVE' block up to its #else/#endif."""
    lines = text.splitlines()
    blocks = []
    for i, line in enumerate(lines):
        if line.strip() != "#ifdef PETARI_NATIVE":
            continue
        depth, j = 1, i + 1
        while j < len(lines):
            s = lines[j].strip()
            if s.startswith("#if"):
                depth += 1
            elif s.startswith("#endif"):
                depth -= 1
            if depth == 0 or (depth == 1 and s.startswith("#else")):
                break
            j += 1
        blocks.append((i + 1, lines[i + 1:j]))
    return blocks


def test_gating():
    hooks = 0
    for name in FILES:
        text = (ROOT / name).read_text()
        for first, block in native_blocks(text):
            code = [l for l in block if not l.strip().startswith(("//", "#include", "extern \"C\""))]
            body = "\n".join(code)
            if not HOOK.search(body) or "namespace {" in body or "struct OdysseyAir" in body:
                continue  # includes, declarations and the mod's own state
            hooks += 1
            check(GATE.search(body) is not None, f"{name}:{first}: an OdysseyMovement hook without a mod check")
        # Outside PETARI_NATIVE blocks there must be no mod code at all.
        outside = re.sub(r"#ifdef PETARI_NATIVE.*?#endif", "", text, flags=re.S)
        check(not re.search(r"PetariNative::Odyssey|sOdyssey|petari_mod_enabled", outside),
              f"{name}: mod code outside a PETARI_NATIVE block")
    check(hooks >= 15, f"the gating check found the hooks ({hooks})")


test_baseline()
test_gating()
print(f"{checks} movement baseline checks passed")

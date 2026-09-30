#!/usr/bin/env python3
"""Tests for native/gx/patch_aurora_sync.py's command_processor.cpp patch against the pinned Aurora sources.

usage: aurora_sync_patch_tests.py AURORA_DIR NATIVE_GX_DIR

Soak finding: Aurora's prepare_idx_buffer reserved ((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16)
bytes for triangle fans and strips. With fewer than 3 vertices (a legal GX draw that shows nothing)
the u32 arithmetic wraps and the static index ByteBuffer is reallocated and zeroed to about 8 GiB,
kept for the life of the process. The patched reservation is the index count the loop appends.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

checks = 0


def check(condition, label):
    global checks
    checks += 1
    if not condition:
        print(f"FAIL: {label}", file=sys.stderr)
        sys.exit(1)


def u32(value):
    return value & 0xFFFFFFFF


def old_reserve(vtx):
    return u32(u32(u32(vtx) - 3) * 3 + 3) * 2


def new_reserve(vtx):
    return (u32(u32(vtx - 3) * 3 + 3) if vtx >= 3 else u32(vtx)) * 2


def appended_indices(vtx):
    """Indices the fan/strip loop appends: one each for the first three vertices, three per later one."""
    return min(vtx, 3) + max(vtx - 3, 0) * 3


def main():
    aurora, native_gx = Path(sys.argv[1]), Path(sys.argv[2])
    source = aurora / "lib/gx/command_processor.cpp"
    with tempfile.TemporaryDirectory() as tmp:
        output = Path(tmp) / "command_processor.cpp"
        result = subprocess.run([sys.executable, str(native_gx / "patch_aurora_sync.py"), str(source), str(output)],
                                capture_output=True, text=True)
        check(result.returncode == 0, f"the patch applies: {result.stderr.strip()}")
        text = output.read_text()
        reserves = re.findall(r"buf\.reserve_extra\((.*?)\);", text, re.S)
        wrapping = [r for r in reserves if "- 3) * 3 + 3" in r and "vtxCount >= 3" not in r]
        check(not wrapping, f"no unguarded (vtxCount - 3) reservation remains: {wrapping}")
        check(sum("vtxCount >= 3" in r for r in reserves) == 2, "both the fan and the strip reservation are guarded")
        again = subprocess.run([sys.executable, str(native_gx / "patch_aurora_sync.py"), str(output), str(Path(tmp) / "x.cpp")],
                               capture_output=True, text=True)
        check(again.returncode != 0, "an already patched file is refused")

    check(old_reserve(1) > 8 * 1024**3 - 16, "the original reservation for a 1-vertex fan is about 8 GiB (the soak's 8,589,934,592-byte realloc)")
    for vtx in range(0, 2048):
        check(new_reserve(vtx) == appended_indices(vtx) * 2, f"the reservation equals the appended indices for {vtx} vertices")
    print(f"aurora sync patch tests passed ({checks} checks)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Tests for native/gx/patch_aurora_present.py against the pinned Aurora sources.

usage: present_patch_tests.py AURORA_DIR NATIVE_GX_DIR

- GXFrameBuffer.cpp and aurora.cpp patch; aurora.cpp also on top of
  patch_aurora_allocations.py output, which root runs first.
- Re-running on unchanged input leaves the output untouched (build stamps).
- Changed anchors fail loudly instead of producing a half-patched file.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

checks = 0


def check(condition, label):
    global checks
    checks += 1
    if not condition:
        print(f'FAIL: {label}', file=sys.stderr)
        sys.exit(1)


def run(script, source, output):
    return subprocess.run([sys.executable, str(script), str(source), str(output)], capture_output=True, text=True)


def main():
    aurora, gx = Path(sys.argv[1]), Path(sys.argv[2])
    present = gx / 'patch_aurora_present.py'
    allocations = gx / 'patch_aurora_allocations.py'
    framebuffer = aurora / 'lib/dolphin/gx/GXFrameBuffer.cpp'
    core = aurora / 'lib/aurora.cpp'
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        out = tmp / 'fb/GXFrameBuffer.cpp'
        check(run(present, framebuffer, out).returncode == 0, 'GXFrameBuffer.cpp patches')
        text = out.read_text()
        check('void GXCopyDisp(void* dest, GXBool clear) {}' not in text, 'GXCopyDisp is no longer empty')
        check('void GXSetCopyClamp(GXFBClamp clamp)' in text, 'GXSetCopyClamp defined')
        check(text.count('GX_AURORA_LOAD_COPY_DEST') == 2, 'display and texture copies both load a destination')
        check('u32 GXSetDispCopyYScale(f32 vscale) { return 0; }' not in text, 'Y scale returns the line count')
        before = out.stat().st_mtime_ns
        os.utime(out, ns=(before - 10**9, before - 10**9))
        stamped = out.stat().st_mtime_ns
        check(run(present, framebuffer, out).returncode == 0 and out.stat().st_mtime_ns == stamped,
              'unchanged output is not rewritten')

        out = tmp / 'core/aurora.cpp'
        check(run(present, core, out).returncode == 0, 'aurora.cpp patches')
        text = out.read_text()
        check('petari_present::take()' in text and 'petari_present::bind_image' in text
              and 'petari_present::draw_dim' in text, 'present hooks in aurora_end_frame')
        check(text.count('pass.Draw(3);') == core.read_text().count('pass.Draw(3);'),
              'the same draws, now conditional')

        alloc = tmp / 'alloc/aurora.cpp'
        check(run(allocations, core, alloc).returncode == 0, 'allocation patch runs first')
        both = tmp / 'both/aurora.cpp'
        check(run(present, alloc, both).returncode == 0, 'present patch composes after the allocation patch')
        text = both.read_text()
        check('PetariNative::HostAllocationScope petariHostAllocations;' in text
              and 'petari_present::take()' in text, 'both patches present')

        broken = tmp / 'broken/GXFrameBuffer.cpp'
        broken.parent.mkdir()
        broken.write_text(framebuffer.read_text().replace('void GXCopyDisp(void* dest, GXBool clear) {}', ''))
        result = run(present, broken, tmp / 'broken-out/GXFrameBuffer.cpp')
        check(result.returncode != 0 and 'anchor mismatch' in result.stderr + result.stdout,
              'a moved anchor fails')
        check(not (tmp / 'broken-out/GXFrameBuffer.cpp').exists(), 'no half-patched output')
        other = tmp / 'other.cpp'
        other.write_text('')
        check(run(present, other, tmp / 'x.cpp').returncode != 0, 'unknown sources are refused')
    print(f'native present patch tests passed ({checks} checks)')


if __name__ == '__main__':
    main()

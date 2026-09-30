#!/usr/bin/env python3
"""Fail on function bodies that are only Wii assembly.

A body whose only content is an `#ifdef __MWERKS__` block holding asm, with no `#else`,
compiles to an empty function on the native build. MR::PSvecBlend was one: MR::vecBlend
silently kept its old value everywhere (Mario's binder offset stuck after a launch star, so
he hovered over Good Egg's Bean B). Every such body needs a native branch, or an entry in
ALLOWED with the reason it is a host no-op.

This check finds MISSING native branches. Whether existing branches compute the same
thing as the asm is native_wii_asm_equivalence (wii_asm_equivalence_tests.py): it runs
the asm text in a paired-single interpreter and compares the native branch on the same
inputs. Audit of 2026-09-30 (every asm split in src/, libs/, include/ plus
native/src/mtx.cpp, jmath.cpp, intrinsics.cpp) checked these classes:
- argument order and which components are touched (x/y lanes vs a separate z);
- outputs aliasing inputs: in-place calls must read everything they need before
  writing, as the asm does (J3DMtxProjConcat and J3DPSMtxArrayConcat did not);
- fused multiply-add order where values cancel (JMAHermiteInterpolation's expanded
  polynomial drifted from the asm's factored form);
- quantized loads/stores: GQR2-5 are u8/u16/s8/s16 with scale 0 (J3DSys::drawInit,
  OSInitFastCast); stores saturate and truncate toward zero; NaN stores 0;
- fsel (>= 0 including -0, NaN takes the negative operand), __cntlzw(0) = 32;
- fres/frsqrte: the native versions are exact where the Wii estimates. Every refined
  use (a Newton step) converges to the same value; the unrefined ones
  (JMath::fastReciprocal for joint scale compensation, JMAFastSqrt/fastSqrt for
  Y-billboards) are only more precise on native, a visual-only difference.
Sources that are not compiled natively (the RVL GX library, whose FAST_FLAG_SET and
WriteLightObjPS have empty native forms) are out of scope.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
ROOTS = ["src", "libs", "include"]
SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".inc"}
# path -> function name: bodies that are correct as host no-ops.
ALLOWED = {
    # Loads the PPC GQR quantizers for the paired-single casts; host casts need no setup.
    "libs/RVL_SDK/include/revolution/os/OSFastCast.h": "OSInitFastCast",
}

MWERKS_IF = re.compile(r"#\s*if(?:def)?\s*(?:defined\s*\(?\s*)?__MWERKS__\)?\s*$")
ASM = re.compile(r"\basm\b|\bpsq_|\bps_|\bmtspr\b|\bmfspr\b")


def asm_only_bodies(text):
    """Yield the line opening each function body that is only an asm __MWERKS__ block."""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        if not MWERKS_IF.match(lines[i].strip()):
            i += 1
            continue
        depth, j, has_else, has_asm = 0, i + 1, False, False
        while j < len(lines):
            s = lines[j].strip()
            if re.match(r"#\s*if", s):
                depth += 1
            elif re.match(r"#\s*endif", s):
                if depth == 0:
                    break
                depth -= 1
            elif re.match(r"#\s*el", s) and depth == 0:
                has_else = True
            if ASM.search(s):
                has_asm = True
            j += 1
        if has_asm and not has_else:
            before = next((lines[k].strip() for k in range(i - 1, -1, -1) if lines[k].strip()), "")
            after = next((lines[k].strip() for k in range(j + 1, len(lines)) if lines[k].strip()), "")
            if before.endswith("{") and after.startswith("}"):
                yield before
        i = j + 1


def self_test():
    sample = "void f(Vec* p) {\n#ifdef __MWERKS__\n    asm {\n        psq_l f0, 0(p), 0, 0\n    }\n#endif\n}\n"
    covered = "void g(Vec* p) {\n#ifdef __MWERKS__\n    asm { psq_l f0, 0(p), 0, 0 }\n#else\n    p->x = 0;\n#endif\n}\n"
    assert list(asm_only_bodies(sample)) == ["void f(Vec* p) {"], "detector misses an asm-only body"
    assert list(asm_only_bodies(covered)) == [], "detector flags a body with a native branch"


def main():
    self_test()
    failures = []
    for root in ROOTS:
        for path in sorted((ROOT / root).rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(ROOT).as_posix()
            for opener in asm_only_bodies(path.read_text(encoding="utf-8", errors="replace")):
                allowed = ALLOWED.get(relative)
                if allowed is not None and re.search(r"\b" + re.escape(allowed) + r"\s*\(", opener):
                    continue
                failures.append(f"{relative}: {opener}")
    if failures:
        print("Function bodies with only __MWERKS__ assembly (empty on the native build):", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1
    print("no asm-only function bodies")
    return 0


if __name__ == "__main__":
    sys.exit(main())

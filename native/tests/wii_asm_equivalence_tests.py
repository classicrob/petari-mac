#!/usr/bin/env python3
"""Native branches vs the Wii paired-single asm they replace.

For each function, the Wii asm text is read from the source and run in the
paired-single interpreter (paired_single.py); the native branch runs in
petari_wii_asm_equivalence_harness. Both get the same representative and edge
inputs (zeros, -0, large values, singular matrices, saturation limits, NaN, and
outputs aliasing inputs where the Wii code is in-place safe), and must agree:
copies and sign flips bit for bit, arithmetic within a few float32 ulps (the Wii
fuses multiply-adds; fres/frsqrte are refined estimates on the Wii).

Usage: wii_asm_equivalence_tests.py HARNESS
"""
from pathlib import Path
import math
import random
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paired_single as ps  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MATH_UTIL = ROOT / "src/Game/Util/MathUtil.cpp"
J3D_TRANSFORM = ROOT / "src/JSystem/J3DGraphBase/J3DTransform.cpp"
J3D_SHAPE_MTX = ROOT / "src/JSystem/J3DGraphBase/J3DShapeMtx.cpp"
J3D_ANIMATION = ROOT / "src/JSystem/J3DGraphAnimator/J3DAnimation.cpp"
J3D_DRAW_BUFFER = ROOT / "libs/JSystem/include/JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
JMATH_CPP = ROOT / "src/JSystem/JMath/JMath.cpp"
JMATH_HPP = ROOT / "libs/JSystem/include/JSystem/JMath/JMath.hpp"
TVEC = ROOT / "libs/JSystem/include/JSystem/JGeometry/TVec.hpp"
OS_H = ROOT / "libs/RVL_SDK/include/revolution/os.h"
FAST_CAST = ROOT / "libs/RVL_SDK/include/revolution/os/OSFastCast.h"


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


class Case:
    """Wii side of one call: memory, register bindings, and how to read the outputs."""

    def __init__(self):
        self.mem = ps.Memory()
        self.m = ps.Machine(self.mem)

    def floats(self, values):
        address = self.mem.alloc(4 * len(values))
        self.mem.put_f32s(address, values)
        return address

    def s16s(self, values):
        address = self.mem.alloc(2 * len(values))
        self.mem.put_s16s(address, values)
        return address

    def ptr(self, name, address, abi=None):
        if abi:
            self.m.alias[name] = abi
        self.m.bind_r(name, address)

    def run(self, path, anchor, **kwargs):
        self.m.run(ps.extract_asm(path, anchor), **kwargs)


def wii(name, x):
    """Run the Wii asm for `name` on inputs `x`; return the outputs in the harness layout."""
    c = Case()
    m, mem = c.m, c.mem
    if name.startswith("JGeometry::subInternal"):
        a, b, d = c.floats(x[:3]), c.floats(x[3:6]), c.floats([0.0] * 3)
        dst = a if name.endswith("_a") else b if name.endswith("_b") else d
        c.ptr("vec1", a), c.ptr("vec2", b), c.ptr("dst", dst)
        c.run(TVEC, "inline static void subInternal(register")
        return mem.f32s(dst, 3)
    if name.startswith("SDK::PSMTXMultVec"):
        matrix, src, dst = c.floats(x[:12]), c.floats(x[12:15]), c.floats([0.0] * 3)
        if name.endswith("_inplace"):
            dst = src
        c.ptr("m", matrix), c.ptr("src", src), c.ptr("dst", dst)
        func = "PSMTXMultVecSR" if "SR" in name else "PSMTXMultVec"
        c.run(ROOT / "src/RVL_SDK/mtx/mtxvec.c", "asm void " + func + "\n")
        return mem.f32s(dst, 3)
    if name == "SDK::PSMTXQuat":
        q, matrix = c.floats(x[:4]), c.floats([0.0] * 12)
        c.ptr("q", q), c.ptr("m", matrix)
        m.bind_f("c_one", 1.0)
        c.run(ROOT / "src/RVL_SDK/mtx/mtx.c", "void PSMTXQuat (")
        return mem.f32s(matrix, 12)
    if name.startswith("SDK::PSQUATMultiply") or name == "SDK::PSQUATDotProduct":
        a, b, dst = c.floats(x[:4]), c.floats(x[4:8]), c.floats([0.0] * 4)
        if name.endswith("_a"):
            dst = a
        if name.endswith("_b"):
            dst = b
        c.ptr("p", a), c.ptr("q", b), c.ptr("pq", dst)
        multiply = "Multiply" in name
        c.run(ROOT / "src/RVL_SDK/mtx/quat.c", "void PSQUATMultiply" if multiply else "f32 PSQUATDotProduct(")
        return mem.f32s(dst, 4) if multiply else [m.f("dp")[0]]
    if name == "J3DPSCalcInverseTranspose":
        src, dst = c.floats(x[:12]), c.floats(x[12:21])
        c.ptr("src", src, "r3"), c.ptr("dst", dst, "r4")
        c.run(J3D_TRANSFORM, "asm void J3DPSCalcInverseTranspose")
        return mem.f32s(dst, 9)
    if name == "J3DScaleNrmMtx":
        mtx, scl = c.floats(x[:12]), c.floats(x[12:15])
        c.ptr("mtx", mtx, "r3"), c.ptr("scl", scl, "r4")
        c.run(J3D_TRANSFORM, "asm void J3DScaleNrmMtx(")
        return mem.f32s(mtx, 12)
    if name == "J3DScaleNrmMtx33":
        mtx, scl = c.floats(x[:9]), c.floats(x[9:12])
        c.ptr("mtx", mtx, "r3"), c.ptr("scale", scl, "r4")
        c.run(J3D_TRANSFORM, "asm void J3DScaleNrmMtx33(")
        return mem.f32s(mtx, 9)
    if name in ("J3DMtxProjConcat", "J3DMtxProjConcat_inplace"):
        a, b = c.floats(x[:12]), c.floats(x[12:28])
        dst = a if name.endswith("_inplace") else c.floats([0.0] * 12)
        c.ptr("mtx1", a, "r3"), c.ptr("mtx2", b, "r4"), c.ptr("dst", dst, "r5")
        c.run(J3D_TRANSFORM, "asm void J3DMtxProjConcat(")
        return mem.f32s(dst, 12)
    if name in ("J3DPSMtxArrayConcat", "J3DPSMtxArrayConcat_inplace"):
        count = int(x[0])
        a, b = c.floats(x[1:13]), c.floats(x[13:13 + 12 * count])
        ab = b if name.endswith("_inplace") else c.floats([0.0] * (12 * count))
        m.symbols["Unit01"] = c.floats([0.0, 1.0])
        c.ptr("mA", a, "r3"), c.ptr("mB", b, "r4"), c.ptr("mAB", ab, "r5"), c.ptr("count", count, "r6")
        c.run(J3D_TRANSFORM, "asm void J3DPSMtxArrayConcat", defines={"UNIT_R": "r7"})
        return mem.f32s(ab, 12 * count)
    if name == "PSVECKillElement":
        src, kill, dst = c.floats(x[:3]), c.floats(x[3:6]), c.floats([0.0] * 3)
        c.ptr("pSrc", src), c.ptr("pKill", kill), c.ptr("pDst", dst)
        c.run(MATH_UTIL, "f32 PSVECKillElement(")
        return mem.f32s(dst, 3) + [m.f("dot")[0]]
    if name == "MR::vecScaleAdd":
        a, b = c.floats(x[:3]), c.floats(x[3:6])
        c.ptr("pA1", a), c.ptr("pA2", b)
        m.alias["a3"] = "f1"
        m.bind_f("a3", x[6])
        c.run(MATH_UTIL, "void vecScaleAdd(")
        return mem.f32s(a, 3)
    if name == "MR::PSvecBlend":
        a, b, d = c.floats(x[:3]), c.floats(x[3:6]), c.floats([0.0] * 3)
        c.ptr("pA1", a), c.ptr("pA2", b), c.ptr("pA3", d)
        m.alias["a4"], m.alias["a5"] = "f1", "f2"
        m.bind_f("a4", x[6]), m.bind_f("a5", x[7])
        c.run(MATH_UTIL, "void PSvecBlend(")
        return mem.f32s(d, 3)
    if name in ("JMAVECScaleAdd", "JMAVECLerp"):
        a, b, d = c.floats(x[:3]), c.floats(x[3:6]), c.floats([0.0] * 3)
        if name == "JMAVECScaleAdd":
            c.ptr("vec1", a), c.ptr("vec2", b), c.ptr("dst", d)
            m.bind_f("scale", x[6])
        else:
            c.ptr("a", a), c.ptr("b", b), c.ptr("dst", d)
            m.bind_f("t", x[6])
        c.run(JMATH_CPP, "void %s(" % name)
        return mem.f32s(d, 3)
    if name == "J3DPSMtx33Copy":
        src, dst = c.floats(x[:9]), c.floats([0.0] * 9)
        c.ptr("source", src), c.ptr("destination", dst)
        c.run(J3D_SHAPE_MTX, "void J3DPSMtx33Copy(")
        return mem.f32s(dst, 9)
    if name == "J3DPSMtx33CopyFrom34":
        src, dst = c.floats(x[:12]), c.floats([0.0] * 9)
        c.ptr("src", src), c.ptr("dst", dst)
        c.run(J3D_SHAPE_MTX, "void J3DPSMtx33CopyFrom34(")
        return mem.f32s(dst, 9)
    if name == "J3DCalcZValue":
        mtx, v = c.floats(x[:12]), c.floats(x[12:15])
        c.ptr("m", mtx), c.ptr("v", v)
        m.bind_f("temp_f1", 1.0)
        c.run(J3D_DRAW_BUFFER, "inline f32 J3DCalcZValue(")
        return [m.f("out")[0]]
    if name.startswith("JMathInlineVEC::"):
        short = name.split("::")[1]
        a, b, d = c.floats(x[:3]), c.floats((x[3:6] + [0.0] * 3)[:3]), c.floats([0.0] * 3)
        if short == "PSVECDotProduct":
            c.ptr("pA", a), c.ptr("pB", b)
            c.run(JMATH_HPP, "inline f32 PSVECDotProduct(const register Vec* pA")
            return [m.f("product")[0]]
        if short in ("PSVECAdd", "PSVECSubtract", "PSVECMultiply"):
            c.ptr("vec1", a), c.ptr("vec2", b), c.ptr("dst", d)
            c.run(JMATH_HPP, "inline void %s(register const Vec* vec1" % short)
            return mem.f32s(d, 3)
        if short == "PSVECSquareMag":
            c.ptr("src", a)
            c.run(JMATH_HPP, "inline f32 PSVECSquareMag(register const Vec* src)")
            return [m.f("ret")[0]]
        if short == "PSVECNegate":
            c.ptr("src", a), c.ptr("dst", d)
            c.run(JMATH_HPP, "inline void PSVECNegate(register const Vec* src")
            mem.put_f32s(d + 8, [-mem.f32s(a + 8, 1)[0]])  # dst->z = -src->z;
            return mem.f32s(d, 3)
        if short == "PSVECSquareDistance":
            c.ptr("a", a), c.ptr("b", b)
            c.run(JMATH_HPP, "inline f32 PSVECSquareDistance(const register Vec* a")
            return [m.f("sqdist")[0]]
    if name == "JMath::gekko_ps_copy12":
        src, dst = c.floats(x[:12]), c.floats([0.0] * 12)
        c.ptr("pSrc", src), c.ptr("pDest", dst)
        c.run(JMATH_HPP, "inline void gekko_ps_copy12(")
        return mem.f32s(dst, 12)
    if name in ("JGeometry::negateInternal", "JGeometry::mulInternal"):
        a, b, d = c.floats(x[:3]), c.floats((x[3:6] + [0.0] * 3)[:3]), c.floats([0.0] * 3)
        if name == "JGeometry::negateInternal":
            c.ptr("rSrc", a), c.ptr("rDest", d)
            c.run(TVEC, "inline void negateInternal(register const f32* rSrc")
            mem.put_f32s(d + 8, [-mem.f32s(a + 8, 1)[0]])  # rDest[2] = -rSrc[2];
        else:
            c.ptr("vec1", a), c.ptr("vec2", b), c.ptr("dst", d)
            c.run(TVEC, "inline void mulInternal(register const f32* vec1")
            mem.put_f32s(d + 8, [f32(mem.f32s(a + 8, 1)[0] * mem.f32s(b + 8, 1)[0])])  # dst[2] = vec1[2] * vec2[2];
        return mem.f32s(d, 3)
    if name == "TVec3f::scaleAdd":
        a, b = c.floats(x[:3]), c.floats(x[3:6])
        c.ptr("dest", a), c.ptr("source", b)
        m.bind_f("scale", x[6])
        c.run(TVEC, "void scaleAdd(register f32 scale, const TVec3& src)")
        return mem.f32s(a, 3)
    if name == "TVec3f::dot":
        a, b = c.floats(x[:3]), c.floats(x[3:6])
        c.ptr("a", a), c.ptr("b", b)
        c.run(TVEC, "f32 dot(const TVec3& rOther) const NO_INLINE")
        return [m.f("_fp1")[0]]
    if name == "TVec3f::squared":
        a, b = c.floats(x[:3]), c.floats(x[3:6])
        c.ptr("a", a), c.ptr("b", b)
        c.run(TVEC, "f32 squared(const TVec3& rB) const {")
        return [m.f("sqdist")[0]]
    if name in ("JMAHermiteInterpolation", "J3DHermiteInterpolation_f32"):
        values = x[:7] if name == "JMAHermiteInterpolation" else x[:7]
        for i, value in enumerate(values):
            m.bind_f("p%d" % (i + 1), value)
        c.run(JMATH_HPP, "inline f32 JMAHermiteInterpolation(")
        return [m.f("ff25")[0]]
    if name == "J3DHermiteInterpolation_s16":
        keys = [int(v) for v in x[1:7]]
        data = c.s16s(keys)
        m.bind_f("value", x[0])
        for i in range(6):
            c.ptr("pp%d" % (i + 2), data + 2 * i)
        c.run(J3D_ANIMATION, "inline f32 J3DHermiteInterpolation(__REGISTER f32 pp1, __REGISTER s16 const* pp2")
        result = m.f("value")[0]
        # calcTransform: J3D_ROT_SHIFT((int)value, 0) stored into an s16.
        integer = int(math.trunc(result)) if math.isfinite(result) else 0
        integer = ((integer + 0x8000) & 0xFFFF) - 0x8000
        return [float(integer)]
    if name in ("__OSf32tos16", "__OSf32tou8", "OSf32tou16"):
        tmp = c.floats([0.0])
        if name == "OSf32tou16":
            m.bind_f("in", x[0]), c.ptr("ptr", tmp)
            c.run(FAST_CAST, "static inline u16 __OSf32tou16(")
            return [float(m.r("r"))]
        m.bind_f("inF", x[0]), c.ptr("tmpPtr", tmp)
        c.run(OS_H, "inline %s %s(" % ("s16" if name == "__OSf32tos16" else "u8", name))
        value = m.r("out")
        if name == "__OSf32tos16":
            value = value - 0x100000000 if value & 0x80000000 else value
        return [float(value)]
    raise KeyError(name)


def rand(rng, n, scale=10.0):
    return [f32(rng.uniform(-scale, scale)) for _ in range(n)]


def cases(rng):
    """Representative and edge inputs per function."""
    identity = [1, 0, 0, 5, 0, 1, 0, -3, 0, 0, 1, 2]
    singular = [1, 2, 3, 0, 2, 4, 6, 0, 1, 1, 1, 0]
    out = []
    for _ in range(40):
        out.append(("J3DPSCalcInverseTranspose", rand(rng, 12) + [7.0] * 9))
        out.append(("J3DScaleNrmMtx", rand(rng, 15)))
        out.append(("J3DScaleNrmMtx33", rand(rng, 12)))
        out.append(("J3DMtxProjConcat", rand(rng, 28)))
        out.append(("J3DMtxProjConcat_inplace", rand(rng, 28)))
        out.append(("J3DPSMtxArrayConcat", [3.0] + rand(rng, 12 + 36)))
        out.append(("J3DPSMtxArrayConcat_inplace", [3.0] + rand(rng, 12 + 36)))
        out.append(("PSVECKillElement", rand(rng, 6)))
        out.append(("MR::vecScaleAdd", rand(rng, 7)))
        out.append(("MR::PSvecBlend", rand(rng, 8, 2.0)))
        out.append(("JMAVECScaleAdd", rand(rng, 7)))
        out.append(("JMAVECLerp", rand(rng, 7, 2.0)))
        out.append(("J3DPSMtx33Copy", rand(rng, 9, 1e30)))
        out.append(("J3DPSMtx33CopyFrom34", rand(rng, 12, 1e30)))
        out.append(("J3DCalcZValue", rand(rng, 15)))
        for short in ("PSVECDotProduct", "PSVECAdd", "PSVECSubtract", "PSVECMultiply", "PSVECSquareMag", "PSVECNegate",
                      "PSVECSquareDistance"):
            out.append(("JMathInlineVEC::" + short, rand(rng, 6)))
        out.append(("JMath::gekko_ps_copy12", rand(rng, 12, 1e30)))
        out.append(("JGeometry::negateInternal", rand(rng, 3)))
        out.append(("JGeometry::mulInternal", rand(rng, 6)))
        out.append(("TVec3f::scaleAdd", rand(rng, 7)))
        out.append(("TVec3f::dot", rand(rng, 6)))
        out.append(("TVec3f::squared", rand(rng, 6)))
        t0 = rng.randint(0, 50)
        t1 = t0 + rng.randint(1, 60)
        frame = f32(rng.uniform(t0, t1 - 0.001))
        out.append(("JMAHermiteInterpolation", [frame, t0, *rand(rng, 2, 100), t1, *rand(rng, 2, 100)]))
        keys = [t0, rng.randint(-20000, 20000), rng.randint(-300, 300), t1, rng.randint(-20000, 20000), rng.randint(-300, 300)]
        out.append(("J3DHermiteInterpolation_s16", [frame] + [float(k) for k in keys]))
        out.append(("J3DHermiteInterpolation_f32", [frame, float(t0), *rand(rng, 2, 3), float(t1), *rand(rng, 2, 3)]))
    for _ in range(80):
        for suffix in ("", "_a", "_b"):
            out.append(("JGeometry::subInternal" + suffix, rand(rng, 6)))
            out.append(("SDK::PSQUATMultiply" + suffix, rand(rng, 8, 1.0)))
        for suffix in ("", "SR", "_inplace", "SR_inplace"):
            out.append(("SDK::PSMTXMultVec" + suffix, rand(rng, 15, 1.0)))
        out.append(("SDK::PSMTXQuat", rand(rng, 4, 1.0)))
        out.append(("SDK::PSQUATDotProduct", rand(rng, 8, 1.0)))
    # Edges: zeros and -0, identity, a singular matrix (no write), large values.
    out.append(("J3DPSCalcInverseTranspose", identity + [7.0] * 9))
    out.append(("J3DPSCalcInverseTranspose", singular + [7.0] * 9))
    out.append(("J3DPSCalcInverseTranspose", [0.0] * 12 + [7.0] * 9))
    out.append(("J3DScaleNrmMtx", [-0.0] * 12 + [1.0, -0.0, 0.0]))
    out.append(("PSVECKillElement", [1e20, -1e20, 3.0, 0.0, 0.0, 1.0]))
    out.append(("MR::PSvecBlend", [-62.6, -16.0, -26.9, 29.4, 2.1, 63.5, 0.9, 0.1]))
    out.append(("J3DPSMtxArrayConcat", [1.0] + identity + identity))
    for value in (0.0, -0.0, 0.5, -0.5, 0.999, -0.999, 1.0, -1.0, 127.9, 128.0, 255.4, 255.9, 256.0, 1000.0, -1000.0,
                  32767.0, 32767.9, 32768.0, -32768.0, -32768.9, -40000.0, 65535.0, 65535.9, 70000.0, 1e30, -1e30,
                  math.inf, -math.inf):
        for name in ("__OSf32tos16", "__OSf32tou8", "OSf32tou16"):
            out.append((name, [value]))
    return out


EXACT = ("Copy", "copy", "negate", "Negate")


def close(a, b, name):
    if math.isnan(a) and math.isnan(b):
        return True
    if any(key in name for key in EXACT) or name.startswith(("__OS", "OSf32")):
        return struct.pack("<f", a) == struct.pack("<f", b) or (a == b)
    if a == b:
        return True
    if math.isinf(a) or math.isinf(b):
        return False
    tolerance = 2e-5 if name == "J3DPSCalcInverseTranspose" else 1e-5 if "Hermite" in name else 2e-6
    if name == "J3DHermiteInterpolation_s16":
        return abs(a - b) <= 1.0  # the float result is truncated to an integer
    return abs(a - b) <= tolerance * max(1.0, abs(a), abs(b))


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    rng = random.Random(20260930)
    all_cases = cases(rng)
    stdin = "\n".join("%s %s" % (name, " ".join(float(v).hex() if math.isfinite(v) else ("inf" if v > 0 else "-inf") if math.isinf(v) else "nan" for v in x))
                      for name, x in all_cases) + "\n"
    result = subprocess.run([sys.argv[1]], input=stdin, capture_output=True, text=True, check=True)
    lines = result.stdout.splitlines()
    assert len(lines) == len(all_cases), "harness answered %d of %d cases" % (len(lines), len(all_cases))
    failures, checked = [], {}
    for (name, x), line in zip(all_cases, lines):
        if line.startswith("unknown"):
            failures.append("%s: not in the harness" % name)
            continue
        native = [float.fromhex(t) if t not in ("inf", "-inf", "nan") else float(t) for t in line.split()]
        expected = wii(name, x)
        checked[name] = checked.get(name, 0) + 1
        if len(native) != len(expected) or not all(close(e, n, name) for e, n in zip(expected, native)):
            failures.append("%s%s:\n    wii    %s\n    native %s" % (name, [round(v, 4) for v in x[:8]], expected, native))
    for name in sorted(checked):
        print("%-36s %d cases" % (name, checked[name]))
    if failures:
        print("\n%d mismatches:" % len(failures), file=sys.stderr)
        for failure in failures[:40]:
            print("  " + failure, file=sys.stderr)
        return 1
    print("all %d cases match the Wii asm" % len(all_cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())

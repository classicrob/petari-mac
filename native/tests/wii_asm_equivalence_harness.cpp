// Runs native branches of functions whose Wii versions are paired-single asm, for
// native/tests/wii_asm_equivalence_tests.py, which runs the Wii asm text in its
// paired-single interpreter (native/tests/paired_single.py) and compares.
//
// Protocol: one case per stdin line, "<function> <f32 inputs as %a hex floats...>";
// one stdout line per case with the outputs as %a hex floats.
#include "Game/Util/MathUtil.hpp"
#include "JSystem/J3DGraphAnimator/J3DAnimation.hpp"
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DTransform.hpp"
#include "JSystem/JGeometry/TVec.hpp"
#include "JSystem/JMath/JMath.hpp"
#include <revolution/os.h>
#include <revolution/os/OSFastCast.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

f32 PSVECKillElement(const Vec*, const Vec*, const Vec*);
void J3DPSMtx33Copy(Mtx3P, Mtx3P);

using Floats = std::vector< f32 >;

static void put(Floats& rOut, const f32* pValues, size_t count) {
    rOut.insert(rOut.end(), pValues, pValues + count);
}

static Vec vec(const f32* p) {
    Vec v = {p[0], p[1], p[2]};
    return v;
}

// Each case receives its inputs and appends its outputs. Layouts match the Python side.
static bool run(const std::string& rName, const Floats& in, Floats& out) {
    const f32* p = in.data();
    if (rName.find("JGeometry::subInternal") == 0) {
        f32 a[3], b[3], d[3];
        std::memcpy(a, p, sizeof(a));
        std::memcpy(b, p + 3, sizeof(b));
        f32* dst = rName == "JGeometry::subInternal_a" ? a : rName == "JGeometry::subInternal_b" ? b : d;
        JGeometry::subInternal(a, b, dst);
        put(out, dst, 3);
    } else if (rName.find("SDK::PSMTXMultVec") == 0) {
        Mtx matrix;
        std::memcpy(matrix, p, sizeof(matrix));
        Vec v = vec(p + 12), d;
        Vec* dst = rName.find("_inplace") != std::string::npos ? &v : &d;
        if (rName.find("SR") != std::string::npos) PSMTXMultVecSR(matrix, &v, dst);
        else PSMTXMultVec(matrix, &v, dst);
        put(out, &dst->x, 3);
    } else if (rName == "SDK::PSMTXQuat") {
        Quaternion q{p[0], p[1], p[2], p[3]};
        Mtx matrix;
        PSMTXQuat(matrix, &q);
        put(out, &matrix[0][0], 12);
    } else if (rName.find("SDK::PSQUATMultiply") == 0) {
        Quaternion a{p[0], p[1], p[2], p[3]}, b{p[4], p[5], p[6], p[7]}, d;
        Quaternion* dst = rName == "SDK::PSQUATMultiply_a" ? &a : rName == "SDK::PSQUATMultiply_b" ? &b : &d;
        PSQUATMultiply(&a, &b, dst);
        put(out, &dst->x, 4);
    } else if (rName == "SDK::PSQUATDotProduct") {
        Quaternion a{p[0], p[1], p[2], p[3]}, b{p[4], p[5], p[6], p[7]};
        out.push_back(PSQUATDotProduct(&a, &b));
    } else if (rName == "J3DPSCalcInverseTranspose") {
        Mtx src;
        Mtx33 dst;
        std::memcpy(src, p, sizeof(src));
        std::memcpy(dst, p + 12, sizeof(dst));  // sentinel: kept when the determinant is 0
        J3DPSCalcInverseTranspose(src, dst);
        put(out, &dst[0][0], 9);
    } else if (rName == "J3DScaleNrmMtx") {
        Mtx mtx;
        std::memcpy(mtx, p, sizeof(mtx));
        J3DScaleNrmMtx(mtx, vec(p + 12));
        put(out, &mtx[0][0], 12);
    } else if (rName == "J3DScaleNrmMtx33") {
        Mtx33 mtx;
        std::memcpy(mtx, p, sizeof(mtx));
        J3DScaleNrmMtx33(mtx, vec(p + 9));
        put(out, &mtx[0][0], 9);
    } else if (rName == "J3DMtxProjConcat" || rName == "J3DMtxProjConcat_inplace") {
        Mtx a;
        Mtx44 b;
        std::memcpy(a, p, sizeof(a));
        std::memcpy(b, p + 12, sizeof(b));
        Mtx dst;
        if (rName == "J3DMtxProjConcat") {
            J3DMtxProjConcat(a, b, dst);
        } else {
            J3DMtxProjConcat(a, b, a);
            std::memcpy(dst, a, sizeof(dst));
        }
        put(out, &dst[0][0], 12);
    } else if (rName == "J3DPSMtxArrayConcat" || rName == "J3DPSMtxArrayConcat_inplace") {
        const u32 count = static_cast< u32 >(p[0]);
        Mtx a;
        std::memcpy(a, p + 1, sizeof(a));
        std::vector< f32 > b(p + 13, p + 13 + 12 * count), ab(12 * count, 0.0f);
        Mtx* pB = reinterpret_cast< Mtx* >(b.data());
        Mtx* pAB = rName == "J3DPSMtxArrayConcat" ? reinterpret_cast< Mtx* >(ab.data()) : pB;
        J3DPSMtxArrayConcat(a, *pB, *pAB, count);
        put(out, &(*pAB)[0][0], 12 * count);
    } else if (rName == "PSVECKillElement") {
        Vec src = vec(p), kill = vec(p + 3), dst;
        const f32 dot = PSVECKillElement(&src, &kill, &dst);
        put(out, &dst.x, 3);
        out.push_back(dot);
    } else if (rName == "MR::vecScaleAdd") {
        TVec3f a(p[0], p[1], p[2]), b(p[3], p[4], p[5]);
        MR::vecScaleAdd(&a, &b, p[6]);
        put(out, &a.x, 3);
    } else if (rName == "MR::PSvecBlend") {
        TVec3f a(p[0], p[1], p[2]), b(p[3], p[4], p[5]), d;
        MR::PSvecBlend(&a, &b, &d, p[6], p[7]);
        put(out, &d.x, 3);
    } else if (rName == "JMAVECScaleAdd") {
        Vec a = vec(p), b = vec(p + 3), d;
        JMAVECScaleAdd(&a, &b, &d, p[6]);
        put(out, &d.x, 3);
    } else if (rName == "JMAVECLerp") {
        Vec a = vec(p), b = vec(p + 3), d;
        JMAVECLerp(&a, &b, &d, p[6]);
        put(out, &d.x, 3);
    } else if (rName == "J3DPSMtx33Copy") {
        Mtx33 src, dst;
        std::memcpy(src, p, sizeof(src));
        J3DPSMtx33Copy(src, dst);
        put(out, &dst[0][0], 9);
    } else if (rName == "J3DPSMtx33CopyFrom34") {
        Mtx src;
        Mtx33 dst;
        std::memcpy(src, p, sizeof(src));
        J3DPSMtx33CopyFrom34(src, dst);
        put(out, &dst[0][0], 9);
    } else if (rName == "J3DCalcZValue") {
        Mtx m;
        std::memcpy(m, p, sizeof(m));
        out.push_back(J3DCalcZValue(m, vec(p + 12)));
    } else if (rName == "JMathInlineVEC::PSVECDotProduct") {
        Vec a = vec(p), b = vec(p + 3);
        out.push_back(JMathInlineVEC::PSVECDotProduct(&a, &b));
    } else if (rName == "JMathInlineVEC::PSVECAdd" || rName == "JMathInlineVEC::PSVECSubtract" || rName == "JMathInlineVEC::PSVECMultiply") {
        Vec a = vec(p), b = vec(p + 3), d;
        if (rName == "JMathInlineVEC::PSVECAdd") {
            JMathInlineVEC::PSVECAdd(&a, &b, &d);
        } else if (rName == "JMathInlineVEC::PSVECSubtract") {
            JMathInlineVEC::PSVECSubtract(&a, &b, &d);
        } else {
            JMathInlineVEC::PSVECMultiply(&a, &b, &d);
        }
        put(out, &d.x, 3);
    } else if (rName == "JMathInlineVEC::PSVECSquareMag") {
        Vec a = vec(p);
        out.push_back(JMathInlineVEC::PSVECSquareMag(&a));
    } else if (rName == "JMathInlineVEC::PSVECNegate") {
        Vec a = vec(p), d;
        JMathInlineVEC::PSVECNegate(&a, &d);
        put(out, &d.x, 3);
    } else if (rName == "JMathInlineVEC::PSVECSquareDistance") {
        Vec a = vec(p), b = vec(p + 3);
        out.push_back(JMathInlineVEC::PSVECSquareDistance(&a, &b));
    } else if (rName == "JMath::gekko_ps_copy12") {
        f32 dst[12];
        JMath::gekko_ps_copy12(dst, p);
        put(out, dst, 12);
    } else if (rName == "JGeometry::negateInternal" || rName == "JGeometry::mulInternal") {
        f32 d[3];
        if (rName == "JGeometry::negateInternal") {
            JGeometry::negateInternal(p, d);
        } else {
            JGeometry::mulInternal(p, p + 3, d);
        }
        put(out, d, 3);
    } else if (rName == "TVec3f::scaleAdd") {
        TVec3f a(p[0], p[1], p[2]), b(p[3], p[4], p[5]);
        a.scaleAdd(p[6], b);
        put(out, &a.x, 3);
    } else if (rName == "TVec3f::dot") {
        TVec3f a(p[0], p[1], p[2]), b(p[3], p[4], p[5]);
        out.push_back(a.dot(b));
    } else if (rName == "TVec3f::squared") {
        TVec3f a(p[0], p[1], p[2]), b(p[3], p[4], p[5]);
        out.push_back(a.squared(b));
    } else if (rName == "JMAHermiteInterpolation") {
        out.push_back(JMAHermiteInterpolation(p[0], p[1], p[2], p[3], p[4], p[5], p[6]));
    } else if (rName == "J3DHermiteInterpolation_s16" || rName == "J3DHermiteInterpolation_f32") {
        // One joint whose X rotation (s16 keys) or X scale (f32 keys) has two keys of
        // three values each (time, value, tangent); the frame lies between them, so
        // J3DGetKeyFrameInterpolation calls the Hermite once with the keys in order.
        static J3DAnmTransformKeyTable table[3];
        std::memset(table, 0, sizeof(table));
        s16 rot[6];
        f32 scale[6];
        for (int i = 0; i < 6; i++) {
            rot[i] = static_cast< s16 >(p[1 + i]);
            scale[i] = p[1 + i];
        }
        J3DAnmTransformKey anm;
        anm.mAnmTable = table;
        anm.mRotData = rot;
        anm.mScaleData = scale;
        anm.mDecShift = 0;
        if (rName == "J3DHermiteInterpolation_s16") {
            table[0].mRotationInfo.mMaxFrame = 2;
        } else {
            table[0].mScaleInfo.mMaxFrame = 2;
        }
        J3DTransformInfo info;
        anm.calcTransform(p[0], 0, &info);
        out.push_back(rName == "J3DHermiteInterpolation_s16" ? static_cast< f32 >(info.mRotation.x) : info.mScale.x);
    } else if (rName == "__OSf32tos16") {
        out.push_back(static_cast< f32 >(__OSf32tos16(p[0])));
    } else if (rName == "__OSf32tou8") {
        out.push_back(static_cast< f32 >(__OSf32tou8(p[0])));
    } else if (rName == "OSf32tou16") {
        volatile u16 value = 0;
        OSf32tou16(&p[0], &value);
        out.push_back(static_cast< f32 >(value));
    } else {
        return false;
    }
    return true;
}

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream stream(line);
        std::string name, token;
        stream >> name;
        Floats in, out;
        while (stream >> token) {
            in.push_back(std::strtof(token.c_str(), nullptr));
        }
        if (!run(name, in, out)) {
            std::printf("unknown %s\n", name.c_str());
            std::fflush(stdout);
            continue;
        }
        for (size_t i = 0; i < out.size(); i++) {
            std::printf(i ? " %a" : "%a", static_cast< double >(out[i]));
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    return 0;
}

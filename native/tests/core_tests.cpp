#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <type_traits>
#include "Game/LiveActor/Nerve.hpp"
#include "Game/System/NerveExecutor.hpp"
#include "JSystem/JMath/JMath.hpp"
#include "JSystem/JMath/JMATrigonometric.hpp"
#include "JSystem/JMath/random.hpp"
#include "JSystem/JGeometry/TMatrix.hpp"
#include <revolution/os.h>
#include "JSystem/JAudio2/JASSeqReader.hpp"
#include <petari/locale.hpp>

static_assert(sizeof(u32) == 4 && sizeof(s32) == 4);
static_assert(sizeof(u64) == 8 && sizeof(s64) == 8);
static_assert(sizeof(Vec) == 12 && sizeof(Mtx) == 48 && sizeof(Quaternion) == 16);
static_assert(std::is_same_v<decltype(nullptr), std::nullptr_t>);

namespace {
int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
bool near(float a, float b, float tolerance = 0.0001f) { return std::fabs(a - b) <= tolerance; }
void checkVec(const Vec& v, float x, float y, float z, const char* label) {
    check(near(v.x, x) && near(v.y, y) && near(v.z, z), label);
}
void checkIdentity(const Mtx m) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            check(near(m[i][j], i == j ? 1.0f : 0.0f), "inverse composition");
}

struct Counter { int executions = 0; int endings = 0; };
struct CountingNerve : Nerve {
    const Nerve* next = nullptr;
    void execute(Spine* spine) const override {
        ++static_cast<Counter*>(spine->mExecutor)->executions;
        if (next) spine->setNerve(next);
    }
    void executeOnEnd(Spine* spine) const override {
        ++static_cast<Counter*>(spine->mExecutor)->endings;
    }
};
}

int main() {
    for (int requested = -1; requested <= 255; ++requested) {
        const auto usa = PetariNative::selectDiscLanguage('E', requested);
        check(usa && usa->table == 1 && (usa->language == 1 || usa->language == 3 || usa->language == 4), "USA language bounded to available locales");
        const auto korea = PetariNative::selectDiscLanguage('K', requested);
        check(korea && korea->table == 4 && korea->language == 9, "Korean assets select Korean language");
    }
    check(PetariNative::selectDiscLanguage('E', 3)->language == 3, "USA French selection");
    check(PetariNative::selectDiscLanguage('E', 4)->language == 4, "USA Spanish selection");
    check(!PetariNative::selectDiscLanguage('\0', 1), "Unrecognized disc region is rejected");
    u8 sequenceBytes[] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    JASSeqReader sequence;
    sequence.init(sequenceBytes);
    check(sequence.get16(1) == 0x3456, "unaligned big-endian 16-bit read");
    check(sequence.get24(0) == 0x123456, "24-bit read at buffer start");
    check(sequence.get32(1) == 0x3456789a, "unaligned big-endian 32-bit read");
    check(sequence.read24() == 0x123456 && sequence.getOffset() == 3, "sequence read24 advances cursor");
    check(sequence.read16() == 0x789a && sequence.getOffset() == 5, "sequence read16 advances cursor");
    check(sequence.call(0) && sequence.readByte() == 0x12 && sequence.ret(), "sequence call and return");
    check(sequence.getOffset() == 5, "sequence returns to saved 64-bit pointer");
    check(sequence.loopStart(2), "sequence loop start");
    sequence.readByte(); sequence.loopEnd();
    check(sequence.getOffset() == 5 && sequence.getLoopCount() == 1, "sequence loop repeat");
    sequence.readByte(); sequence.loopEnd();
    check(sequence.getOffset() == 6 && sequence.getLoopCount() == 0, "sequence loop exit");
    alignas(32) unsigned char storage[64];
    check(ROUND_UP_PTR(storage + 1, 32) == storage + 32, "64-bit pointer alignment");
    check(__cntlzw(0) == 32 && __cntlzw(1) == 31 && __cntlzw(0x80000000) == 0, "32-bit count leading zeros");
    float castInput = -10; u16 castOutput;
    OSf32tou16(&castInput, &castOutput); check(castOutput == 0, "fast cast negative saturation");
    castInput = 70000;
    OSf32tou16(&castInput, &castOutput); check(castOutput == 65535, "fast cast positive saturation");
    castInput = 10.9f;
    OSf32tou16(&castInput, &castOutput); check(castOutput == 10, "fast cast truncation");
    OSu16tof32(&castOutput, &castInput); check(castInput == 10, "fast cast to float");
    const float colorInputs[] = {-40000.0f, -32768.0f, -12.9f, 0.0f, 0.9f, 127.9f, 255.0f, 256.0f, 40000.0f};
    const s16 signedExpected[] = {-32768, -32768, -12, 0, 0, 127, 255, 256, 32767};
    const u8 byteExpected[] = {0, 0, 0, 0, 0, 127, 255, 255, 255};
    for (unsigned i = 0; i < std::size(colorInputs); ++i) {
        float value = colorInputs[i];
        s16 signedColor = 1234;
        u8 alpha = 42;
        OSf32tos16(&value, &signedColor);
        OSf32tou8(&value, &alpha);
        check(signedColor == signedExpected[i], "signed animation color saturates and truncates");
        check(alpha == byteExpected[i], "animation opacity saturates and truncates");
    }
    JMath::TRandom_fast_ random(0);
    check(random.rand() == 1013904223u, "RNG first value");
    check(random.rand() == 1196435762u, "RNG wraps at 32 bits");
    for (int i = 0; i < 10000; ++i) {
        const float value = random.getRandF();
        check(value >= 0 && value < 1, "RNG float range");
    }

    Vec a{1, 2, 3}, b{4, -5, 6}, result;
    check(near(PSVECDotProduct(&a, &b), 12), "dot product");
    PSVECCrossProduct(&a, &b, &a);
    checkVec(a, 27, 6, -13, "cross product with aliased output");
    a = {3, 0, 4};
    PSVECNormalize(&a, &a);
    checkVec(a, 0.6f, 0, 0.8f, "in-place normalize");
    TVec3f gameVector(1.f, 2.f, 3.f);
    TVec3f copiedVector(gameVector);
    copiedVector = TVec3f(4.f, 5.f, 6.f);
    checkVec(copiedVector, 4, 5, 6, "game vector assignment");
    check(near(copiedVector.dot(gameVector), 32), "game vector dot product");
    check(near(copiedVector.squared(gameVector), 27), "game vector distance squared");
    auto difference = copiedVector - gameVector;
    checkVec(difference, 3, 3, 3, "game vector subtraction");
    difference.negate(); checkVec(difference, -3, -3, -3, "game vector negation");
    copiedVector.setPSZeroVec(); checkVec(copiedVector, 0, 0, 0, "game vector zero");
    Vec zero{0, 0, 0};
    check(PSVECMag(&zero) == 0, "zero magnitude");
    check(near(JMAFastSqrt(9), 3), "native fast square root");
    check(near(JMAATan2(1, 1), 0.78539816f), "atan table endpoint");
    check(near(JMAATan2(1, -1), 2.3561945f), "atan second quadrant");
    check(near(JMAATan2(-1, -1), -2.3561945f), "atan third quadrant");
    check(near(JMAATan2(-1, 1), -0.78539816f), "atan fourth quadrant");
    check(near(JMAHermiteInterpolation(2, 2, 10, 0, 6, 20, 0), 10), "Hermite start");
    check(near(JMAHermiteInterpolation(6, 2, 10, 0, 6, 20, 0), 20), "Hermite end");
    check(near(JMAHermiteInterpolation(4, 2, 10, 0, 6, 20, 0), 15), "Hermite midpoint");
    check(near(JMAHermiteInterpolation(4, 2, 10, 2, 6, 20, -1), 16.5f), "Hermite tangents");
    a = {1, 2, 3}; b = {5, 6, 7};
    JMAVECLerp(&a, &b, &a, 0.25f);
    checkVec(a, 2, 3, 4, "in-place vector interpolation");
    JMAVECScaleAdd(&a, &b, &b, 2);
    checkVec(b, 9, 12, 15, "in-place scale add");

    std::array<float, 16> original{}, copy{};
    for (int i = 0; i < 16; ++i) original[i] = float(i + 1);
    JMath::gekko_ps_copy16(copy.data(), original.data());
    check(copy == original, "paired-single matrix copy replacement");
    copy.fill(0); JMath::gekko_ps_copy12(copy.data(), original.data());
    check(copy[11] == 12 && copy[12] == 0, "copy12 boundary");
    copy.fill(0); JMath::gekko_ps_copy6(copy.data(), original.data());
    check(copy[5] == 6 && copy[6] == 0, "copy6 boundary");
    copy.fill(0); JMath::gekko_ps_copy3(copy.data(), original.data());
    check(copy[2] == 3 && copy[3] == 0, "copy3 boundary");

    Mtx transform, scale, inverse, identity;
    PSMTXTrans(transform, 10, 20, 30);
    PSMTXScale(scale, 2, 3, 4);
    PSMTXConcat(transform, scale, transform);
    a = {1, 2, 3};
    PSMTXMultVec(transform, &a, &result);
    checkVec(result, 12, 26, 42, "affine transform");
    PSMTXMultVecSR(transform, &a, &result);
    checkVec(result, 2, 6, 12, "direction ignores translation");
    check(PSMTXInverse(transform, inverse) == 1, "invertible matrix");
    PSMTXConcat(transform, inverse, identity); checkIdentity(identity);
    PSMTXInverse(transform, transform);
    a = {12, 26, 42}; PSMTXMultVec(transform, &a, &result);
    checkVec(result, 1, 2, 3, "in-place inverse");
    PSMTXScale(scale, 0, 1, 1);
    PSMTXIdentity(inverse);
    check(PSMTXInverse(scale, inverse) == 0, "singular inverse status");
    checkIdentity(inverse);
    for (char axis : {'x', 'y', 'z'}) {
        Vec axisVector{axis == 'x' ? 1.f : 0.f, axis == 'y' ? 1.f : 0.f, axis == 'z' ? 1.f : 0.f};
        PSMTXRotRad(transform, axis, 0.7f);
        PSMTXRotAxisRad(scale, &axisVector, 0.7f);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) check(near(transform[i][j], scale[i][j]), "axis rotation agreement");
        PSMTXInverse(transform, inverse);
        PSMTXConcat(transform, inverse, identity); checkIdentity(identity);
    }
    for (int n = 0; n < 100; ++n) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 4; ++j) transform[i][j] = random.getRandF() - 0.5f;
            transform[i][i] += 2.0f;
        }
        check(PSMTXInverse(transform, inverse) == 1, "general affine inverse");
        PSMTXConcat(transform, inverse, identity); checkIdentity(identity);
        PSMTXCopy(inverse, scale);
        PSMTXConcat(transform, scale, scale); checkIdentity(scale);
    }

    Quaternion q{0, 0, 0, 1}, opposite{0, 0, 0, -1};
    JMAQuatLerp(&q, &opposite, 0.5f, &q);
    check(near(q.w, 1), "quaternion shortest-path interpolation");
    JMAEulerToQuat(0, 0, 0x4000, &q);
    PSMTXQuat(transform, &q);
    a = {1, 0, 0};
    PSMTXMultVec(transform, &a, &a);
    checkVec(a, 0, 1, 0, "game trig table to quaternion to matrix");
    C_QUATMtx(&opposite, transform);
    check(near(std::fabs(PSQUATDotProduct(&q, &opposite)), 1), "matrix quaternion round trip");

    Mtx44 projection;
    C_MTXPerspective(projection, 90, 1, 1, 100);
    check(near(projection[0][0], 1), "perspective field of view");
    auto depth = [&](float z) { return (projection[2][2] * z + projection[2][3]) / -z; };
    check(near(depth(-1), -1) && near(depth(-100), 0), "Wii projection depth convention");

    Counter counter;
    CountingNerve first, second;
    first.next = &second;
    Spine spine(&counter, &first);
    spine.update();
    check(counter.executions == 1 && counter.endings == 1, "original Nerve transition callbacks");
    check(spine.getCurrentNerve() == &second && spine.mStep == 0, "original Spine resets step");
    spine.update();
    check(counter.executions == 2 && spine.mStep == 1, "original Spine executes next state");

    std::printf("PASS: %d native core checks; pointers=%zu bits, u32=%zu bits\n", checks, sizeof(void*) * 8, sizeof(u32) * 8);
}

// MR vector helpers whose Wii versions are paired-single assembly (src/Game/Util/MathUtil.cpp).
//
// PSvecBlend and vecScaleAdd had only an `#ifdef __MWERKS__` asm body, so on the native
// build they did nothing. MR::vecBlend went through PSvecBlend: every in-place blend kept its
// old value. Mario's binder offset (MarioActor::_2C4) then stayed at the up vector of the
// planet he launched from, and after the Peanut launch star in Good Egg he hovered ~100
// units above Bean B until a blown landing snapped him to a stale ground point.
//
// Usage: petari_math_util_tests
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MtxUtil.hpp"
#include <cmath>
#include <cstdio>

static int sFailures = 0;

static void checkVec(const TVec3f& rActual, f32 x, f32 y, f32 z, const char* pText) {
    if (std::fabs(rActual.x - x) > 1e-4f || std::fabs(rActual.y - y) > 1e-4f || std::fabs(rActual.z - z) > 1e-4f) {
        std::fprintf(stderr, "FAIL: %s: got (%g, %g, %g), expected (%g, %g, %g)\n", pText, rActual.x, rActual.y, rActual.z, x, y, z);
        ++sFailures;
    }
}

static void checkRotationOrders() {
    // Independent scalar matrices, in left-to-right product order.
    const int orders[6][3] = {{2, 1, 0}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {1, 0, 2}, {0, 1, 2}};
    for (int sample = 0; sample < 20; ++sample) {
        const TVec3f angles(0.17f * sample, -0.11f * sample, 0.07f * sample);
        const double x = angles.x, y = angles.y, z = angles.z;
        const double rotation[3][3][3] = {
            {{1, 0, 0}, {0, std::cos(x), -std::sin(x)}, {0, std::sin(x), std::cos(x)}},
            {{std::cos(y), 0, std::sin(y)}, {0, 1, 0}, {-std::sin(y), 0, std::cos(y)}},
            {{std::cos(z), -std::sin(z), 0}, {std::sin(z), std::cos(z), 0}, {0, 0, 1}}
        };
        for (int order = -1; order <= 6; ++order) {
            const int* axes = orders[order < 0 || order > 5 ? 0 : order];
            double expected[3][3] = {};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    for (int j = 0; j < 3; ++j)
                        for (int k = 0; k < 3; ++k)
                            expected[row][col] += rotation[axes[0]][row][j] * rotation[axes[1]][j][k] * rotation[axes[2]][k][col];
            Mtx actual;
            MR::orderRotateMtx(order, angles, actual);
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 4; ++col) {
                    const double reference = col == 3 ? 0 : expected[row][col];
                    // The game's sine/cosine come from JMath's lookup table, which differs from the scalar
                    // reference by up to about 7e-4; an axis-order mistake is off by far more than that.
                    if (!std::isfinite(actual[row][col]) || std::fabs(actual[row][col] - reference) > 2e-3) {
                        std::fprintf(stderr, "FAIL rotation order %d sample %d [%d,%d]: %g vs %g\n", order, sample, row, col, actual[row][col], reference);
                        ++sFailures;
                    }
                }
            }
        }
    }
    std::puts("Checked 160 rotation matrices against scalar reference (1920 elements)");
}

int main() {
    checkRotationOrders();
    // PSvecBlend: dst = from * invRate + to * rate.
    TVec3f from(1.0f, 2.0f, 3.0f), to(10.0f, 20.0f, 30.0f), out(0.0f, 0.0f, 0.0f);
    MR::PSvecBlend(&from, &to, &out, 0.25f, 0.5f);
    checkVec(out, 5.25f, 10.5f, 15.75f, "PSvecBlend into a separate destination");

    // vecBlend in place, as MarioActor::updateGravityVec blends its binder offset each frame.
    TVec3f offset(-62.6f, -16.0f, -26.9f);
    const TVec3f target(29.4f, 2.1f, 63.5f);
    MR::vecBlend(offset, target, &offset, 0.1f);
    checkVec(offset, -62.6f * 0.9f + 29.4f * 0.1f, -16.0f * 0.9f + 2.1f * 0.1f, -26.9f * 0.9f + 63.5f * 0.1f, "vecBlend in place moves toward the target");
    for (int i = 0; i < 200; i++) {
        MR::vecBlend(offset, target, &offset, 0.1f);
    }
    checkVec(offset, target.x, target.y, target.z, "repeated in-place vecBlend converges on the target");

    // The destination may also alias the target.
    TVec3f a(0.0f, 0.0f, 0.0f), b(4.0f, 8.0f, 12.0f);
    MR::vecBlend(a, b, &b, 0.25f);
    checkVec(b, 1.0f, 2.0f, 3.0f, "vecBlend with the destination aliasing the target");

    // vecScaleAdd: first += second * scale.
    TVec3f accum(1.0f, 1.0f, 1.0f);
    const TVec3f add(2.0f, -4.0f, 0.5f);
    MR::vecScaleAdd(&accum, &add, 3.0f);
    checkVec(accum, 7.0f, -11.0f, 2.5f, "vecScaleAdd accumulates into its first argument");

    if (sFailures != 0) {
        std::fprintf(stderr, "%d math util check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("MR vector helper tests passed");
    return 0;
}

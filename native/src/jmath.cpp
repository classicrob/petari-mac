#include "JSystem/JMath/JMath.hpp"

namespace JMathInlineVEC {
void PSVECCopy(const Vec* src, Vec* dst) { *dst = *src; }
void PSVECAdd(const Vec* a, const Vec* b, Vec* dst) { ::PSVECAdd(a, b, dst); }
void PSVECSubtract(const Vec* a, const Vec* b, Vec* dst) { ::PSVECSubtract(a, b, dst); }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return ::PSVECDotProduct(a, b); }
f32 PSVECSquareMag(const Vec* v) { return ::PSVECDotProduct(v, v); }
void PSVECNegate(const Vec* src, Vec* dst) { *dst = {-src->x, -src->y, -src->z}; }
f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
    Vec delta;
    ::PSVECSubtract(a, b, &delta);
    return ::PSVECDotProduct(&delta, &delta);
}
void PSVECMultiply(const Vec* a, const Vec* b, Vec* dst) {
    *dst = {a->x * b->x, a->y * b->y, a->z * b->z};
}
}

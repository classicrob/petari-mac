#include <revolution/mtx.h>
#include <cmath>
#include <cstring>

void PSVECAdd(const Vec* a, const Vec* b, Vec* out) {
    *out = {a->x + b->x, a->y + b->y, a->z + b->z};
}

void PSVECSubtract(const Vec* a, const Vec* b, Vec* out) {
    *out = {a->x - b->x, a->y - b->y, a->z - b->z};
}

void PSVECScale(const Vec* v, Vec* out, f32 scale) {
    *out = {v->x * scale, v->y * scale, v->z * scale};
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

f32 PSVECMag(const Vec* v) {
    return std::sqrt(PSVECDotProduct(v, v));
}

void PSVECNormalize(const Vec* v, Vec* out) {
    PSVECScale(v, out, 1.0f / PSVECMag(v));
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* out) {
    *out = {a->y * b->z - a->z * b->y,
            a->z * b->x - a->x * b->z,
            a->x * b->y - a->y * b->x};
}

f32 PSVECDistance(const Vec* a, const Vec* b) {
    Vec difference;
    PSVECSubtract(a, b, &difference);
    return PSVECMag(&difference);
}

void PSMTXIdentity(Mtx out) {
    std::memset(out, 0, sizeof(Mtx));
    for (int i = 0; i < 3; ++i) out[i][i] = 1.0f;
}

void C_MTXIdentity(Mtx out) { PSMTXIdentity(out); }
void PSMTXCopy(const Mtx src, Mtx out) { std::memmove(out, src, sizeof(Mtx)); }
void C_MTXCopy(const Mtx src, Mtx out) { PSMTXCopy(src, out); }

void PSMTX44Identity(Mtx44 out) {
    std::memset(out, 0, sizeof(Mtx44));
    for (int i = 0; i < 4; ++i) out[i][i] = 1.0f;
}

void PSMTX44Copy(const Mtx44 src, Mtx44 out) { std::memmove(out, src, sizeof(Mtx44)); }

void PSMTXConcat(const Mtx a, const Mtx b, Mtx out) {
    Mtx result;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            result[row][col] = a[row][0] * b[0][col] + a[row][1] * b[1][col] + a[row][2] * b[2][col];
            if (col == 3) result[row][col] += a[row][3];
        }
    }
    PSMTXCopy(result, out);
}

void PSMTXMultVecSR(const Mtx m, const Vec* v, Vec* out) {
    *out = {m[0][0] * v->x + m[0][1] * v->y + m[0][2] * v->z,
            m[1][0] * v->x + m[1][1] * v->y + m[1][2] * v->z,
            m[2][0] * v->x + m[2][1] * v->y + m[2][2] * v->z};
}

void PSMTXMultVec(const Mtx m, const Vec* v, Vec* out) {
    Vec result;
    PSMTXMultVecSR(m, v, &result);
    *out = {result.x + m[0][3], result.y + m[1][3], result.z + m[2][3]};
}

void PSMTXMultVecArraySR(const Mtx m, const Vec* src, Vec* out, u32 count) {
    for (u32 i = 0; i < count; ++i) PSMTXMultVecSR(m, src + i, out + i);
}

u32 PSMTXInverse(const Mtx m, Mtx out) {
    const f32 a = m[0][0], b = m[0][1], c = m[0][2];
    const f32 d = m[1][0], e = m[1][1], f = m[1][2];
    const f32 g = m[2][0], h = m[2][1], i = m[2][2];
    const f32 determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (determinant == 0.0f) return 0;
    const f32 reciprocal = 1.0f / determinant;
    Mtx result = {
        {(e * i - f * h) * reciprocal, (c * h - b * i) * reciprocal, (b * f - c * e) * reciprocal, 0},
        {(f * g - d * i) * reciprocal, (a * i - c * g) * reciprocal, (c * d - a * f) * reciprocal, 0},
        {(d * h - e * g) * reciprocal, (b * g - a * h) * reciprocal, (a * e - b * d) * reciprocal, 0}};
    for (int row = 0; row < 3; ++row)
        result[row][3] = -(result[row][0] * m[0][3] + result[row][1] * m[1][3] + result[row][2] * m[2][3]);
    PSMTXCopy(result, out);
    return 1;
}

extern "C" u32 PSMTXInvXpose(const Mtx m, Mtx out) {
    Mtx inverse;
    if (!PSMTXInverse(m, inverse)) return 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) out[row][col] = inverse[col][row];
        out[row][3] = 0;
    }
    return 1;
}

void PSMTXTrans(Mtx out, f32 x, f32 y, f32 z) {
    PSMTXIdentity(out);
    out[0][3] = x; out[1][3] = y; out[2][3] = z;
}

void PSMTXTransApply(const Mtx src, Mtx out, f32 x, f32 y, f32 z) {
    PSMTXCopy(src, out);
    out[0][3] += x; out[1][3] += y; out[2][3] += z;
}

void PSMTXScale(Mtx out, f32 x, f32 y, f32 z) {
    std::memset(out, 0, sizeof(Mtx));
    out[0][0] = x; out[1][1] = y; out[2][2] = z;
}

void PSMTXScaleApply(const Mtx src, Mtx out, f32 x, f32 y, f32 z) {
    const f32 scale[3] = {x, y, z};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) out[row][col] = src[row][col] * scale[row];
}

void PSMTXRotTrig(Mtx out, char axis, f32 sine, f32 cosine) {
    axis |= 0x20;
    if (axis != 'x' && axis != 'y' && axis != 'z') return;
    PSMTXIdentity(out);
    const int first = axis == 'x' ? 1 : axis == 'y' ? 2 : 0;
    const int second = (first + 1) % 3;
    out[first][first] = out[second][second] = cosine;
    out[first][second] = -sine;
    out[second][first] = sine;
}

void PSMTXRotRad(Mtx out, char axis, f32 radians) {
    PSMTXRotTrig(out, axis, std::sin(radians), std::cos(radians));
}

void PSMTXRotAxisRad(Mtx out, const Vec* axis, f32 radians) {
    Vec v;
    PSVECNormalize(axis, &v);
    const f32 s = std::sin(radians), c = std::cos(radians), t = 1.0f - c;
    const Mtx result = {
        {t*v.x*v.x+c, t*v.x*v.y-s*v.z, t*v.x*v.z+s*v.y, 0},
        {t*v.x*v.y+s*v.z, t*v.y*v.y+c, t*v.y*v.z-s*v.x, 0},
        {t*v.x*v.z-s*v.y, t*v.y*v.z+s*v.x, t*v.z*v.z+c, 0}};
    PSMTXCopy(result, out);
}

void PSQUATMultiply(const Quaternion* a, const Quaternion* b, Quaternion* out) {
    *out = {a->w*b->x + a->x*b->w + a->y*b->z - a->z*b->y,
            a->w*b->y - a->x*b->z + a->y*b->w + a->z*b->x,
            a->w*b->z + a->x*b->y - a->y*b->x + a->z*b->w,
            a->w*b->w - a->x*b->x - a->y*b->y - a->z*b->z};
}

f32 PSQUATDotProduct(const Quaternion* a, const Quaternion* b) {
    return a->x*b->x + a->y*b->y + a->z*b->z + a->w*b->w;
}

void PSMTXQuat(Mtx out, const Quaternion* q) {
    const f32 s = 2.0f / PSQUATDotProduct(q, q);
    const f32 xx=q->x*q->x*s, yy=q->y*q->y*s, zz=q->z*q->z*s;
    const f32 xy=q->x*q->y*s, xz=q->x*q->z*s, yz=q->y*q->z*s;
    const f32 wx=q->w*q->x*s, wy=q->w*q->y*s, wz=q->w*q->z*s;
    const Mtx result = {{1-yy-zz, xy-wz, xz+wy, 0},
                        {xy+wz, 1-xx-zz, yz-wx, 0},
                        {xz-wy, yz+wx, 1-xx-yy, 0}};
    PSMTXCopy(result, out);
}

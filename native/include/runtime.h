#pragma once
// Native replacement for the Metrowerks PowerPC runtime helpers that game code
// calls directly. Only helpers with a known portable meaning are declared.
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Port of runtime.c __cvt_dbl_usll: truncates toward zero, returns 0 for
// magnitudes below 1, and saturates to 0x7FFFFFFFFFFFFFFF or
// 0x8000000000000000 by sign when the value exceeds 2^63. NaN and infinity
// saturate by sign in the same way.
static inline uint64_t __cvt_dbl_usll(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));

    int exponent = (int)((bits >> 52) & 0x7FF);
    if (exponent < 1023) {
        return 0;
    }

    int negative = (int)(bits >> 63);
    int shift = exponent - 1075;
    if (shift > 10) {
        return negative ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    }

    uint64_t mantissa = (bits & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    mantissa = shift < 0 ? mantissa >> -shift : mantissa << shift;
    return negative ? (uint64_t)0 - mantissa : mantissa;
}

#ifdef __cplusplus
}
#endif

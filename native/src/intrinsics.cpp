#include <revolution/types.h>
#include <cmath>
#include <cstring>

f32 __frsqrte(f32 value) { return 1.0f / std::sqrt(value); }
u32 __cntlzw(u32 value) { return value == 0 ? 32 : __builtin_clz(value); }
s32 __abs(s32 value) { return value < 0 ? static_cast<s32>(0u - static_cast<u32>(value)) : value; }
f32 __fabsf(f32 value) { return std::fabs(value); }
f64 __fabs(f64 value) { return std::fabs(value); }
void* __memcpy(void* dst, const void* src, int size) { return std::memcpy(dst, src, size); }

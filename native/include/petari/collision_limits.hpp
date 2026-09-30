#pragma once
#include <cmath>

namespace PetariNative {
// 1024 subdivisions already cover nearly 36,000 world units in one frame.
// Larger/non-finite requests are invalid actor movement, not safe sweep work.
// Return zero to reject before converting to s32 (or adding to that s32).
inline int binderSweepSteps(float length) {
    const float subdivisions = (1.0f / 35.0f) * length;
    if (!std::isfinite(subdivisions) || subdivisions < 0.0f || subdivisions >= 1024.0f) return 0;
    return static_cast<int>(subdivisions) + 1;
}
}

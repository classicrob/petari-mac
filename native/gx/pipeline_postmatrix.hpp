#pragma once
#include "gx.hpp"

namespace aurora::gx {
// XF post-transform memory is 64 four-float rows. Selectors address rows, not
// complete matrices, and the shader wraps each of its three row reads at 64.
inline std::array<Vec4<float>, 64> petariPostMatrixRows{};
constexpr unsigned petari_postmatrix_row(unsigned selector, unsigned row = 0) {
  return (selector - GX_PTTEXMTX0 + row) & 63u;
}
} // namespace aurora::gx

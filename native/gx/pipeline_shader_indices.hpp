#pragma once
#include "gx.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace aurora::gx {
inline void petari_shader_index_diagnostic(const char* field, unsigned value, unsigned replacement) {
  static std::atomic<unsigned> messages{0};
  if (messages.fetch_add(1, std::memory_order_relaxed) < 32)
    std::fprintf(stderr, "[gx shader index] field=%s value=%u replacement=%u\n", field, value, replacement);
}

// Keep serialized/API configurations within the GX register field widths before
// either shader analysis or source generation indexes host arrays. NULL indirect
// orders use slot zero, as in RVL_SDK/gx/GXBump.c; direct NULL maps stay disabled.
inline ShaderConfig petari_shader_indices(ShaderConfig config) {
  const auto bound = [](const char* field, unsigned value, unsigned maximum) {
    if (value <= maximum) return value;
    petari_shader_index_diagnostic(field, value, maximum);
    return maximum;
  };
  config.tevStageCount = std::max(1u, bound("tevStageCount", config.tevStageCount, config.tevStages.size()));
  config.numIndStages = bound("numIndStages", config.numIndStages, config.indStages.size());
  for (unsigned i = 0; i < config.tevStageCount; ++i) {
    auto& stage = config.tevStages[i];
    stage.colorOp.outReg = static_cast<GXTevRegID>(unsigned(stage.colorOp.outReg) & 3u);
    stage.alphaOp.outReg = static_cast<GXTevRegID>(unsigned(stage.alphaOp.outReg) & 3u);
    stage.tevSwapRas = static_cast<GXTevSwapSel>(unsigned(stage.tevSwapRas) & 3u);
    stage.tevSwapTex = static_cast<GXTevSwapSel>(unsigned(stage.tevSwapTex) & 3u);
    if (stage.texMapId != GX_TEXMAP_NULL)
      stage.texMapId = static_cast<GXTexMapID>(unsigned(stage.texMapId) & 7u);
    if (stage.texCoordId != GX_TEXCOORD_NULL)
      stage.texCoordId = static_cast<GXTexCoordID>(unsigned(stage.texCoordId) & 7u);
    else if (stage.texMapId != GX_TEXMAP_NULL)
      stage.texCoordId = GX_TEXCOORD0;
    if (unsigned(stage.channelId) > GX_ALPHA_BUMPN && stage.channelId != GX_COLOR_NULL)
      stage.channelId = GX_COLOR_ZERO;
  }
  for (unsigned i = 0; i < config.numIndStages; ++i) {
    auto& stage = config.indStages[i];
    if (stage.texMapId == GX_TEXMAP_NULL) {
      petari_shader_index_diagnostic("indirect.texMapId", unsigned(stage.texMapId), 0);
      stage.texMapId = GX_TEXMAP0;
    }
    stage.texMapId = static_cast<GXTexMapID>(unsigned(stage.texMapId) & 7u);
    if (stage.texCoordId == GX_TEXCOORD_NULL) stage.texCoordId = GX_TEXCOORD0;
    stage.texCoordId = static_cast<GXTexCoordID>(unsigned(stage.texCoordId) & 7u);
  }
  for (auto& tcg : config.tcgs) {
    tcg.embossSrc &= 7u;
    tcg.postMtx = static_cast<GXPTTexMtx>(GX_PTTEXMTX0 + ((unsigned(tcg.postMtx) - GX_PTTEXMTX0) & 63u));
  }
  return config;
}
} // namespace aurora::gx

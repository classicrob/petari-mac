#pragma once
#include <array>
#include <cstddef>
#include <type_traits>

static_assert(std::is_same_v<decltype(&GXSetArray), void (*)(GXAttr, const void*, u32, u8, bool)>,
              "Native vertex arrays need byte length, stride, and endianness");
static_assert(std::is_same_v<decltype(&GXEnd), void (*)()>);

using GxAbiSnapshot = std::array<std::size_t, 25>;

static inline GxAbiSnapshot gxAbiSnapshot() {
    return {sizeof(GXTexObj), sizeof(GXTlutObj), sizeof(GXLightObj),
            sizeof(GXColor), sizeof(GXColorS10), sizeof(GXVtxAttrFmtList),
            sizeof(GXVtxDescList), sizeof(GXRenderModeObj), sizeof(GXFogAdjTable),
            offsetof(GXVtxAttrFmtList, frac), offsetof(GXRenderModeObj, xFBmode),
            offsetof(GXRenderModeObj, sample_pattern), offsetof(GXRenderModeObj, vfilter),
            GX_VA_POS, GX_VA_CLR0, GX_RGBA8, GX_TRIANGLES, GX_TF_RGBA8, GX_TF_C8,
            GX_ORTHOGRAPHIC, GX_PASSCLR, GX_COLOR0A0, GX_PNMTX0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL};
}

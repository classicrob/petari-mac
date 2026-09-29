#include <dolphin/gx.h>
#include "dolphin/gx/__gx.h"
#include "gfx/texture.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
float depthScale = 16777216.0f;
float depthOffset = 0.0f;
}
extern "C" float petari_gx_depth_scale() { return depthScale; }
extern "C" float petari_gx_depth_offset() { return depthOffset; }

extern "C" {
void GXGetTexObjAll(const GXTexObj* object, void** data, u16* width, u16* height,
                    GXTexFmt* format, GXTexWrapMode* wrapS, GXTexWrapMode* wrapT, GXBool* mipmap) {
    const auto& texture = *reinterpret_cast<const GXTexObj_*>(object);
    *data = const_cast<void*>(texture.data);
    *width = texture.width();
    *height = texture.height();
    *format = static_cast<GXTexFmt>(texture.format());
    *wrapS = texture.wrap_s();
    *wrapT = texture.wrap_t();
    *mipmap = texture.has_mips();
}
void GXGetTexObjLODAll(const GXTexObj* object, GXTexFilter* minFilter, GXTexFilter* magFilter,
                       f32* minLod, f32* maxLod, f32* bias, GXBool* biasClamp, GXBool* edgeLod, GXAnisotropy* aniso) {
    const auto& texture = *reinterpret_cast<const GXTexObj_*>(object);
    *minFilter = texture.min_filter();
    *magFilter = texture.mag_filter();
    *minLod = texture.min_lod();
    *maxLod = texture.max_lod();
    *bias = texture.lod_bias();
    *biasClamp = texture.bias_clamp();
    *edgeLod = texture.do_edge_lod();
    *aniso = texture.max_aniso();
}
void GXLoadTexMtxIndx(u16 index, u32 id, GXTexMtxType type) {
    const u32 address = id >= GX_PTTEXMTX0 ? 0x500 + ((id - GX_PTTEXMTX0) << 2) : id << 2;
    const u32 count = type == GX_MTX2x4 ? 8 : 12;
    GX_WRITE_U8(0x30);
    GX_WRITE_U32((static_cast<u32>(index) << 16) | ((count - 1) << 12) | address);
}
void GXSetScissorBoxOffset(s32 x, s32 y) {
    const u32 horizontal = (static_cast<u32>(x) + 342) >> 1;
    const u32 vertical = (static_cast<u32>(y) + 342) >> 1;
    GX_WRITE_RAS_REG(0x59000000 | (horizontal & 0x3FF) | ((vertical & 0x3FF) << 10));
}
void GXSetZScaleOffset(f32 scale, f32 offset) {
    depthScale = scale * 16777215.0f + 1.0f;
    depthOffset = offset * 16777215.0f;
    GXSetViewport(__gx->vpLeft, __gx->vpTop, __gx->vpWd, __gx->vpHt, __gx->vpNearz, __gx->vpFarz);
}
void GXSetMisc(GXMiscToken token, u32 value) {
    switch (static_cast<u32>(token)) {
    case GX_MT_NULL: break;
    case GX_MT_XF_FLUSH:
        // The backend omits the Wii's dummy flush primitive, but keeps its SDK state.
        __gx->vNum = value;
        __gx->bpSent = 1;
        if (value) __gx->dirtyState |= 8;
        break;
    case GX_MT_DL_SAVE_CONTEXT: __gx->dlSaveContext = value != 0; break;
    case 3: // GX_MT_ABORT_WAIT_COPYOUT in the Wii SDK.
        // Host abort synchronization is handled by the frame/sync bridge.
        if (value != 0) {
            std::fprintf(stderr, "GX abort copy-out waiting is not integrated yet\n");
            std::abort();
        }
        break;
    default: std::abort();
    }
}

static u32 __GXGetNumXfbLines(u32 efbHt, u32 iScale) {
    if (efbHt == 0 || iScale == 0) {
        std::fprintf(stderr, "Invalid GX display-copy scaling dimensions\n");
        std::abort();
    }
    u32 count, realHt, iScaleD;

    count = (efbHt - 1) * 256;
    realHt = count / iScale + 1;

    iScaleD = iScale;
    if (iScaleD > 0x80 && iScaleD < 0x100) {
        while ((iScaleD & 0x01) == 0) {
            iScaleD >>= 1;
        }

        if ((efbHt % iScaleD) == 0) {
            ++realHt;
        }
    }

    if (realHt > 1024) {
        realHt = 1024;
    }

    return realHt;
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
    u32 iScale;
    iScale = (u32)(256.0f / yScale) & 0x1ff;
    return ((u16)__GXGetNumXfbLines((u32)efbHeight, iScale));
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
    f32 fScale, yScale;
    u32 iScale, tgtHt, realHt;

    if (efbHeight == 0 || xfbHeight == 0 || xfbHeight > 1024) std::abort();
    tgtHt = xfbHeight;
    yScale = (f32)xfbHeight / (f32)efbHeight;
    iScale = (u32)(256.0f / yScale) & 0x1ff;
    realHt = __GXGetNumXfbLines((u32)efbHeight, iScale);

    while (realHt > xfbHeight) {
        tgtHt--;
        yScale = (f32)tgtHt / (f32)efbHeight;
        iScale = (u32)(256.0f / yScale) & 0x1ff;
        realHt = __GXGetNumXfbLines((u32)efbHeight, iScale);
    }

    fScale = yScale;
    while (realHt < xfbHeight) {
        fScale = yScale;
        tgtHt++;
        yScale = (f32)tgtHt / (f32)efbHeight;
        iScale = (u32)(256.0f / yScale) & 0x1ff;
        realHt = __GXGetNumXfbLines((u32)efbHeight, iScale);
    }

    return fScale;
}

}

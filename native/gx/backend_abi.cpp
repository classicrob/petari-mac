#include <dolphin/gx.h>
#include <dolphin/os.h>
#include "abi_snapshot.hpp"
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <petari/gx_commands.hpp>
#include "gx/regs.hpp"

static_assert(PetariNative::GX::extensionOpcode == GX_AURORA);
static_assert(PetariNative::GX::arrayBaseCommand == GX_AURORA_LOAD_ARRAYBASE);

GxAbiSnapshot auroraGxAbiSnapshot() {
    return gxAbiSnapshot();
}

bool auroraCheckLegacyTexture(const void* pixels, u32 physical, const void* palette, u32 palettePhysical) {
    if (OSCachedToPhysical(const_cast<void*>(pixels)) != physical ||
        OSPhysicalToCached(physical) != pixels ||
        OSUncachedToPhysical(const_cast<void*>(palette)) != palettePhysical ||
        OSPhysicalToUncached(palettePhysical) != palette ||
        OSCachedToUncached(const_cast<void*>(pixels)) != pixels ||
        OSUncachedToCached(const_cast<void*>(palette)) != palette) return false;
    using aurora::gx::fifo::handle_bp;
    using aurora::gx::g_gxState;
    handle_bp(0xFEFFFFFF);
    handle_bp(0x88000000 | 7 | (7 << 10) | (GX_TF_I4 << 20));
    handle_bp(0x84000000);
    handle_bp(0x94000000 | (physical >> 5));
    const auto& texture = g_gxState.loadedTextures[0];
    if (texture.data != pixels || texture.width() != 8 || texture.height() != 8 ||
        texture.format() != GX_TF_I4 || texture.texObjId != 0 || texture.has_mips()) return false;
    handle_bp(0x84003000);
    if (!texture.has_mips() || texture.mip_count() != 4) return false;
    handle_bp(0x88000000 | 7 | (7 << 10) | (GX_TF_C4 << 20));
    handle_bp(0x64000000 | (palettePhysical >> 5));
    handle_bp(0x65000000 | (1 << 10) | 896);
    handle_bp(0x98000000 | (GX_TL_RGB565 << 10) | 896);
    const auto& tlut = g_gxState.loadedTluts[texture.tlut];
    return texture.format() == GX_TF_C4 && tlut.data == palette && tlut.numEntries == 16 && tlut.format == GX_TL_RGB565;
}

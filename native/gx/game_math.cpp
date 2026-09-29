#include "JSystem/JMath/JMath.hpp"
#include <revolution/gx.h>
#include <cstring>
#include <petari/gx_commands.hpp>
#include <revolution/os.h>

extern "C" void* petari_probe_texture_storage(unsigned bytes);

static void loadLegacyTexture(unsigned frame) {
    static auto* pixels = static_cast<u8*>(petari_probe_texture_storage(32));
    static auto* palette = static_cast<u8*>(petari_probe_texture_storage(32));
    static bool initialized = false;
    if (!initialized) {
        for (unsigned i = 0; i < 32; ++i) pixels[i] = static_cast<u8>((i % 16) * 17);
        for (unsigned i = 0; i < 16; ++i) {
            const u16 color = static_cast<u16>((i * 2 << 11) | ((15 - i) * 4 << 5) | i * 2);
            palette[2 * i] = static_cast<u8>(color >> 8);
            palette[2 * i + 1] = static_cast<u8>(color);
        }
        initialized = true;
    }
    const bool indexed = (frame / 60) % 2 != 0;
    alignas(32) u8 commands[64]{};
    unsigned offset = 0;
    auto bp = [&](u32 value) {
        commands[offset++] = 0x61;
        for (int shift = 24; shift >= 0; shift -= 8) commands[offset++] = static_cast<u8>(value >> shift);
    };
    bp(0xFEFFFFFF);
    bp(0x80000000);
    bp(0x84000000);
    bp(0x88000000 | 7 | (7 << 10) | ((indexed ? GX_TF_C4 : GX_TF_I4) << 20));
    bp(0x94000000 | static_cast<u32>(OSCachedToPhysical(pixels) >> 5));
    if (indexed) {
        bp(0x64000000 | static_cast<u32>(OSCachedToPhysical(palette) >> 5));
        bp(0x65000000 | (1 << 10) | 896);
        bp(0x98000000 | (GX_TL_RGB565 << 10) | 896);
    }
    GXCallDisplayList(commands, sizeof(commands));
}

extern "C" void petari_probe_matrix(unsigned frame, float matrix[3][4]) {
    Quaternion rotation;
    JMAEulerToQuat(0, 0, static_cast<s16>(frame * 128), &rotation);
    PSMTXQuat(matrix, &rotation);
}

extern "C" void petari_probe_draw(unsigned frames) {
        GXSetCopyClear(GXColor{12, 18, 32, 255}, 0x00ffffff);
        GXSetViewport(0, 0, 640, 480, 0, 1);
        GXSetScissor(0, 0, 640, 480);
        GXSetCullMode(GX_CULL_NONE);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(1);
        GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
        GXSetTexCoordScaleManually(GX_TEXCOORD0, GX_TRUE, 8, 8);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        loadLegacyTexture(frames);
        const Mtx44 projection = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -0.1f, -1}, {0, 0, 0, 1}};
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        Mtx model;
        petari_probe_matrix(frames, model);
        GXLoadPosMtxImm(model, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
        GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        static const float positions[3][3] = {{0, 0.65f, -1}, {-0.65f, -0.55f, -1}, {0.65f, -0.55f, -1}};
        static const GXColor colors[3] = {{255, 190, 50, 255}, {80, 170, 255, 255}, {245, 100, 150, 255}};
        static unsigned char bigEndianPositions[sizeof(positions)];
        static bool prepared = false;
        if (!prepared) {
            for (unsigned i = 0; i < 9; ++i) {
                u32 bits;
                std::memcpy(&bits, reinterpret_cast<const unsigned char*>(positions) + i * 4, 4);
                bigEndianPositions[i * 4] = static_cast<u8>(bits >> 24);
                bigEndianPositions[i * 4 + 1] = static_cast<u8>(bits >> 16);
                bigEndianPositions[i * 4 + 2] = static_cast<u8>(bits >> 8);
                bigEndianPositions[i * 4 + 3] = static_cast<u8>(bits);
            }
            prepared = true;
        }
        const bool littleEndian = (frames / 60) % 2 == 0;
        GXSetArray(GX_VA_POS, littleEndian ? static_cast<const void*>(positions) : bigEndianPositions,
                   sizeof(positions), sizeof(positions[0]), littleEndian);
        // Exercise the same big-endian extended command used by J3D's GD lists.
        alignas(32) unsigned char arrayList[32]{};
        const auto command = PetariNative::GX::arrayCommand(0,
            littleEndian ? static_cast<const void*>(positions) : bigEndianPositions,
            sizeof(positions), sizeof(positions[0]), littleEndian);
        std::memcpy(arrayList, command.data(), command.size());
        GXCallDisplayList(arrayList, sizeof(arrayList));
        GXSetArray(GX_VA_CLR0, colors, sizeof(colors), sizeof(colors[0]), true);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        for (u8 i = 0; i < 3; ++i) {
            GXPosition1x8(i);
            GXColor1x8(i);
            GXTexCoord2f32(i == 2 ? 1.0f : 0.0f, i == 0 ? 0.0f : 1.0f);
        }
        GXEnd();
}

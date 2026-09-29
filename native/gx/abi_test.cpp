#include <cstring>
#include <revolution/gx.h>
#include "abi_snapshot.hpp"
#include <cstdio>
#include <cstdint>
#include <revolution/os.h>

GxAbiSnapshot auroraGxAbiSnapshot();
bool auroraCheckLegacyTexture(const void*, u32, const void*, u32);

int main() {
    const auto expected = auroraGxAbiSnapshot();
    const auto actual = gxAbiSnapshot();
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            std::fprintf(stderr, "GX ABI entry %zu: Petari %zu, Aurora %zu\n", i, actual[i], expected[i]);
            return 1;
        }
    }
    GXTexRegion cache{};
    GXInitTexCacheRegion(&cache, GX_TRUE, 0x8000, GX_TEXCACHE_32K, 0x80000, GX_TEXCACHE_128K);
    unsigned char cacheBytes[sizeof(cache)];
    std::memcpy(cacheBytes, &cache, sizeof(cache));
    u32 cacheWords[2];
    std::memcpy(cacheWords, &cache, sizeof(cacheWords));
    if (cacheWords[0] != (0x400 | (3 << 15) | (3 << 18)) ||
        cacheWords[1] != (0x4000 | (4 << 15) | (4 << 18)) ||
        cacheBytes[12] != 1 || cacheBytes[13] != 1) return 5;
    constexpr std::uint64_t guard = 0x12345678abcdef01ULL;
    struct alignas(8) TextureStorage {
        std::uint64_t before = guard;
        GXTexObj object{};
        std::uint64_t after = guard;
    } texture;
    struct alignas(8) PaletteStorage {
        std::uint64_t before = guard;
        GXTlutObj object{};
        std::uint64_t after = guard;
    } palette;
    alignas(32) unsigned char pixels[64]{};
    alignas(32) unsigned char colors[32]{};
    GXInitTexObj(&texture.object, pixels, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_REPEAT, GX_FALSE);
    if (texture.before != guard || texture.after != guard ||
        GXGetTexObjFmt(&texture.object) != GX_TF_RGBA8 || GXGetTexObjMipMap(&texture.object)) return 2;
    GXInitTexObjCI(&texture.object, pixels, 8, 4, GX_TF_C8, GX_CLAMP, GX_CLAMP, GX_FALSE, 3);
    GXInitTlutObj(&palette.object, colors, GX_TL_RGB565, 16);
    if (texture.before != guard || texture.after != guard ||
        palette.before != guard || palette.after != guard || GXGetTexObjTlut(&texture.object) != 3) return 3;
    void* image = nullptr;
    u16 width = 0, height = 0;
    GXTexFmt format;
    GXTexWrapMode wrapS, wrapT;
    GXBool mipmap;
    GXGetTexObjAll(&texture.object, &image, &width, &height, &format, &wrapS, &wrapT, &mipmap);
    if (image != pixels || width != 8 || height != 4 || static_cast<u32>(format) != GX_TF_C8 ||
        wrapS != GX_CLAMP || wrapT != GX_CLAMP || mipmap) return 6;
    GXInitTexObjLOD(&texture.object, GX_LIN_MIP_LIN, GX_LINEAR, 1.5f, 3.5f, -0.75f, GX_TRUE, GX_FALSE, GX_ANISO_4);
    GXTexFilter minFilter, magFilter;
    f32 minLod, maxLod, bias;
    GXBool clamp, edge;
    GXAnisotropy aniso;
    GXGetTexObjLODAll(&texture.object, &minFilter, &magFilter, &minLod, &maxLod, &bias, &clamp, &edge, &aniso);
    if (minFilter != GX_LIN_MIP_LIN || magFilter != GX_LINEAR || minLod != 1.5f || maxLod != 3.5f ||
        bias != -0.75f || !clamp || edge || aniso != GX_ANISO_4) {
        std::fprintf(stderr, "LOD roundtrip: filters %u/%u, lod %g/%g, bias %g, clamp %u, edge %u, aniso %u\n",
            minFilter, magFilter, minLod, maxLod, bias, clamp, edge, aniso);
        return 7;
    }
    if (GXGetNumXfbLines(480, 1.0f) != 480 || GXGetYScaleFactor(480, 480) != 1.0f ||
        GXGetNumXfbLines(240, 2.0f) != 479) return 8;
    void* legacyPixels = OSAllocFromMEM1ArenaLo(64, 32);
    void* legacyPalette = OSAllocFromMEM1ArenaLo(32, 32);
    if (!auroraCheckLegacyTexture(legacyPixels, OSCachedToPhysical(legacyPixels),
                                 legacyPalette, OSCachedToPhysical(legacyPalette))) return 4;
    std::puts("Petari/Aurora GX layouts, signatures, handles and legacy texture/palette addresses passed.");
    return 0;
}

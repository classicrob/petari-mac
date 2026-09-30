// CPU-only calls to Aurora's production texture and palette converters.
#include "gfx/texture_convert.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

static void run() {
    using namespace aurora;
    using namespace aurora::gfx;
    std::array<uint8_t, 4096> image{};
    const ArrayRef<uint8_t> input{image.data(), image.size()};
    unsigned calls = 0;
    for (const auto format : std::array<uint32_t, 11>{GX_TF_I4, GX_TF_I8, GX_TF_IA4, GX_TF_IA8, GX_TF_RGB565,
                             GX_TF_RGB5A3, GX_TF_RGBA8, GX_TF_C4, GX_TF_C8, GX_TF_C14X2, GX_TF_CMPR}) {
        const auto converted = convert_texture(format, 8, 8, 1, input);
        const bool indexed = format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2;
        require(converted.width == 8 && converted.height == 8 && converted.mips == 1, "conversion dimensions changed");
        require(converted.data.size() == 64 * (indexed ? 2 : 4), "unexpected decoded byte count");
        ++calls;
    }
    // The LOAD_TEXOBJ command maps non-CTF depth formats before conversion.
    // Verify each mapped destination, including the launch-star RGBA8 case.
    for (const auto format : {GX_TF_Z8, GX_TF_Z16, GX_TF_Z24X8}) {
        const auto converted = convert_texture(static_cast<uint32_t>(format) & 0xf, 4, 4, 1, input);
        require(converted.data.size() == 4 * 4 * 4, "depth texture sampling conversion failed");
        ++calls;
    }
    for (const auto format : {GX_TF_C4, GX_TF_C8, GX_TF_C14X2}) {
        for (const auto tlut : {GX_TL_IA8, GX_TL_RGB565, GX_TL_RGB5A3}) {
            std::array<uint8_t, 32> palette{};
            if (tlut == GX_TL_IA8) { palette[0] = 0xff; palette[1] = 0xff; }
            if (tlut == GX_TL_RGB565) { palette[0] = 0xf8; palette[1] = 0x00; }
            if (tlut == GX_TL_RGB5A3) { palette[0] = 0xfc; palette[1] = 0x00; }
            const auto converted = convert_texture_palette(format, 8, 8, 1, input, tlut, 16,
                                                            {palette.data(), palette.size()});
            require(converted.data.size() == 8 * 8 * 4, "CI/TLUT conversion returned wrong byte count");
            for (size_t pixel = 0; pixel < 64; ++pixel) {
                const auto* rgba = converted.data.data() + pixel * 4;
                require(rgba[0] == 255 && rgba[3] == 255, "CI/TLUT red or alpha channel changed");
                require(rgba[1] == (tlut == GX_TL_IA8 ? 255 : 0) && rgba[2] == rgba[1],
                        "CI/TLUT channel mapping changed");
            }
            ++calls;
        }
    }
    for (const bool direct : {false, true}) {
        webgpu::g_textureComponentSwizzleSupported = direct;
        webgpu::g_bcTexturesSupported = direct;
        for (const auto format : {GX_TF_R8_PC, GX_TF_RG8_PC, GX_TF_RGBA8_PC, GX_TF_BC1_PC}) {
            const auto converted = convert_texture(format, 8, 8, 1, input);
            require(converted.format == to_wgpu(format), "PC texture format mismatch");
            require(converted.data.empty() == uses_direct_texture_upload(format), "PC direct-upload decision mismatch");
            ++calls;
        }
    }
    std::printf("Texture conversion: %u production calls; base formats, depth sampling destinations, all CI/TLUT pairs and PC feature paths pass\n", calls);
}

int main() {
    try { run(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "Texture conversion test failed: %s\n", error.what()); return 1; }
}

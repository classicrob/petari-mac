#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include <cstdio>
#include <fstream>
#include <string>

using namespace aurora::gx;

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

ShaderConfig indirect_config() {
    ShaderConfig config{};
    config.attrs[GX_VA_POS] = {.attrType = GX_DIRECT, .cnt = 3, .compType = GX_F32, .offset = 0};
    config.vtxStride = 12;
    config.tcgs[0].src = GX_TG_POS;
    config.tevStageCount = 1;
    config.numIndStages = 1;
    config.indStages[0] = {GX_TEXCOORD1, GX_TEXMAP1, GX_ITS_2, GX_ITS_4};
    auto& stage = config.tevStages[0];
    stage.texCoordId = GX_TEXCOORD0;
    stage.texMapId = GX_TEXMAP0;
    stage.colorPass.d = GX_CC_TEXC;
    stage.alphaPass.d = GX_CA_TEXA;
    stage.indTexMtxId = GX_ITM_0;
    return config;
}

void regression_tests() {
    // Observatory draw: one texgen, but indirect order still points at coord 1.
    auto config = indirect_config();
    auto info = build_shader_info(config);
    expect(info.sampledTexCoords.to_ulong() == 1, "inactive indirect coord must select coord 0");
    expect(info.sampledTextures.test(0) && info.sampledTextures.test(1), "both texture maps remain bound");
    expect(info.usedIndStages.test(0) && info.usedIndTexMtxs.test(0), "indirect stage and matrix remain active");
    auto source = build_shader_source(config, DstAlphaMode::None);
    expect(source.find("tex0_uv * ubuf.texcoord_scale[0].xy * vec2f((1.0 / 2.0), (1.0 / 4.0)) / ubuf.tex1_size_bias.xy") != std::string::npos,
           "fallback sample uses coordinate 0 scale, indirect scales, and texture map 1 dimensions");
    expect(source.find("tex1_uv") == std::string::npos, "inactive coordinate is never emitted");

    config.tcgs[1].src = GX_TG_POS;
    info = build_shader_info(config);
    expect(info.sampledTexCoords.to_ulong() == 3, "valid indirect coord 1 remains distinct");
    source = build_shader_source(config, DstAlphaMode::None);
    expect(source.find("tex1_uv * ubuf.texcoord_scale[1].xy") != std::string::npos,
           "valid indirect coordinate keeps its own scale");

    config.tcgs[0] = {};
    config.tcgs[1] = {};
    auto& stage = config.tevStages[0];
    stage.texCoordId = GX_TEXCOORD_NULL;
    stage.texMapId = GX_TEXMAP_NULL;
    stage.colorPass.d = GX_CC_ZERO;
    stage.alphaPass.d = GX_CA_ZERO;
    info = build_shader_info(config);
    expect(info.sampledTexCoords.none(), "zero texgens do not read inactive generators");
    source = build_shader_source(config, DstAlphaMode::None);
    expect(source.find("textureSampleBias(tex1, tex1_samp, vec2f(0.0)") != std::string::npos,
           "zero-texgen indirect read follows Dolphin's zero-coordinate approximation");
}
}

int main(int argc, char** argv) {
    if (argc == 1) {
        regression_tests();
        return failures ? 1 : 0;
    }
    if (argc != 2) return 2;
    PipelineConfig config{};
    std::ifstream in(argv[1], std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(&config), sizeof(config))) return 2;
    const auto& s = config.shaderConfig;
    const auto info = build_shader_info(s);
    for (unsigned i = 0; i < s.tcgs.size(); ++i)
        expect(!info.sampledTexCoords[i] || s.tcgs[i].src != GX_MAX_TEXGENSRC,
               "cached config samples inactive generator");
    if (!failures) {
        const auto source = build_shader_source(s, DstAlphaMode::None);
        expect(!source.empty(), "cached config generates shader source");
    }
    return failures ? 1 : 0;
}

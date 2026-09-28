#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" void petari_probe_matrix(unsigned frame, float matrix[3][4]);

static void logMessage(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(length), message);
    if (level == LOG_FATAL) std::abort();
}

int main(int argc, char** argv) {
    unsigned frameLimit = 0;
    if (argc == 3 && std::strcmp(argv[1], "--frames") == 0) {
        char* end;
        const auto value = std::strtoul(argv[2], &end, 10);
        if (*end || value == 0 || value > 100000) return 2;
        frameLimit = static_cast<unsigned>(value);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: petari_gx_probe [--frames COUNT]\n");
        return 2;
    }
    const auto statePath = (std::filesystem::current_path() / "build/gx-probe-state").string();
    std::filesystem::create_directories(statePath);
    AuroraConfig config{};
    config.appName = "Petari - GX/Metal integration probe";
    config.userPath = statePath.c_str();
    config.cachePath = statePath.c_str();
    config.desiredBackend = BACKEND_METAL;
    config.windowWidth = 960;
    config.windowHeight = 720;
    config.vsync = true;
    config.logCallback = logMessage;
    config.logLevel = LOG_INFO;
    const auto info = aurora_initialize(1, argv, &config);
    if (!info.window || aurora_get_backend() != BACKEND_METAL) {
        std::fprintf(stderr, "Metal initialization failed\n");
        aurora_shutdown();
        return 1;
    }

    unsigned frames = 0;
    bool exiting = false;
    while (!exiting && (!frameLimit || frames < frameLimit)) {
        for (const AuroraEvent* event = aurora_update(); event && event->type != AURORA_NONE; ++event)
            if (event->type == AURORA_EXIT) exiting = true;
        if (exiting || !aurora_begin_frame()) continue;

        GXSetCopyClear(GXColor{12, 18, 32, 255}, GX_MAX_Z24);
        GXSetViewport(0, 0, 640, 480, 0, 1);
        GXSetScissor(0, 0, 640, 480);
        GXSetCullMode(GX_CULL_NONE);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GXSetColorUpdate(GX_TRUE);
        GXSetAlphaUpdate(GX_TRUE);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        const Mtx44 projection = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -0.1f, -1}, {0, 0, 0, 1}};
        GXSetProjection(projection, GX_ORTHOGRAPHIC);
        Mtx model;
        petari_probe_matrix(frames, model);
        GXLoadPosMtxImm(model, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);
        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GXPosition3f32(0, 0.65f, -1); GXColor4u8(255, 190, 50, 255);
        GXPosition3f32(-0.65f, -0.55f, -1); GXColor4u8(80, 170, 255, 255);
        GXPosition3f32(0.65f, -0.55f, -1); GXColor4u8(245, 100, 150, 255);
        GXEnd();
        aurora_end_frame();
        ++frames;
    }
    aurora_shutdown();
    std::printf("Rendered %u frames through native GX/Metal using Petari's quaternion math.\n", frames);
    return frameLimit && frames < frameLimit ? 1 : 0;
}

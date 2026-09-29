// RVLFaceLib rendering through the native GX backend (Aurora/Metal), checked
// by reading the EFB back from the GPU:
// - RFLInitCharModel builds the default Mii's face (it renders the face
//   texture through the EFB and GXCopyTex), then the model is drawn with
//   RFLDrawOpa/RFLDrawXlu, set up as RFLiMakeIcon sets it up;
// - RFLMakeIcon renders an icon and copies it out; a quad textured from the
//   copy shows the icon.
// RFL_Res.dat comes from the disc; RFL's work buffer, the resource, the model,
// and the icon live in JKR heaps over the game arenas, as in MiiFacePartsHolder.
//
//        petari_rfl_render_tests --assets GAME_FILES_DIR
//
// Needs a graphical session with a Metal device. This file is the SDK side;
// rfl_render_aurora.cpp holds everything that includes Aurora.

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <revolution/gx.h>
#include <revolution/mtx.h>
#include <revolution/dvd.h>
#include <revolution/nand.h>
#include <revolution/os.h>

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "RVLFaceLib.h"
#include "archive.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/dvd.hpp"
#include "petari/platform/nand.hpp"
#include "rfl_render_aurora.h"

extern "C" void petari_probe_init_vi();
extern "C" unsigned petari_probe_wait_vi();
extern "C" void petari_shutdown_vi_renderer();

namespace fs = std::filesystem;

namespace {

int checks = 0;
bool gAuroraUp = false;

void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        if (gAuroraUp) {
            petari_shutdown_vi_renderer();
            rfl_render_shutdown();
        }
        std::exit(1);
    }
}

constexpr u16 kWidth = 640;
constexpr u16 kHeight = 480;
const GXColor kBackground = {12, 18, 32, 255};
const GXColor kIconBackground = {220, 30, 30, 255};

std::vector<std::uint8_t> discResource(const std::string& files) {
    PetariNative::HostAllocationScope host;
    std::ifstream in(files + "/ObjectData/MiiFaceDatabase.arc", std::ios::binary);
    check(in.good(), "open MiiFaceDatabase.arc under " + files);
    const std::vector<std::uint8_t> arc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto archive = PetariNative::Resource::Archive::parse({arc.data(), arc.size()});
    for (std::size_t i = 0; i < archive.entries().size(); ++i) {
        const auto& entry = archive.entries()[i];
        if (!entry.isDirectory() && (entry.name == "rfl_res.dat" || entry.name == "RFL_Res.dat")) {
            return archive.resourceData(i);
        }
    }
    check(false, "RFL_Res.dat in MiiFaceDatabase.arc");
    return {};
}

std::uint8_t channel(std::uint32_t argb, int shift) {
    return static_cast<std::uint8_t>(argb >> shift);
}

bool nearColor(std::uint32_t argb, GXColor color, int tolerance) {
    return std::abs(channel(argb, 16) - color.r) <= tolerance && std::abs(channel(argb, 8) - color.g) <= tolerance &&
           std::abs(channel(argb, 0) - color.b) <= tolerance;
}

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

// Screen-space drawing state: pixel coordinates, one TEV stage.
void screenSpace() {
    Mtx44 projection;
    C_MTXOrtho(projection, 0.0f, kHeight, 0.0f, kWidth, 0.0f, 1.0f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    Mtx identity;
    PSMTXIdentity(identity);
    GXLoadPosMtxImm(identity, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0.0f, 0.0f, kWidth, kHeight, 0.0f, 1.0f);
    GXSetScissor(0, 0, kWidth, kHeight);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetNumChans(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
}

// Covers the EFB with a color at the far plane (color and depth).
void fillScreen(GXColor color) {
    screenSpace();
    GXSetTevColor(GX_TEVREG0, color);
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(0.0f, 0.0f, -1.0f);
    GXPosition3f32(kWidth, 0.0f, -1.0f);
    GXPosition3f32(kWidth, kHeight, -1.0f);
    GXPosition3f32(0.0f, kHeight, -1.0f);
    GXEnd();
}

// Draws an RGBA8 texture's color (alpha ignored) as an opaque rectangle.
void drawTexture(void* image, u16 width, u16 height, f32 x, f32 y) {
    screenSpace();
    GXTexObj texture;
    GXInitTexObj(&texture, image, width, height, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&texture, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&texture, GX_TEXMAP0);
    GXSetTevColor(GX_TEVREG0, GXColor{0, 0, 0, 255});
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x, y, -0.5f);
    GXTexCoord2f32(0.0f, 0.0f);
    GXPosition3f32(x + width, y, -0.5f);
    GXTexCoord2f32(1.0f, 0.0f);
    GXPosition3f32(x + width, y + height, -0.5f);
    GXTexCoord2f32(1.0f, 1.0f);
    GXPosition3f32(x, y + height, -0.5f);
    GXTexCoord2f32(0.0f, 1.0f);
    GXEnd();
}

// The camera, light, and passes RFLiMakeIcon uses, on the full EFB.
void drawModel(RFLCharModel* model) {
    GXSetViewport(0.0f, 0.0f, kWidth, kHeight, 0.0f, 1.0f);
    GXSetScissor(0, 0, kWidth, kHeight);
    const f32 aspect = static_cast<f32>(kWidth) / kHeight;
    const f32 fovy = 2.0f * (180.0f / 3.14159265f) * std::atan2(43.2f, 500.0f);
    Mtx44 projection;
    C_MTXPerspective(projection, fovy, aspect, 500.0f, 700.0f);
    GXSetProjection(projection, GX_PERSPECTIVE);
    Vec camera = {0.0f, 34.5f, 600.0f};
    Vec target = {0.0f, 34.5f, 0.0f};
    Vec up = {0.0f, 1.0f, 0.0f};
    Mtx view;
    C_MTXLookAt(view, &camera, &up, &target);
    GXLightObj light;
    GXInitLightColor(&light, GXColor{255, 255, 255, 255});
    Vec lightPos = {1600.0f, 1500.0f, 6000.0f};
    PSMTXMultVec(view, &lightPos, &lightPos);
    GXInitLightPos(&light, lightPos.x, lightPos.y, lightPos.z);
    GXLoadLightObjImm(&light, GX_LIGHT0);
    RFLSetMtx(model, view);

    RFLDrawSetting setting;
    setting.lightEnable = TRUE;
    setting.lightMask = GX_LIGHT0;
    setting.diffuse = GX_DF_CLAMP;
    setting.attn = GX_AF_NONE;
    setting.ambColor = GXColor{160, 160, 160, 255};
    setting.compLoc = 0;
    RFLLoadDrawSetting(&setting);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    RFLDrawOpa(model);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GXSetAlphaUpdate(GX_FALSE);
    RFLDrawXlu(model);
    GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
}

std::uint64_t gTicket = (std::uint64_t(1) << 62);

// One frame: begin, wait for the retrace, draw, end.
template <class Draw>
void frame(const char* what, Draw draw) {
    check(rfl_render_begin_frame(), std::string("begin frame: ") + what);
    petari_probe_wait_vi();
    draw();
    rfl_render_end_frame();
}

}  // namespace

extern "C" int rfl_render_main(int argc, char** argv) {
    if (argc != 3 || std::strcmp(argv[1], "--assets") != 0) {
        std::fprintf(stderr, "Usage: %s --assets GAME_FILES_DIR\n", argv[0]);
        return 2;
    }
    const std::string files = argv[2];

    // Startup order of petari_gx_probe: Aurora, VI (OSInit), then heaps.
    check(rfl_render_initialize(1, argv), "Aurora with Metal");
    gAuroraUp = true;
    petari_probe_init_vi();

    fs::path nandRoot;
    {
        PetariNative::HostAllocationScope host;
        nandRoot = fs::temp_directory_path() / ("petari-rfl-render-" + std::to_string(::getpid()));
        fs::remove_all(nandRoot);
        fs::create_directories(nandRoot);
        std::string error;
        check(PetariNative::Platform::NAND::mount(nandRoot, &error), "mount NAND root: " + error);
    }
    {
        // As the application starts: the disc first, so NANDInit finds the
        // title's home directory from its disc ID.
        PetariNative::HostAllocationScope host;
        std::string error;
        check(PetariNative::Platform::DVD::mount({fs::path(files).parent_path()}, &error), "mount the disc: " + error);
    }
    DVDInit();
    check(PetariNative::Platform::DVD::mountInfo().hasDiskId, "disc ID from sys/boot.bin");
    check(NANDInit() == NAND_RESULT_OK, "NANDInit");
    const std::vector<std::uint8_t> resourceBytes = discResource(files);

    JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
    check(root != nullptr, "JKR root heap over the game arenas");
    JKRExpHeap* scene = JKRExpHeap::create(8 * 1024 * 1024, root, false);
    check(scene != nullptr, "scene heap");
    scene->becomeCurrentHeap();
    void* fifo = root->alloc(0x40000, 32);
    check(fifo != nullptr && GXInit(fifo, 0x40000) != nullptr, "GXInit");

    // MiiFacePartsHolder::init: work buffer and the disc resource in the scene heap.
    void* work = scene->alloc(RFLGetWorkSize(FALSE), 32);
    void* resource = scene->alloc(static_cast<u32>(resourceBytes.size()), 32);
    check(work != nullptr && resource != nullptr, "RFL buffers in the scene heap");
    std::memcpy(resource, resourceBytes.data(), resourceBytes.size());
    const RFLErrcode start = RFLInitResAsync(work, resource, static_cast<u32>(resourceBytes.size()), FALSE);
    check(start == RFLErrcode_Busy || start == RFLErrcode_Success, "RFLInitResAsync");
    RFLErrcode status = RFLErrcode_Busy;
    for (int i = 0; i < 600 && status == RFLErrcode_Busy; ++i) {
        status = RFLGetAsyncStatus();
        if (status == RFLErrcode_Busy) {
            OSSleepTicks(OSMillisecondsToTicks(16));
        }
    }
    check(status == RFLErrcode_Success, "RFL boots with the disc resource and no Mii database");

    // The default Mii's face, as FileSelectItem::createMii builds it.
    auto* model = static_cast<RFLCharModel*>(scene->alloc(sizeof(RFLCharModel), 32));
    const u32 modelBytes = RFLGetModelBufferSize(RFLResolution_256, RFLExpFlag_Normal);
    void* modelBuffer = scene->alloc(modelBytes, 32);
    check(model != nullptr && modelBuffer != nullptr, "model buffers in the scene heap");
    void* iconBuffer = scene->alloc(128 * 128 * 4, 32);
    check(iconBuffer != nullptr, "icon buffer in the scene heap");
    std::memset(iconBuffer, 0, 128 * 128 * 4);
    // Everything is allocated; rendering must not touch the scene heap.
    const s32 sceneFreeBefore = scene->getTotalFreeSize();

    RFLErrcode modelResult = RFLErrcode_Fatal;
    frame("model init", [&] {
        modelResult = RFLInitCharModel(model, RFLDataSource_Default, nullptr, 0, modelBuffer, RFLResolution_256,
                                       RFLExpFlag_Normal);
    });
    check(modelResult == RFLErrcode_Success, "RFLInitCharModel(default Mii 0) succeeds");

    frame("model draw", [&] {
        fillScreen(kBackground);
        drawModel(model);
        const std::uint64_t ticket = ++gTicket;
        RflRenderStats stats{};
        check(rfl_render_capture(ticket, &stats), "EFB readback after drawing the model");
        std::uint32_t corner = 0;
        std::uint32_t center = 0;
        check(rfl_render_pixel(ticket, 2, 2, &corner) && rfl_render_pixel(ticket, kWidth / 2, kHeight / 2, &center),
              "read model pixels");
        std::fprintf(stderr, "Model: %u pixels differ from the background, %u have depth; center %08x, corner %08x\n",
                     stats.differentColorPixels, stats.writtenDepthPixels, center, corner);
        check(nearColor(corner, kBackground, 2), "model frame: background where there is no face (" + hex(corner) + ")");
        check(stats.differentColorPixels >= 20000, "the face covers the middle of the frame (" +
                                                       std::to_string(stats.differentColorPixels) + " pixels)");
        check(!nearColor(center, kBackground, 8), "the frame center is face, not background (" + hex(center) + ")");
        check(channel(center, 16) > channel(center, 0), "the face center is skin-toned (red over blue): " + hex(center));

        // A second capture while the first is still held: each ticket keeps
        // its own pixels (capture buffers are shared and pooled).
        fillScreen(kIconBackground);
        const std::uint64_t filled = ++gTicket;
        RflRenderStats filledStats{};
        check(rfl_render_capture(filled, &filledStats), "EFB readback after filling the frame");
        std::uint32_t filledCenter = 0;
        std::uint32_t heldCenter = 0;
        check(rfl_render_pixel(filled, kWidth / 2, kHeight / 2, &filledCenter) &&
                  rfl_render_pixel(ticket, kWidth / 2, kHeight / 2, &heldCenter),
              "read both captures");
        check(nearColor(filledCenter, kIconBackground, 2), "the second capture sees the fill (" + hex(filledCenter) + ")");
        check(heldCenter == center, "the held capture is unchanged by the second (" + hex(heldCenter) + ")");
        rfl_render_retire(filled);
    });

    // RFLMakeIcon renders into the EFB and copies it out to the buffer.
    RFLIconSetting icon;
    icon.width = 128;
    icon.height = 128;
    icon.bgType = RFLIconBG_Direct;
    icon.bgColor = kIconBackground;
    icon.drawXluOnly = FALSE;
    RFLErrcode iconResult = RFLErrcode_Fatal;
    frame("icon", [&] {
        iconResult = RFLMakeIcon(iconBuffer, RFLDataSource_Default, nullptr, 0, RFLExp_Normal, &icon);
        fillScreen(kBackground);
        drawTexture(iconBuffer, 128, 128, 256.0f, 176.0f);
        const std::uint64_t ticket = ++gTicket;
        RflRenderStats stats{};
        check(rfl_render_capture(ticket, &stats), "EFB readback after drawing the icon");
        std::uint32_t outside = 0;
        std::uint32_t iconCorner = 0;
        std::uint32_t iconCenter = 0;
        check(rfl_render_pixel(ticket, 100, 100, &outside) && rfl_render_pixel(ticket, 258, 178, &iconCorner) &&
                  rfl_render_pixel(ticket, 320, 240, &iconCenter),
              "read icon pixels");
        std::fprintf(stderr, "Icon: %u pixels differ; outside %08x, icon corner %08x, icon center %08x\n",
                     stats.differentColorPixels, outside, iconCorner, iconCenter);
        check(nearColor(outside, kBackground, 2), "outside the icon quad: background");
        check(nearColor(iconCorner, kIconBackground, 8),
              "icon corner is RFL's copy-clear color, so the copy reached the texture (" + hex(iconCorner) + ")");
        check(!nearColor(iconCenter, kIconBackground, 24) && !nearColor(iconCenter, kBackground, 8),
              "icon center is the face (" + hex(iconCenter) + ")");
        check(stats.differentColorPixels >= 128 * 128 / 2, "most of the icon quad differs from the background");
        rfl_render_retire(ticket);
    });
    check(iconResult == RFLErrcode_Success, "RFLMakeIcon(default Mii 0) succeeds");

    check(scene->check() && scene->getTotalFreeSize() == sceneFreeBefore,
          "model and icon rendering left the scene heap as it was");

    RFLExit();
    petari_shutdown_vi_renderer();
    rfl_render_shutdown();
    gAuroraUp = false;
    {
        PetariNative::HostAllocationScope host;
        PetariNative::Platform::NAND::shutdown();
        fs::remove_all(nandRoot);
    }
    std::printf("native RFL render tests passed (%d checks)\n", checks);
    return 0;
}

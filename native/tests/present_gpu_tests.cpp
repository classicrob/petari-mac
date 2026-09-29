// GPU tests for display copies and XFB presentation (native/gx/present) on a
// real Metal device through Aurora:
// - GXCopyDisp snapshots the EFB at its position in the FIFO: later clears
//   and copies do not change an XFB already copied;
// - the display copy's clear, and texture-copy state surviving a display copy;
// - GXSetDispCopyYScale's line count;
// - which XFB is presented and how (VI black, missing copies, dimming,
//   display aspect), and a full present of an XFB with dimming;
// - the presented image rectangle in window points.

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>

#include "gfx/render_worker.hpp"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"
#include <dolphin/mtx/GeoTypes.h>
#include <dolphin/vi.h>

#include "present.h"
#include "present_aurora.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
    std::exit(1);
  }
}

void logMessage(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
  std::fprintf(stderr, "[%s] %.*s\n", module, static_cast<int>(length), message);
  if (level >= LOG_ERROR) {
    std::fputs("FAIL: Aurora reported an error\n", stderr);
    std::exit(1);
  }
}

struct Rgba {
  int r, g, b, a;
};

// Reads one texel of a texture on the render worker, after the work queued so
// far. x and y are fractions of the texture size.
Rgba readAt(const aurora::gfx::TextureRef& ref, float fx, float fy) {
  using namespace aurora::webgpu;
  Rgba result{-1, -1, -1, -1};
  aurora::gfx::render_worker::enqueue_work([&] {
    const wgpu::BufferDescriptor bufferDescriptor{
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = 256,
    };
    const wgpu::Buffer buffer = g_device.CreateBuffer(&bufferDescriptor);
    const wgpu::CommandEncoder encoder = g_device.CreateCommandEncoder();
    const wgpu::TexelCopyTextureInfo source{
        .texture = ref.texture,
        .origin = {static_cast<uint32_t>(ref.size.width * fx), static_cast<uint32_t>(ref.size.height * fy), 0},
    };
    const wgpu::TexelCopyBufferInfo destination{
        .layout = {.bytesPerRow = 256, .rowsPerImage = 1},
        .buffer = buffer,
    };
    const wgpu::Extent3D size{1, 1, 1};
    encoder.CopyTextureToBuffer(&source, &destination, &size);
    const wgpu::CommandBuffer commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    bool done = false;
    bool mapped = false;
    buffer.MapAsync(wgpu::MapMode::Read, 0, 256, wgpu::CallbackMode::AllowProcessEvents,
                    [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                      done = true;
                      mapped = status == wgpu::MapAsyncStatus::Success;
                    });
    while (!done) {
      g_instance.ProcessEvents();
    }
    if (!mapped) {
      return;
    }
    const auto* p = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, 4));
    const bool bgra = ref.format == wgpu::TextureFormat::BGRA8Unorm || ref.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    result = bgra ? Rgba{p[2], p[1], p[0], p[3]} : Rgba{p[0], p[1], p[2], p[3]};
    buffer.Unmap();
  });
  aurora::gfx::render_worker::synchronize();
  return result;
}

// Reads `count` (at most 64) texels of row y starting at x0.
std::vector<Rgba> readRow(const aurora::gfx::TextureRef& ref, uint32_t x0, uint32_t y, uint32_t count) {
  using namespace aurora::webgpu;
  std::vector<Rgba> row;
  aurora::gfx::render_worker::enqueue_work([&] {
    const wgpu::BufferDescriptor bufferDescriptor{
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = 256,
    };
    const wgpu::Buffer buffer = g_device.CreateBuffer(&bufferDescriptor);
    const wgpu::CommandEncoder encoder = g_device.CreateCommandEncoder();
    const wgpu::TexelCopyTextureInfo source{.texture = ref.texture, .origin = {x0, y, 0}};
    const wgpu::TexelCopyBufferInfo destination{.layout = {.bytesPerRow = 256, .rowsPerImage = 1}, .buffer = buffer};
    const wgpu::Extent3D size{count, 1, 1};
    encoder.CopyTextureToBuffer(&source, &destination, &size);
    const wgpu::CommandBuffer commands = encoder.Finish();
    g_queue.Submit(1, &commands);
    bool done = false;
    bool mapped = false;
    buffer.MapAsync(wgpu::MapMode::Read, 0, 256, wgpu::CallbackMode::AllowProcessEvents,
                    [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                      done = true;
                      mapped = status == wgpu::MapAsyncStatus::Success;
                    });
    while (!done) {
      g_instance.ProcessEvents();
    }
    if (!mapped) {
      return;
    }
    const auto* p = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, count * 4));
    const bool bgra = ref.format == wgpu::TextureFormat::BGRA8Unorm || ref.format == wgpu::TextureFormat::BGRA8UnormSrgb;
    for (uint32_t i = 0; i < count; ++i, p += 4) {
      row.push_back(bgra ? Rgba{p[2], p[1], p[0], p[3]} : Rgba{p[0], p[1], p[2], p[3]});
    }
    buffer.Unmap();
  });
  aurora::gfx::render_worker::synchronize();
  return row;
}

Rgba readCenter(const aurora::gfx::TextureRef& ref) {
  return readAt(ref, 0.5f, 0.5f);
}

bool near(const Rgba& c, int r, int g, int b) {
  return std::abs(c.r - r) <= 2 && std::abs(c.g - g) <= 2 && std::abs(c.b - b) <= 2;
}

std::string describe(const Rgba& c) {
  return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + ")";
}

const aurora::gfx::TextureHandle* copyOf(const void* dest) {
  const auto& copies = aurora::gx::g_gxState.copyTextures;
  const auto it = copies.find(dest);
  return it == copies.end() ? nullptr : &it->second.handle;
}

// A copy that must exist (a missing one fails the test instead of crashing).
const aurora::gfx::TextureRef& requireCopy(const void* dest, const std::string& label) {
  const aurora::gfx::TextureHandle* handle = copyOf(dest);
  check(handle != nullptr && *handle, label + ": the copy exists");
  return **handle;
}

void pumpEvents() {
  for (const AuroraEvent* event = aurora_update(); event != nullptr && event->type != AURORA_NONE; ++event) {
  }
}

bool beginFrame() {
  for (int tries = 0; tries < 200; tries++) {
    pumpEvents();
    if (aurora_begin_frame()) {
      return true;
    }
  }
  return false;
}

alignas(32) unsigned char gFifo[0x10000];
alignas(32) unsigned char gXfbA[16], gXfbB[16], gXfbC[16], gXfbE[16], gTexD[16], gNever[16];

void testDisplayCopies() {
  check(beginFrame(), "first frame opens");
  GXInit(gFifo, sizeof(gFifo));
  GXSetColorUpdate(GX_TRUE);
  GXSetAlphaUpdate(GX_FALSE);
  GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

  GXSetDispCopySrc(0, 0, 640, 480);
  GXSetDispCopyDst(640, 480);
  check(GXSetDispCopyYScale(1.0f) == 480, "Y scale 1 keeps 480 lines");
  check(GXSetDispCopyYScale(2.0f) == 959, "Y scale 2 doubles the lines, as the hardware counts them");
  check(GXSetDispCopyYScale(1.0f) == 480, "Y scale back to 1");
  GXSetCopyFilter(GX_FALSE, nullptr, GX_FALSE, nullptr);
  GXSetDispCopyGamma(GX_GM_1_0);
  GXSetCopyClamp(static_cast<GXFBClamp>(GX_CLAMP_TOP | GX_CLAMP_BOTTOM));

  GXSetTexCopySrc(0, 0, 64, 32);
  GXSetTexCopyDst(64, 32, GX_TF_RGBA8, GX_FALSE);

  const u32 z = 0xFFFFFF;
  GXSetCopyClear(GXColor{255, 0, 0, 255}, z);
  GXCopyDisp(gXfbA, GX_TRUE);  // A: the EFB as it was; the EFB becomes red
  GXSetCopyClear(GXColor{0, 255, 0, 255}, z);
  GXCopyDisp(gXfbB, GX_TRUE);  // B: red; the EFB becomes green
  GXCopyDisp(gXfbC, GX_FALSE);  // C: green; no clear
  GXSetCopyClear(GXColor{0, 0, 255, 255}, z);
  GXCopyTex(gTexD, GX_TRUE);  // D: green, with the texture-copy settings; the EFB becomes blue
  GXCopyDisp(gXfbE, GX_FALSE);  // E: blue in the texture copy's rectangle, green elsewhere
  GXSetCopyClear(GXColor{255, 255, 0, 255}, z);
  GXCopyDisp(gXfbA, GX_TRUE);  // A again: as E; the EFB becomes yellow
  aurora_end_frame();

  const auto* a = copyOf(gXfbA);
  const auto* b = copyOf(gXfbB);
  const auto* c = copyOf(gXfbC);
  const auto* d = copyOf(gTexD);
  const auto* e = copyOf(gXfbE);
  check(a && b && c && d && e && *a && *b && *c && *d && *e, "every copy has a texture, kept by address");
  check(b->get() != c->get() && c->get() != e->get(), "each XFB address has its own texture");
  const Rgba colorB = readCenter(**b);
  const Rgba colorC = readCenter(**c);
  const Rgba colorD = readCenter(**d);
  const Rgba colorE = readCenter(**e);
  const Rgba colorA = readCenter(**a);
  // A texture copy clears only its source rectangle (here the top-left 64x32
  // of 640x480), so the rest of the EFB stays green.
  const Rgba cornerE = readAt(**e, 0.05f, 0.03f);
  const Rgba cornerA = readAt(**a, 0.05f, 0.03f);
  check(near(colorB, 255, 0, 0), "B holds the EFB at its copy (red), got " + describe(colorB));
  check(near(colorC, 0, 255, 0), "C holds the EFB after B's clear (green), got " + describe(colorC));
  check(near(colorD, 0, 255, 0), "texture copy between display copies sees green, got " + describe(colorD));
  check(near(cornerE, 0, 0, 255), "E sees the texture copy's clear in its rectangle (blue), got " + describe(cornerE));
  check(near(colorE, 0, 255, 0), "and the EFB outside it unchanged (green), got " + describe(colorE));
  check(near(cornerA, 0, 0, 255) && near(colorA, 0, 255, 0),
        "copying to A again replaces its first image, got " + describe(cornerA) + describe(colorA));
  const auto& xfbSize = (*b)->size;
  const auto& texSize = (*d)->size;
  check(texSize.width * 10 == xfbSize.width || std::abs(static_cast<int>(texSize.width * 10) - static_cast<int>(xfbSize.width)) <= 10,
        "texture copy kept its 64-wide destination after display copies");
  check(std::abs(static_cast<int>(texSize.height * 15) - static_cast<int>(xfbSize.height)) <= 15,
        "texture copy kept its 32-line destination after display copies");
}

void testPresentSelection() {
  using petari_present::take;
  check(!take().active, "presentation off until enabled: upstream EFB present");
  petari_present_enable();

  PetariPresentVideo video{gXfbB, 1, 0, 0, 16, 9};
  petari_present_set_video(&video);
  auto frame = take();
  check(frame.active && !frame.black && copyOf(gXfbB) != nullptr && frame.xfb == *copyOf(gXfbB), "the latched XFB's copy is presented");
  check(frame.aspectWidth == 16 && frame.aspectHeight == 9 && frame.dimOpacity == 0.0f, "16:9, not dimmed");

  video.dimmed = 1;
  petari_present_set_video(&video);
  frame = take();
  check(!frame.black && frame.dimOpacity > 0.0f && frame.dimOpacity < 1.0f, "dimming darkens partly");

  video.black = 1;
  petari_present_set_video(&video);
  check(take().black, "VI black presents black");
  video.black = 0;
  video.configured = 0;
  petari_present_set_video(&video);
  check(take().black, "no render mode presents black");
  video.configured = 1;
  video.xfb = gNever;
  petari_present_set_video(&video);
  frame = take();
  check(frame.active && frame.black && !frame.xfb, "an XFB never copied to presents black");
  video.xfb = nullptr;
  petari_present_set_video(&video);
  check(take().black, "no XFB presents black");

  video = {gXfbC, 1, 0, 0, 4, 3};
  petari_present_set_video(&video);
  frame = take();
  check(frame.aspectWidth == 4 && frame.aspectHeight == 3, "4:3");

  float x = 0, y = 0, w = 0, h = 0;
  check(petari_present_image_rect(&x, &y, &w, &h) == 1, "image rectangle known");
  check(std::fabs(w / h - 4.0f / 3.0f) < 0.01f && x >= 0 && y >= 0 && w <= 960.5f && h <= 720.5f,
        "image rectangle is the 4:3 viewport in window points");
  video.aspectWidth = 16;
  video.aspectHeight = 9;
  petari_present_set_video(&video);
  check(petari_present_image_rect(&x, &y, &w, &h) == 1 && std::fabs(w / h - 16.0f / 9.0f) < 0.01f &&
            std::fabs(w - 960.0f) < 1.0f && y > 0.0f,
        "16:9 letterboxed in a 4:3 window");
}

alignas(32) unsigned char gXfbFrame[16], gFrameScratch[16];

void testPresentFrames() {
  // Full frames through the patched aurora_end_frame: a dimmed XFB, then VI
  // black, then a missing XFB. Aurora aborts the test on any device error.
  // The presented XFB is copied in each frame (red), because changing the
  // display aspect resizes the EFB, and Aurora clears all copy textures on
  // an EFB resize.
  PetariPresentVideo video{gXfbFrame, 1, 0, 1, 16, 9};
  for (int frame = 0; frame < 3; frame++) {
    petari_present_set_video(&video);
    check(beginFrame(), "frame opens");
    GXSetCopyClear(GXColor{255, 0, 0, 255}, 0xFFFFFF);
    GXCopyDisp(gFrameScratch, GX_TRUE);  // the EFB becomes red
    GXCopyDisp(gXfbFrame, GX_FALSE);
    aurora_end_frame();
    if (frame == 0) {
      video.black = 1;
    } else {
      video = {gNever, 1, 0, 0, 4, 3};
    }
  }
  aurora::gfx::render_worker::synchronize();
  check(near(readCenter(requireCopy(gXfbFrame, "presented XFB")), 255, 0, 0), "presenting does not change the XFB copy");
}

// Changing the display aspect resizes the EFB, and Aurora drops every copy
// texture on a resize: the latched XFB is black until the game copies it
// again (the next frame), then shown in the new size.
void testAspectResize() {
  PetariPresentVideo video{gXfbFrame, 1, 0, 0, 4, 3};
  petari_present_set_video(&video);
  check(beginFrame(), "frame opens");
  GXSetCopyClear(GXColor{0, 255, 0, 255}, 0xFFFFFF);
  GXCopyDisp(gFrameScratch, GX_TRUE);
  GXCopyDisp(gXfbFrame, GX_FALSE);
  aurora_end_frame();
  const AuroraWindowSize before = aurora::window::get_window_size();
  check(copyOf(gXfbFrame) != nullptr, "copy before the resize");

  video.aspectWidth = 16;
  video.aspectHeight = 9;
  petari_present_set_video(&video);
  check(beginFrame(), "frame opens after the aspect change");  // the resize happens here
  check(copyOf(gXfbFrame) == nullptr, "the resize dropped the old copy");
  const auto frame = petari_present::take();
  check(frame.active && frame.black, "a dropped XFB presents black, not stale pixels");
  GXSetCopyClear(GXColor{0, 255, 0, 255}, 0xFFFFFF);
  GXCopyDisp(gFrameScratch, GX_TRUE);
  GXCopyDisp(gXfbFrame, GX_FALSE);
  aurora_end_frame();
  aurora::gfx::render_worker::synchronize();
  const AuroraWindowSize after = aurora::window::get_window_size();
  const auto& copy = requireCopy(gXfbFrame, "copy after the resize");
  std::fprintf(stderr, "present resize: EFB %ux%u (4:3) -> %ux%u (16:9), XFB %ux%u\n", before.fb_width,
               before.fb_height, after.fb_width, after.fb_height, copy.size.width, copy.size.height);
  check(after.fb_height < before.fb_height || after.fb_width < before.fb_width, "the EFB took the new aspect");
  check(copy.size.width == after.fb_width && copy.size.height == after.fb_height, "the new copy has the new size");
  check(near(readCenter(copy), 0, 255, 0), "and the new image");
}

alignas(32) unsigned char gXfbGame[16], gScratch[16];

// Draws an untextured quad in logical EFB coordinates.
void drawRect(float x0, float y0, float x1, float y1, GXColor color) {
  Mtx44 ortho{};
  const float l = 0.f, r = 640.f, t = 0.f, b = 456.f, n = 0.f, f = 1.f;
  ortho[0][0] = 2.f / (r - l);
  ortho[0][3] = -(r + l) / (r - l);
  ortho[1][1] = 2.f / (t - b);
  ortho[1][3] = -(t + b) / (t - b);
  ortho[2][2] = -1.f / (f - n);
  ortho[2][3] = -f / (f - n);
  ortho[3][3] = 1.f;
  GXSetProjection(ortho, GX_ORTHOGRAPHIC);
  Mtx identity{};
  identity[0][0] = identity[1][1] = identity[2][2] = 1.f;
  GXLoadPosMtxImm(identity, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXSetViewport(0.f, 0.f, 640.f, 456.f, 0.f, 1.f);
  GXSetScissor(0, 0, 640, 456);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTexGens(0);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetCullMode(GX_CULL_NONE);
  GXBegin(GX_QUADS, GX_VTXFMT0, 4);
  GXPosition3f32(x0, y0, 0.f);
  GXColor4u8(color.r, color.g, color.b, color.a);
  GXPosition3f32(x1, y0, 0.f);
  GXColor4u8(color.r, color.g, color.b, color.a);
  GXPosition3f32(x1, y1, 0.f);
  GXColor4u8(color.r, color.g, color.b, color.a);
  GXPosition3f32(x0, y1, 0.f);
  GXColor4u8(color.r, color.g, color.b, color.a);
  GXEnd();
}

// The game's render mode (RenderMode.cpp GXNtscIntDf: 640x456 EFB, 456 XFB
// lines) rendered and copied as MainLoopFramework does. Measures that nothing
// on the way to the screen lowers the resolution: the XFB is as large as the
// drawable, a drawn edge is a hard step at native resolution (a 640-wide
// image scaled up would leave a ramp several pixels wide), and the presented
// viewport shows the XFB pixel for pixel.
// Renders the game's mode with the given display aspect and measures it.
void measureResolution(unsigned aspectW, unsigned aspectH) {
  PetariPresentVideo video{gXfbGame, 1, 0, 0, aspectW, aspectH};
  petari_present_enable();
  petari_present_set_video(&video);  // the EFB takes this aspect from the next frame

  for (int frame = 0; frame < 3; ++frame) {
    check(beginFrame(), "frame opens");
    GXSetDispCopySrc(0, 0, 640, 456);
    GXSetDispCopyDst(640, 456);
    check(GXSetDispCopyYScale(1.0f) == 456, "456 XFB lines");
    GXSetCopyClear(GXColor{0, 0, 0, 255}, 0xFFFFFF);
    GXSetColorUpdate(GX_TRUE);
    GXCopyDisp(gScratch, GX_TRUE);  // clears the EFB to black
    drawRect(100.4f, 100.f, 300.f, 300.f, GXColor{255, 255, 255, 255});
    GXCopyDisp(gXfbGame, GX_FALSE);
    aurora_end_frame();
  }
  aurora::gfx::render_worker::synchronize();

  const std::string label = std::to_string(aspectW) + ":" + std::to_string(aspectH) + ": ";
  const AuroraWindowSize window = aurora::window::get_window_size();
  const auto* xfb = copyOf(gXfbGame);
  check(xfb && *xfb, label + "the game-mode XFB has a copy");
  const auto size = (*xfb)->size;
  std::fprintf(stderr,
               "present resolution %s window %ux%u points, drawable %ux%u, EFB %ux%u, XFB texture %ux%u\n",
               label.c_str(), window.width, window.height, window.native_fb_width, window.native_fb_height,
               window.fb_width, window.fb_height, size.width, size.height);
  check(window.native_fb_width >= window.width, label + "drawable in pixels");
  check(size.width == window.fb_width && size.height == window.fb_height,
        label + "the XFB is as large as the EFB render target");
  // The EFB fills the drawable in the displayed aspect (not 640x456).
  const float drawableAspect = float(window.native_fb_width) / float(window.native_fb_height);
  const float aspect = float(aspectW) / float(aspectH);
  const uint32_t fitW = drawableAspect > aspect ? uint32_t(std::lround(window.native_fb_height * aspect))
                                                : window.native_fb_width;
  const uint32_t fitH = drawableAspect > aspect ? window.native_fb_height
                                                : uint32_t(std::lround(window.native_fb_width / aspect));
  check(std::abs(int(window.fb_width) - int(fitW)) <= 1 && std::abs(int(window.fb_height) - int(fitH)) <= 1,
        label + "the EFB is the drawable fitted to the displayed aspect (" + std::to_string(fitW) + "x" +
            std::to_string(fitH) + ")");

  // The rectangle's left edge at logical x 100.4, on the row at logical y 200.
  const double edge = 100.4 * double(size.width) / 640.0;
  const uint32_t y = uint32_t(200.0 * double(size.height) / 456.0);
  const uint32_t x0 = uint32_t(std::max(0.0, edge - 24.0));
  const std::vector<Rgba> row = readRow(**xfb, x0, y, 48);
  check(row.size() == 48, label + "row read back");
  int firstWhite = -1, partial = 0;
  std::string values;
  for (size_t i = 0; i < row.size(); ++i) {
    const int v = row[i].r;
    if (v > 16 && v < 240) {
      ++partial;
    }
    if (firstWhite < 0 && v >= 240) {
      firstWhite = int(x0 + i);
    }
    values += std::to_string(v) + (i + 1 < row.size() ? " " : "");
  }
  std::fprintf(stderr, "present resolution %s edge row %u from x %u: %s\n", label.c_str(), y, x0, values.c_str());
  check(firstWhite >= 0, label + "the rectangle is drawn");
  check(std::fabs(firstWhite - edge) <= 1.0,
        label + "the edge is at its native position " + std::to_string(edge) + ", found " + std::to_string(firstWhite));
  check(partial == 0, label + "the edge is a hard step at native resolution (" + std::to_string(partial) +
                          " intermediate pixels; a scaled-up 640-wide image would show a ramp)");

  // Presented pixel for pixel.
  float rx = 0, ry = 0, rw = 0, rh = 0;
  check(petari_present_image_rect(&rx, &ry, &rw, &rh) == 1, label + "image rectangle");
  const float pixelW = rw * float(window.native_fb_width) / float(window.width);
  const float pixelH = rh * float(window.native_fb_height) / float(window.height);
  std::fprintf(stderr, "present resolution %s viewport %.0fx%.0f pixels for the XFB %ux%u (scale %.3f x %.3f)\n",
               label.c_str(), pixelW, pixelH, size.width, size.height, pixelW / size.width, pixelH / size.height);
  check(std::fabs(pixelW - float(size.width)) <= 1.f && std::fabs(pixelH - float(size.height)) <= 1.f,
        label + "the XFB is presented pixel for pixel");
}

// The game's render mode (RenderMode.cpp GXNtscIntDf: 640x456 EFB, 456 XFB
// lines) rendered and copied as MainLoopFramework does. Measures that nothing
// on the way to the screen lowers the resolution: the XFB is as large as the
// drawable allows in the displayed aspect, a drawn edge is a hard step at
// native resolution (a 640-wide image scaled up would leave a ramp several
// pixels wide), and the presented viewport shows the XFB pixel for pixel,
// both for the window's own aspect and for 16:9 in this 4:3 window (the case
// that used to be resampled 1.00 x 0.75).
void testGameResolution() {
  GXRenderModeObj mode{};
  mode.viTVmode = VI_TVMODE_NTSC_INT;
  mode.fbWidth = 640;
  mode.efbHeight = 456;
  mode.xfbHeight = 456;
  mode.viWidth = 670;
  mode.viHeight = 456;
  mode.xFBmode = VI_XFBMODE_DF;
  VIConfigure(&mode);
  measureResolution(4, 3);
  measureResolution(16, 9);
}

}  // namespace

int main(int argc, char** argv) {
  // A fresh state directory each run: no pipeline cache from earlier runs, so
  // first-use pipelines are compiled here as in a first game launch.
  const auto state = std::filesystem::temp_directory_path() / "petari-present-tests";
  std::filesystem::remove_all(state);
  std::filesystem::create_directories(state);
  const std::string statePath = state.string();
  AuroraConfig config{};
  config.appName = "Petari present tests";
  config.userPath = statePath.c_str();
  config.cachePath = statePath.c_str();
  config.desiredBackend = BACKEND_METAL;
  config.windowWidth = 960;
  config.windowHeight = 720;
  config.vsync = false;
  config.logCallback = logMessage;
  config.logLevel = LOG_WARNING;
  const AuroraInfo info = aurora_initialize(1, argv, &config);
  (void)argc;
  if (info.window == nullptr || aurora_get_backend() != BACKEND_METAL) {
    std::fputs("FAIL: Metal initialization failed\n", stderr);
    return 1;
  }
  testDisplayCopies();
  testPresentSelection();
  testPresentFrames();
  testAspectResize();
  testGameResolution();
  aurora_shutdown();
  std::printf("native present GPU tests passed (%d checks)\n", checks);
  return 0;
}

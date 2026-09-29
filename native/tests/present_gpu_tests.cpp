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

#include "present.h"
#include "present_aurora.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

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
  check(frame.active && !frame.black && frame.xfb == *copyOf(gXfbB), "the latched XFB's copy is presented");
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

void testPresentFrames() {
  // Full frames through the patched aurora_end_frame: a dimmed XFB, then VI
  // black, then a missing XFB. Aurora aborts the test on any device error.
  PetariPresentVideo video{gXfbB, 1, 0, 1, 16, 9};
  for (int frame = 0; frame < 3; frame++) {
    petari_present_set_video(&video);
    check(beginFrame(), "frame opens");
    GXSetCopyClear(GXColor{0, 0, 0, 255}, 0xFFFFFF);
    GXCopyDisp(gXfbC, GX_TRUE);
    aurora_end_frame();
    if (frame == 0) {
      video.black = 1;
    } else {
      video = {gNever, 1, 0, 0, 4, 3};
    }
  }
  aurora::gfx::render_worker::synchronize();
  check(near(readCenter(**copyOf(gXfbB)), 255, 0, 0), "presenting does not change the XFB copy");
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
  aurora_shutdown();
  std::printf("native present GPU tests passed (%d checks)\n", checks);
  return 0;
}

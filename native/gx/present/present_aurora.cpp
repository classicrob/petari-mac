// Aurora side of XFB presentation (README.md in this directory). Compiled
// with Aurora's lib/ on the quote include path; no SDK headers.

#include "present_aurora.hpp"

#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "present.h"

namespace petari_present {
namespace {

// VI dimming darkens the picture. The Wii's exact level is not documented in
// the SDK; this is an approximation (README.md).
constexpr float kDimOpacity = 0.5f;

bool gEnabled = false;
// The EFB's aspect, read by the patched window size (any thread).
std::atomic<bool> gAspectEnabled{false};
std::atomic<unsigned> gAspectWidth{4};
std::atomic<unsigned> gAspectHeight{3};

// Keeps the EFB's aspect at the displayed image's; a change resizes the frame
// buffer (deferred by Aurora to the next frame boundary).
void setContentAspect(unsigned width, unsigned height) {
  if (width == 0 || height == 0) {
    width = 4;
    height = 3;
  }
  const bool widthChanged = gAspectWidth.exchange(width) != width;
  const bool heightChanged = gAspectHeight.exchange(height) != height;
  if ((widthChanged || heightChanged) && gAspectEnabled.load()) {
    aurora::window::request_frame_buffer_resize();
  }
}
PetariPresentVideo gVideo{};
const void* gMissingXfb = nullptr;  // last XFB reported without a display copy

// 1x1 black with kDimOpacity alpha (premultiplied), for draw_dim.
wgpu::Texture gDimTexture;
wgpu::TextureView gDimView;

// PETARI_TRACE_BOOT: the sizes along the presentation path, logged when they
// change: the drawable (surface), the EFB render target, the latched XFB's
// copy texture, and the viewport it is drawn into. A scale of 1.00 means the
// XFB is shown pixel for pixel; below 1 it is shrunk, above 1 enlarged.
bool traceSizes() {
  static const bool enabled = [] {
    const char* value = std::getenv("PETARI_TRACE_BOOT");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void traceFrame(const Frame& frame) {
  using namespace aurora::webgpu;
  struct Sizes {
    uint32_t surfaceW, surfaceH, efbW, efbH, xfbW, xfbH, viewW, viewH, aspectW, aspectH;
    bool operator==(const Sizes& o) const {
      return surfaceW == o.surfaceW && surfaceH == o.surfaceH && efbW == o.efbW && efbH == o.efbH && xfbW == o.xfbW &&
             xfbH == o.xfbH && viewW == o.viewW && viewH == o.viewH && aspectW == o.aspectW && aspectH == o.aspectH;
    }
  };
  static Sizes last{};
  const uint32_t surfaceW = g_graphicsConfig.surfaceConfiguration.width;
  const uint32_t surfaceH = g_graphicsConfig.surfaceConfiguration.height;
  const Viewport viewport = calculate_present_viewport(surfaceW, surfaceH, frame.aspectWidth, frame.aspectHeight);
  const AuroraWindowSize window = aurora::window::get_window_size();
  const Sizes sizes{surfaceW,
                    surfaceH,
                    window.fb_width,
                    window.fb_height,
                    frame.xfb ? frame.xfb->size.width : 0,
                    frame.xfb ? frame.xfb->size.height : 0,
                    static_cast<uint32_t>(viewport.width),
                    static_cast<uint32_t>(viewport.height),
                    frame.aspectWidth,
                    frame.aspectHeight};
  if (sizes == last || sizes.xfbW == 0) {
    return;
  }
  last = sizes;
  std::fprintf(stderr,
               "Petari present: surface %ux%u, EFB %ux%u, XFB texture %ux%u, viewport %ux%u (%u:%u), scale %.2f x %.2f\n",
               sizes.surfaceW, sizes.surfaceH, sizes.efbW, sizes.efbH, sizes.xfbW, sizes.xfbH, sizes.viewW, sizes.viewH,
               sizes.aspectW, sizes.aspectH, static_cast<double>(viewport.width) / sizes.xfbW,
               static_cast<double>(viewport.height) / sizes.xfbH);
}

aurora::webgpu::TextureWithSampler xfb_source(const aurora::gfx::TextureRef& ref) {
  return {
      .texture = ref.texture,
      .view = ref.sampleTextureView,
      .size = ref.size,
      .format = ref.format,
      .sampler = aurora::webgpu::present_source().sampler,
  };
}

}  // namespace

Frame take() noexcept {
  Frame frame;
  if (!gEnabled) {
    return frame;
  }
  frame.active = true;
  frame.aspectWidth = gVideo.aspectWidth != 0 ? gVideo.aspectWidth : 4;
  frame.aspectHeight = gVideo.aspectHeight != 0 ? gVideo.aspectHeight : 3;
  if (!gVideo.configured || gVideo.black || gVideo.xfb == nullptr) {
    return frame;
  }
  const auto& copies = aurora::gx::g_gxState.copyTextures;
  const auto it = copies.find(gVideo.xfb);
  if (it == copies.end() || !it->second) {
    // An XFB the GPU never copied to (for example one written by the CPU)
    // cannot be shown.
    if (gMissingXfb != gVideo.xfb) {
      gMissingXfb = gVideo.xfb;
      std::fprintf(stderr, "Petari present: XFB %p has no display copy; showing black\n", gVideo.xfb);
    }
    return frame;
  }
  frame.black = false;
  frame.xfb = it->second.handle;
  frame.dimOpacity = gVideo.dimmed ? kDimOpacity : 0.0f;
  if (traceSizes()) {
    traceFrame(frame);
  }
  return frame;
}

bool bind_image(const Frame& frame, wgpu::BindGroup& out) noexcept {
  if (frame.black || !frame.xfb) {
    return false;
  }
  out = aurora::webgpu::create_copy_bind_group(xfb_source(*frame.xfb));
  return true;
}

void draw_dim(const wgpu::RenderPassEncoder& pass, const Frame& frame) noexcept {
  using namespace aurora::webgpu;
  if (!gDimTexture) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Petari VI dimming",
        .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {1, 1, 1},
        .format = wgpu::TextureFormat::RGBA8Unorm,
    };
    gDimTexture = g_device.CreateTexture(&descriptor);
    gDimView = gDimTexture.CreateView();
    const uint8_t pixel[4] = {0, 0, 0, static_cast<uint8_t>(kDimOpacity * 255.0f + 0.5f)};
    const wgpu::TexelCopyTextureInfo destination{.texture = gDimTexture};
    const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
    const wgpu::Extent3D size{1, 1, 1};
    g_queue.WriteTexture(&destination, pixel, sizeof(pixel), &layout, &size);
  }
  (void)frame;
  const TextureWithSampler source{
      .texture = gDimTexture,
      .view = gDimView,
      .size = {1, 1, 1},
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .sampler = present_source().sampler,
  };
  pass.SetPipeline(g_CopyPremultipliedAlphaPipeline);
  pass.SetBindGroup(0, create_copy_bind_group(source), 0, nullptr);
  pass.Draw(3);
}

}  // namespace petari_present

extern "C" {

void petari_present_enable(void) {
  petari_present::gEnabled = true;
  if (!petari_present::gAspectEnabled.exchange(true)) {
    aurora::window::request_frame_buffer_resize();
  }
}

int petari_present_content_aspect(unsigned* width, unsigned* height) {
  if (!petari_present::gAspectEnabled.load()) {
    return 0;
  }
  *width = petari_present::gAspectWidth.load();
  *height = petari_present::gAspectHeight.load();
  return 1;
}

void petari_present_set_video(const PetariPresentVideo* video) {
  petari_present::gVideo = *video;
  petari_present::setContentAspect(video->aspectWidth, video->aspectHeight);
}

int petari_present_image_rect(float* x, float* y, float* width, float* height) {
  using namespace aurora::webgpu;
  const uint32_t surfaceWidth = g_graphicsConfig.surfaceConfiguration.width;
  const uint32_t surfaceHeight = g_graphicsConfig.surfaceConfiguration.height;
  const auto& video = petari_present::gVideo;
  const Viewport viewport = calculate_present_viewport(surfaceWidth, surfaceHeight,
                                                       video.aspectWidth != 0 ? video.aspectWidth : 4,
                                                       video.aspectHeight != 0 ? video.aspectHeight : 3);
  const AuroraWindowSize window = aurora::window::get_window_size();
  if (viewport.width <= 0.f || viewport.height <= 0.f || window.native_fb_width == 0 || window.native_fb_height == 0) {
    return 0;
  }
  // Surface pixels to window points.
  const float sx = static_cast<float>(window.width) / static_cast<float>(window.native_fb_width);
  const float sy = static_cast<float>(window.height) / static_cast<float>(window.native_fb_height);
  *x = viewport.left * sx;
  *y = viewport.top * sy;
  *width = viewport.width * sx;
  *height = viewport.height * sy;
  return 1;
}

}  // extern "C"

// Aurora side of XFB presentation (README.md in this directory). Compiled
// with Aurora's lib/ on the quote include path; no SDK headers.

#include "present_aurora.hpp"

#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"

#include <cstdio>

#include "present.h"

namespace petari_present {
namespace {

// VI dimming darkens the picture. The Wii's exact level is not documented in
// the SDK; this is an approximation (README.md).
constexpr float kDimOpacity = 0.5f;

bool gEnabled = false;
PetariPresentVideo gVideo{};
const void* gMissingXfb = nullptr;  // last XFB reported without a display copy

// 1x1 black with kDimOpacity alpha (premultiplied), for draw_dim.
wgpu::Texture gDimTexture;
wgpu::TextureView gDimView;

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
}

void petari_present_set_video(const PetariPresentVideo* video) {
  petari_present::gVideo = *video;
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

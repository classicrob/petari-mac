#include <petari/frame_telemetry.hpp>

namespace aurora::gfx {
void petari_submit_segment(detail::FramePacket&, const PetariNative::GX::SnapshotSource&,
                           uint64_t, PetariNative::GX::SnapshotReady);

void petari_capture_current_efb(uint64_t ticket, PetariNative::GX::SnapshotReady ready) {
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "EFB capture requires an active render pass");
  auto& frame = current_frame_packet();
  auto& previous = current_render_passes()[g_recorder.currentRenderPass];
  const auto logical = vi::configured_fb_size();
  const auto size = previous.colorAttachments[SceneColorAttachmentIndex].size;
  PetariNative::GX::SnapshotSource source{
      previous.copySourceView, previous.copySourceDepthView, size, previous.msaaSamples,
      logical.x, logical.y, 0.f, 0.f, 1.f, 1.f, PetariNative::FrameTelemetry::nowNs()};
  AURORA_ASSERT(logical.x && logical.y, "EFB capture requires a configured VI mode");
  if (gx::g_gxState.viewportPolicy != AURORA_VIEWPORT_NATIVE) {
    const bool stretch = gx::g_gxState.viewportPolicy == AURORA_VIEWPORT_STRETCH;
    const float sx = float(size.width) / logical.x, sy = float(size.height) / logical.y;
    source.scaleX = stretch ? sx : std::min(sx, sy);
    source.scaleY = stretch ? sy : std::min(sx, sy);
    source.offsetX = (float(size.width) - logical.x * source.scaleX) * 0.5f;
    source.offsetY = (float(size.height) - logical.y * source.scaleY) * 0.5f;
  }
  enqueue_pass(frame, g_recorder.currentRenderPass);
  petari_submit_segment(frame, source, ticket, ready);
  resume_efb_pass_loading(previous);
}
}

namespace PetariNative::GX {
void captureEfb(uint64_t ticket, SnapshotReady ready) {
  HostAllocationScope host;
  aurora::gfx::petari_capture_current_efb(ticket, ready);
}
}

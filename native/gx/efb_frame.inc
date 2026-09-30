#include <petari/frame_telemetry.hpp>

namespace aurora::gfx {
namespace {
// PETARI_EFB_SYNC_SEGMENT=1: the recorder waits until the render worker has
// submitted each capture segment (the previous behaviour), for A/B measurement.
bool petari_sync_segments() {
  static const bool sync = [] {
    const char* value = std::getenv("PETARI_EFB_SYNC_SEGMENT");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return sync;
}
}

// Called only by the recorder thread, stopped at the capture boundary.
//
// Staging slots: the recorder writes into its own slot (petariRecordingSlots),
// and the render worker's pass encodes copy out of frame.stagingBuffer. This
// segment's work item switches the worker's slot, in queue order: every pass
// enqueued before it (all written to the old slot) is encoded before the old
// slot is unmapped and submitted, and every later pass copies from the new
// slot, which also holds the old slot's contents at the same offsets. So the
// recorder switches its own slot at once and does not wait for the worker.
void petari_submit_segment(detail::FramePacket& frame, const PetariNative::GX::SnapshotSource& source,
                           uint64_t ticket, PetariNative::GX::SnapshotReady ready) {
  const auto nextSlot = [] {
    PetariNative::FrameTelemetry::Scope timing{PetariNative::FrameTelemetry::EfbStagingWait};
    return acquire_mapped_staging_buffer();
  }();
  AURORA_ASSERT(nextSlot.has_value(), "Cannot acquire staging memory for an EFB segment");
  const size_t packetIndex = static_cast<size_t>(&frame - g_framePackets.data());
  AURORA_ASSERT(packetIndex < petariRecordingSlotCount, "EFB segment for an unknown frame packet");
  const auto oldSlot = petariRecordingSlots[packetIndex];
  std::array<ByteBuffer, 5> next;
  ByteBuffer* previous[] = {&frame.verts, &frame.uniforms, &frame.indices, &frame.storage, &frame.textureUpload};
  const uint64_t capacities[] = {VertexBufferSize, UniformBufferSize, IndexBufferSize,
                                 StorageBufferSize, UseTextureBuffer ? TextureUploadSize : 0};
  uint64_t offset = 0;
  for (size_t i = 0; i < next.size(); ++i) {
    if (!capacities[i]) continue;
    next[i] = ByteBuffer{static_cast<uint8_t*>(g_stagingBuffers[*nextSlot].GetMappedRange(offset, capacities[i])),
                         static_cast<size_t>(capacities[i])};
    if (!previous[i]->empty()) next[i].append(previous[i]->data(), previous[i]->size());
    offset += capacities[i];
  }
  const bool sync = petari_sync_segments();
  auto complete = sync ? std::make_shared<std::promise<void>>() : nullptr;
  auto completion = sync ? complete->get_future() : std::future<void>{};
  {
    PetariNative::FrameTelemetry::Scope timing{PetariNative::FrameTelemetry::EfbSubmitWait};
    render_worker::enqueue_work([&frame, source, ticket, ready, oldSlot, newSlot = *nextSlot, complete] {
      auto mapReadback = PetariNative::GX::encodeSnapshot(frame.encoder, source, ticket, ready);
      g_stagingBuffers[oldSlot].Unmap();
      g_mappingStates[oldSlot].store(BufferMapState::Unmapped, std::memory_order_release);
      const auto commands = frame.encoder.Finish();
      g_queue.Submit(1, &commands);
      for (auto& callback : frame.afterSubmitCallbacks) callback();
      frame.afterSubmitCallbacks.clear();
      mapReadback();
      depth_peek::after_submit();
      constexpr wgpu::CommandEncoderDescriptor descriptor{.label = "Petari continued EFB segment"};
      frame.encoder = g_device.CreateCommandEncoder(&descriptor);
      frame.stagingBuffer = newSlot;  // later pass encodes copy from the new slot
      map_staging_buffer(oldSlot, true);
      process_events();
      if (complete) complete->set_value();
    });
    if (sync) completion.get();
  }
  petariRecordingSlots[packetIndex] = *nextSlot;
  for (size_t i = 0; i < next.size(); ++i) *previous[i] = std::move(next[i]);
}
}

namespace aurora::gfx {
// Called only by the recorder thread, stopped at the capture boundary. Queue
// ordering finishes all earlier pass encodes before unmapping their staging slot.
void petari_submit_segment(detail::FramePacket& frame, const PetariNative::GX::SnapshotSource& source,
                           uint64_t ticket, PetariNative::GX::SnapshotReady ready) {
  const auto nextSlot = acquire_mapped_staging_buffer();
  AURORA_ASSERT(nextSlot.has_value(), "Cannot acquire staging memory for an EFB segment");
  const auto oldSlot = frame.stagingBuffer;
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
  auto complete = std::make_shared<std::promise<void>>();
  auto completion = complete->get_future();
  render_worker::enqueue_work([&frame, source, ticket, ready, oldSlot, complete] {
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
    map_staging_buffer(oldSlot, true);
    process_events();
    complete->set_value();
  });
  completion.get();
  frame.stagingBuffer = *nextSlot;
  for (size_t i = 0; i < next.size(); ++i) *previous[i] = std::move(next[i]);
}
}

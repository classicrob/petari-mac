#pragma once

#include "gfx/types.hpp"
#include <cstdint>

namespace PetariNative::GX {
using SnapshotReady = void (*)(std::uint64_t);
struct SnapshotSource {
    wgpu::TextureView color;
    wgpu::TextureView depth;
    wgpu::Extent3D size;
    std::uint32_t samples;
    std::uint32_t width, height;
    float offsetX, offsetY, scaleX, scaleY;
};

// Recorder/FIFO thread only. Closes a pass and submits a segment without presenting.
void captureEfb(std::uint64_t ticket, SnapshotReady ready);
// Render worker only; returned closure starts mapping after the segment is submitted.
aurora::gfx::AfterSubmitCallback encodeSnapshot(const wgpu::CommandEncoder& encoder,
    const SnapshotSource& source, std::uint64_t ticket, SnapshotReady ready);
bool readSnapshot(std::uint64_t ticket, std::uint16_t x, std::uint16_t y,
                  std::uint32_t& depth, std::uint32_t& argb);
void retireSnapshots(std::uint64_t throughTicket);
struct SnapshotStats {
    std::uint32_t differentColorPixels, writtenDepthPixels;
};
bool snapshotStats(std::uint64_t ticket, SnapshotStats& result);
}

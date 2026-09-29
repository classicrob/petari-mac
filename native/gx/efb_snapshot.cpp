#include "efb_snapshot.hpp"
#include "sync_backend.h"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include <petari/host_allocation.hpp>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace PetariNative::GX {
namespace {
struct Pixel { std::uint32_t depth, argb; };
struct Snapshot {
    std::uint32_t width, height;
    std::vector<Pixel> pixels;
};
std::mutex snapshotMutex;
std::map<std::uint64_t, Snapshot> snapshots;

constexpr const char* shader = R"(
struct Params {
    dst: vec2u, src: vec2u,
    offset: vec2f, scale: vec2f,
    reversed: u32, pad0: u32, pad1: u32, pad2: u32,
};
struct Pixel { depth: u32, argb: u32, };
@group(0) @binding(0) var color: texture_2d<f32>;
@group(0) @binding(1) var depth: texture_depth_2d;
@group(0) @binding(2) var<storage, read_write> pixels: array<Pixel>;
@group(0) @binding(3) var<uniform> p: Params;
@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) id: vec3u) {
    if (id.x >= p.dst.x || id.y >= p.dst.y) { return; }
    let coord = clamp(vec2i(floor(p.offset + (vec2f(id.xy) + vec2f(0.5)) * p.scale)),
                      vec2i(0), vec2i(p.src) - vec2i(1));
    let c = vec4u(round(clamp(textureLoad(color, coord, 0), vec4f(0), vec4f(1)) * 255.0));
    let raw = textureLoad(depth, coord, 0);
    let z = select(raw, 1.0 - raw, p.reversed != 0u);
    let i = id.y * p.dst.x + id.x;
    pixels[i].depth = min(u32(clamp(z, 0.0, 1.0) * 16777215.0 + 0.5), 0xffffffu);
    pixels[i].argb = (c.a << 24u) | (c.r << 16u) | (c.g << 8u) | c.b;
}
)";
struct Params {
    std::uint32_t width, height, sourceWidth, sourceHeight;
    float offsetX, offsetY, scaleX, scaleY;
    std::uint32_t reversed, padding[3];
};
static_assert(sizeof(Params) == 48);
[[noreturn]] void fail(const char* message) {
    std::fprintf(stderr, "Native EFB snapshot: %s\n", message);
    std::abort();
}
}

aurora::gfx::AfterSubmitCallback encodeSnapshot(const wgpu::CommandEncoder& encoder,
    const SnapshotSource& source, std::uint64_t ticket, SnapshotReady ready) {
    HostAllocationScope host;
    using aurora::webgpu::g_device;
    if (!source.color || !source.depth || !source.width || !source.height ||
        source.samples != 1 || !ready) fail("invalid capture source (single-sample EFB required)");
    const auto bytes = std::uint64_t(source.width) * source.height * sizeof(Pixel);
    const wgpu::BufferDescriptor storageDesc{.label = "Petari EFB pixels",
        .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc, .size = bytes};
    const auto storage = g_device.CreateBuffer(&storageDesc);
    const wgpu::BufferDescriptor readDesc{.label = "Petari EFB readback",
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, .size = bytes};
    const auto readback = g_device.CreateBuffer(&readDesc);
    const wgpu::BufferDescriptor paramsDesc{.label = "Petari EFB parameters",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(Params)};
    const auto paramsBuffer = g_device.CreateBuffer(&paramsDesc);
    const Params params{source.width, source.height, source.size.width, source.size.height,
        source.offsetX, source.offsetY, source.scaleX, source.scaleY,
        aurora::gx::UseReversedZ ? 1u : 0u, {0, 0, 0}};
    aurora::webgpu::g_queue.WriteBuffer(paramsBuffer, 0, &params, sizeof(params));
    const wgpu::ShaderSourceWGSL wgsl{wgpu::ShaderSourceWGSL::Init{.code = shader}};
    const wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &wgsl, .label = "Petari EFB capture"};
    const auto module = g_device.CreateShaderModule(&moduleDesc);
    const wgpu::ComputePipelineDescriptor pipelineDesc{.label = "Petari EFB capture",
        .compute = wgpu::ComputeState{.module = module, .entryPoint = "main"}};
    const auto pipeline = g_device.CreateComputePipeline(&pipelineDesc);
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .textureView = source.color},
        wgpu::BindGroupEntry{.binding = 1, .textureView = source.depth},
        wgpu::BindGroupEntry{.binding = 2, .buffer = storage, .size = bytes},
        wgpu::BindGroupEntry{.binding = 3, .buffer = paramsBuffer, .size = sizeof(Params)},
    };
    const wgpu::BindGroupDescriptor bindDesc{.label = "Petari EFB capture",
        .layout = pipeline.GetBindGroupLayout(0), .entryCount = entries.size(), .entries = entries.data()};
    const auto binding = g_device.CreateBindGroup(&bindDesc);
    const auto pass = encoder.BeginComputePass();
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, binding);
    pass.DispatchWorkgroups((source.width + 7) / 8, (source.height + 7) / 8);
    pass.End();
    encoder.CopyBufferToBuffer(storage, 0, readback, 0, bytes);
    return [readback, bytes, ticket, ready, width = source.width, height = source.height] {
        readback.MapAsync(wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
            [readback, bytes, ticket, ready, width, height](wgpu::MapAsyncStatus status, wgpu::StringView) {
                HostAllocationScope host;
                if (status != wgpu::MapAsyncStatus::Success) fail("GPU readback mapping failed");
                const auto* data = static_cast<const Pixel*>(readback.GetConstMappedRange(0, bytes));
                if (!data) fail("GPU readback returned no data");
                Snapshot result{width, height, std::vector<Pixel>(data, data + bytes / sizeof(Pixel))};
                readback.Unmap();
                {
                    std::lock_guard lock(snapshotMutex);
                    if (!snapshots.emplace(ticket, std::move(result)).second) fail("duplicate snapshot ticket");
                }
                ready(ticket);
            });
    };
}

bool readSnapshot(std::uint64_t ticket, std::uint16_t x, std::uint16_t y,
                  std::uint32_t& depth, std::uint32_t& argb) {
    std::lock_guard lock(snapshotMutex);
    const auto it = snapshots.find(ticket);
    if (it == snapshots.end() || x >= it->second.width || y >= it->second.height) return false;
    const auto pixel = it->second.pixels[std::size_t(y) * it->second.width + x];
    depth = pixel.depth;
    argb = pixel.argb;
    return true;
}

void retireSnapshots(std::uint64_t throughTicket) {
    HostAllocationScope host;
    std::lock_guard lock(snapshotMutex);
    snapshots.erase(snapshots.begin(), snapshots.upper_bound(throughTicket));
}
bool snapshotStats(std::uint64_t ticket, SnapshotStats& result) {
    std::lock_guard lock(snapshotMutex);
    const auto it = snapshots.find(ticket);
    if (it == snapshots.end() || it->second.pixels.empty()) return false;
    result = {};
    const auto background = it->second.pixels.front().argb;
    for (const auto pixel : it->second.pixels) {
        result.differentColorPixels += pixel.argb != background;
        result.writtenDepthPixels += pixel.depth != 0xffffff;
    }
    return true;
}
}

extern "C" void petari_gx_install_snapshots() {
    petari_gx_sync_set_snapshot_hook([](std::uint64_t ticket, std::uint16_t,
                                       std::uint64_t, void*) {
        PetariNative::GX::retireSnapshots(petari_gx_sync_delivered_ticket());
        PetariNative::GX::captureEfb(ticket, petari_gx_sync_snapshot_ready);
    }, nullptr);
}

extern "C" bool petari_gx_read_snapshot(std::uint64_t ticket, std::uint16_t x,
    std::uint16_t y, std::uint32_t* depth, std::uint32_t* argb) {
    return PetariNative::GX::readSnapshot(ticket, x, y, *depth, *argb);
}

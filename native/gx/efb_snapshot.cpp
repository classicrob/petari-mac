#include "efb_snapshot.hpp"
#include "sync_backend.h"
#include "gx/gx.hpp"
#include "webgpu/gpu.hpp"
#include <petari/efb_dump_mark.hpp>
#include <petari/png_write.hpp>
#include <petari/frame_telemetry.hpp>
#include <petari/host_allocation.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace PetariNative::GX {
namespace {
struct Pixel { std::uint32_t depth, argb; };
struct Snapshot {
    std::uint32_t width, height;
    std::vector<Pixel> pixels;
};
std::mutex snapshotMutex;
std::map<std::uint64_t, Snapshot> snapshots;
// Pixel storage of retired snapshots, reused by later ones (same size in
// practice, so no allocation or page faults per capture). snapshotMutex.
std::vector<std::vector<Pixel>> sparePixels;
constexpr std::size_t kMaxSparePixels = 4;


// --- Opt-in PNG dump (PETARI_EFB_DUMP=<dir>), off and free when unset. -----------------------
// Writes every PETARI_EFB_DUMP_EVERY-th (default 60) snapshot, for up to
// PETARI_EFB_DUMP_SPAN (default 1200) snapshots after each phase mark whose label is listed in
// PETARI_EFB_DUMP_LABELS (default "idle,walk,jump"). About three snapshots are taken per game frame.
// Files: <dir>/<label>-<index since mark>-t<ticket>.png (RGB; alpha ignored). Uncompressed PNG.
struct DumpConfig {
    std::string dir, labels;
    unsigned long every = 60, span = 1200;
    bool enabled = false;
};
const DumpConfig& dumpConfig() {
    static const DumpConfig config = [] {
        DumpConfig c;
        if (const char* dir = std::getenv("PETARI_EFB_DUMP"); dir && *dir) {
            c.dir = dir;
            c.enabled = true;
            c.labels = std::getenv("PETARI_EFB_DUMP_LABELS") ? std::getenv("PETARI_EFB_DUMP_LABELS") : "idle,walk,jump";
            if (const char* v = std::getenv("PETARI_EFB_DUMP_EVERY")) c.every = std::max(1ul, std::strtoul(v, nullptr, 10));
            if (const char* v = std::getenv("PETARI_EFB_DUMP_SPAN")) c.span = std::strtoul(v, nullptr, 10);
        }
        return c;
    }();
    return config;
}
// Called for every completed snapshot (readback thread, host allocation scope).
void maybeDump(std::uint64_t ticket, const Snapshot& snapshot) {
    const auto& config = dumpConfig();
    if (!config.enabled) return;
    static std::uint64_t seenMark = 0, sinceMark = 0;
    static const char* label = "none";
    static bool directoryMade = false;
    const auto counter = EfbDump::markCounter.load(std::memory_order_acquire);
    if (counter != seenMark) {
        seenMark = counter;
        sinceMark = 0;
        label = EfbDump::markLabel.load(std::memory_order_acquire);
    }
    const auto index = sinceMark++;
    if (index >= config.span || index % config.every != 0) return;
    bool listed = false;
    for (size_t at = 0; at <= config.labels.size() && !listed;) {
        const size_t comma = std::min(config.labels.find(',', at), config.labels.size());
        const std::string token = config.labels.substr(at, comma - at);
        listed = !token.empty() && std::string(label).compare(0, token.size(), token) == 0;
        at = comma + 1;
    }
    if (!listed) return;
    if (!directoryMade) {
        std::string command = "mkdir -p '" + config.dir + "'";
        if (std::system(command.c_str()) != 0) return;
        directoryMade = true;
    }
    std::vector<unsigned char> rgb;
    rgb.reserve(snapshot.pixels.size() * 3);
    for (const auto& pixel : snapshot.pixels) {
        rgb.push_back(static_cast<unsigned char>(pixel.argb >> 16));
        rgb.push_back(static_cast<unsigned char>(pixel.argb >> 8));
        rgb.push_back(static_cast<unsigned char>(pixel.argb));
    }
    PetariNative::writePngRgb(config.dir + "/" + label + "-" + std::to_string(index) + "-t" + std::to_string(ticket) + ".png",
                              snapshot.width, snapshot.height, rgb.data());
}

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

// Capture objects that do not depend on the source, made once per device.
// Render worker only (encodeSnapshot). Sharing the parameter and storage
// buffers between captures is safe because each capture's segment is
// submitted before the next capture is encoded (petari_submit_segment), so
// queue order puts every WriteBuffer and dispatch after the previous
// capture's copy out of storage. Never destroyed: the app leaves with _Exit,
// and static destructors would release Dawn objects after Aurora's shutdown.
struct Resources {
    wgpu::Device device;
    wgpu::ComputePipeline pipeline;
    wgpu::BindGroupLayout layout;
    wgpu::Buffer params;
    wgpu::Buffer storage;
    std::uint64_t storageBytes = 0;
    // The last bind group, for the views it binds (references held, so the
    // handles cannot be reused by other views while cached).
    wgpu::BindGroup binding;
    wgpu::TextureView boundColor, boundDepth;
};
Resources& resources() {
    static Resources* instance = new Resources;
    return *instance;
}

// Idle readback buffers, all unmapped and readbackBytes long. A buffer leaves
// the pool for one capture and returns after its mapping was copied out and
// unmapped. Returns from an older generation (other size or device) are dropped.
std::mutex readbackMutex;
std::vector<wgpu::Buffer>& idleReadbacks() {
    static auto* pool = new std::vector<wgpu::Buffer>;  // first used in encodeSnapshot, host scope
    return *pool;
}
std::uint64_t readbackBytes = 0;
std::uint64_t readbackGeneration = 0;
constexpr std::size_t kMaxIdleReadbacks = 4;

wgpu::Buffer takeReadback(std::uint64_t bytes, std::uint64_t& generation) {
    {
        std::lock_guard lock(readbackMutex);
        if (bytes != readbackBytes) {
            idleReadbacks().clear();
            readbackBytes = bytes;
            ++readbackGeneration;
        }
        generation = readbackGeneration;
        if (!idleReadbacks().empty()) {
            wgpu::Buffer buffer = std::move(idleReadbacks().back());
            idleReadbacks().pop_back();
            return buffer;
        }
    }
    const wgpu::BufferDescriptor readDesc{.label = "Petari EFB readback",
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst, .size = bytes};
    return aurora::webgpu::g_device.CreateBuffer(&readDesc);
}

void returnReadback(wgpu::Buffer buffer, std::uint64_t generation) {
    std::lock_guard lock(readbackMutex);
    if (generation == readbackGeneration && idleReadbacks().size() < kMaxIdleReadbacks) {
        idleReadbacks().push_back(std::move(buffer));
    }
}

Resources& prepareResources(std::uint64_t storageBytes) {
    using aurora::webgpu::g_device;
    Resources& r = resources();
    if (r.device.Get() != g_device.Get()) {
        r = {};
        r.device = g_device;
        const wgpu::ShaderSourceWGSL wgsl{wgpu::ShaderSourceWGSL::Init{.code = shader}};
        const wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &wgsl, .label = "Petari EFB capture"};
        const auto module = g_device.CreateShaderModule(&moduleDesc);
        const wgpu::ComputePipelineDescriptor pipelineDesc{.label = "Petari EFB capture",
            .compute = wgpu::ComputeState{.module = module, .entryPoint = "main"}};
        r.pipeline = g_device.CreateComputePipeline(&pipelineDesc);
        r.layout = r.pipeline.GetBindGroupLayout(0);
        const wgpu::BufferDescriptor paramsDesc{.label = "Petari EFB parameters",
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, .size = sizeof(Params)};
        r.params = g_device.CreateBuffer(&paramsDesc);
        std::lock_guard lock(readbackMutex);
        idleReadbacks().clear();
        readbackBytes = 0;
        ++readbackGeneration;
    }
    if (r.storageBytes != storageBytes) {
        const wgpu::BufferDescriptor storageDesc{.label = "Petari EFB pixels",
            .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc, .size = storageBytes};
        r.storage = g_device.CreateBuffer(&storageDesc);
        r.storageBytes = storageBytes;
        r.binding = nullptr;
    }
    return r;
}
}

aurora::gfx::AfterSubmitCallback encodeSnapshot(const wgpu::CommandEncoder& encoder,
    const SnapshotSource& source, std::uint64_t ticket, SnapshotReady ready) {
    HostAllocationScope host;
    using aurora::webgpu::g_device;
    if (!source.color || !source.depth || !source.width || !source.height ||
        source.samples != 1 || !ready) fail("invalid capture source (single-sample EFB required)");
    const auto bytes = std::uint64_t(source.width) * source.height * sizeof(Pixel);
    Resources& r = prepareResources(bytes);
    std::uint64_t generation = 0;
    const auto readback = takeReadback(bytes, generation);
    const Params params{source.width, source.height, source.size.width, source.size.height,
        source.offsetX, source.offsetY, source.scaleX, source.scaleY,
        aurora::gx::UseReversedZ ? 1u : 0u, {0, 0, 0}};
    aurora::webgpu::g_queue.WriteBuffer(r.params, 0, &params, sizeof(params));
    if (!r.binding || r.boundColor.Get() != source.color.Get() || r.boundDepth.Get() != source.depth.Get()) {
        const std::array entries{
            wgpu::BindGroupEntry{.binding = 0, .textureView = source.color},
            wgpu::BindGroupEntry{.binding = 1, .textureView = source.depth},
            wgpu::BindGroupEntry{.binding = 2, .buffer = r.storage, .size = bytes},
            wgpu::BindGroupEntry{.binding = 3, .buffer = r.params, .size = sizeof(Params)},
        };
        const wgpu::BindGroupDescriptor bindDesc{.label = "Petari EFB capture",
            .layout = r.layout, .entryCount = entries.size(), .entries = entries.data()};
        r.binding = g_device.CreateBindGroup(&bindDesc);
        r.boundColor = source.color;
        r.boundDepth = source.depth;
    }
    const auto pass = encoder.BeginComputePass();
    pass.SetPipeline(r.pipeline);
    pass.SetBindGroup(0, r.binding);
    pass.DispatchWorkgroups((source.width + 7) / 8, (source.height + 7) / 8);
    pass.End();
    encoder.CopyBufferToBuffer(r.storage, 0, readback, 0, bytes);
    return [readback, bytes, generation, ticket, ready, width = source.width, height = source.height,
            requested = source.requestedNs] {
        readback.MapAsync(wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
            [readback, bytes, generation, ticket, ready, width, height, requested](wgpu::MapAsyncStatus status,
                                                                                   wgpu::StringView) {
                HostAllocationScope host;
                if (status != wgpu::MapAsyncStatus::Success) fail("GPU readback mapping failed");
                const auto* data = static_cast<const Pixel*>(readback.GetConstMappedRange(0, bytes));
                if (!data) fail("GPU readback returned no data");
                Snapshot result{width, height, {}};
                {
                    std::lock_guard lock(snapshotMutex);
                    if (!sparePixels.empty()) {
                        result.pixels = std::move(sparePixels.back());
                        sparePixels.pop_back();
                    }
                }
                result.pixels.assign(data, data + bytes / sizeof(Pixel));
                maybeDump(ticket, result);
                readback.Unmap();
                returnReadback(readback, generation);
                {
                    std::lock_guard lock(snapshotMutex);
                    if (!snapshots.emplace(ticket, std::move(result)).second) fail("duplicate snapshot ticket");
                }
                if (requested != 0) {
                    FrameTelemetry::add(FrameTelemetry::EfbCaptureLatency, FrameTelemetry::nowNs() - requested);
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
    const auto end = snapshots.upper_bound(throughTicket);
    for (auto it = snapshots.begin(); it != end && sparePixels.size() < kMaxSparePixels; ++it) {
        sparePixels.push_back(std::move(it->second.pixels));
    }
    snapshots.erase(snapshots.begin(), end);
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

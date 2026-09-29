#include <aurora/gfx.h>
#include <gfx/render_worker.hpp>
#include <gx/fifo.hpp>
#include <petari/host_allocation.hpp>
#include <cstdio>
#include "efb_snapshot.hpp"
#include "webgpu/gpu.hpp"
#include "gx/regs.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
std::atomic<std::uint64_t> readyTicket{0};
void markReady(std::uint64_t ticket) { readyTicket.store(ticket, std::memory_order_release); }
bool checkScissorOffset() {
    using aurora::gx::fifo::handle_bp;
    using aurora::gx::g_gxState;
    const auto oldTop = g_gxState.bpRegCache[0x20];
    const auto oldBottom = g_gxState.bpRegCache[0x21];
    const auto oldOffset = g_gxState.bpRegValid[0x59] ? g_gxState.bpRegCache[0x59] : (171 | (171 << 10));
    handle_bp(0xFEFFFFFF);
    handle_bp(0x59000000 | 171 | (171 << 10));
    handle_bp(0x20000000 | 342 | (342 << 12));
    handle_bp(0x21000000 | (342 + 99) | ((342 + 199) << 12));
    const auto origin = g_gxState.logicalScissor;
    handle_bp(0x59000000 | 176 | (181 << 10));
    const auto shifted = g_gxState.logicalScissor;
    handle_bp(0x59000000 | (oldOffset & 0xffffff));
    handle_bp(0x20000000 | (oldTop & 0xffffff));
    handle_bp(0x21000000 | (oldBottom & 0xffffff));
    return origin.x == 0 && origin.y == 0 && origin.width == 200 && origin.height == 100 &&
           shifted.x == -10 && shifted.y == -20 && shifted.width == 200 && shifted.height == 100;
}
}

extern "C" bool petari_probe_check_readback(unsigned frame) {
    PetariNative::HostAllocationScope host;
    aurora::gx::fifo::drain();
    if (frame == 0 && !checkScissorOffset()) {
        std::fprintf(stderr, "Scissor register offset check failed\n");
        return false;
    }
    std::uint32_t firstDepth = 0, firstColor = 0;
    for (unsigned capture = 0; capture < 2; ++capture) {
        const auto ticket = (std::uint64_t(1) << 63) + std::uint64_t(frame) * 2 + capture + 1;
        PetariNative::GX::captureEfb(ticket, markReady);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (readyTicket.load(std::memory_order_acquire) != ticket) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "EFB capture did not complete before end_frame\n");
                return false;
            }
            aurora::gfx::render_worker::enqueue_work([] { aurora::webgpu::g_instance.ProcessEvents(); });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::uint32_t depth = 0, color = 0;
        if (!PetariNative::GX::readSnapshot(ticket, 320, 240, depth, color) || depth > 0xffffff) return false;
        PetariNative::GX::SnapshotStats stats{};
        if (!PetariNative::GX::snapshotStats(ticket, stats)) return false;
        if (frame == 0 && capture == 0) std::fprintf(stderr,
            "EFB rendered pixels: %u differ from background, %u contain depth\n",
            stats.differentColorPixels, stats.writtenDepthPixels);
        if (!stats.differentColorPixels) {
            std::fprintf(stderr, "EFB capture contains no geometry pixels\n");
            return false;
        }
        if (!capture) { firstDepth = depth; firstColor = color; }
        else if (firstDepth != depth || firstColor != color) {
            std::fprintf(stderr, "EFB contents changed across a no-draw segment boundary\n");
            return false;
        }
        PetariNative::GX::retireSnapshots(ticket);
    }
    if (frame == 0) std::fprintf(stderr, "Mid-frame EFB captures: depth %06x, ARGB %08x; preserved across segments\n",
                                  firstDepth, firstColor);
    return true;
}

extern "C" bool petari_probe_check_draws() {
    PetariNative::HostAllocationScope host;
    aurora::gx::fifo::drain();
    aurora::gfx::render_worker::synchronize();
    const auto stats = *aurora_get_stats();
    std::fprintf(stderr, "Last submitted frame: %u draw calls, %u vertex bytes, %u index bytes\n",
                 stats.drawCallCount, stats.lastVertSize, stats.lastIndexSize);
    return stats.drawCallCount > 0 && stats.lastVertSize > 0;
}

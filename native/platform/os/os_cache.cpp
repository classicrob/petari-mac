// Cache operations on a coherent host. Replaces the cache half of
// src/RVL_SDK/os/OSCache.c.
//
// Host memory is coherent between CPU cores and every native "device" (DVD
// drive, ARAM, audio mixer, renderer) is host code, so no cache line ever has
// to be written back or discarded. What remains of these operations is
// ordering:
// - DC*Flush/Store: a producer's writes before the call are ordered before
//   anything after it. DCFlushRange/DCStoreRange ("sync" on the Wii) use a
//   sequentially consistent fence, the NoSync forms a release fence.
// - DCInvalidateRange/DCInvalidate: a consumer's reads after the call are
//   ordered after anything before it (acquire fence). Paired with a flush in
//   the producer and any flag between them, the consumer sees the producer's
//   data, which is the Wii contract for device DMA.
// - DCZeroRange zeroes the 32-byte blocks covering the range, as dcbz does.
// - ICInvalidateRange invalidates the host instruction cache for the range
//   (sys_icache_invalidate). No host code is generated at run time, so
//   ICFlashInvalidate has nothing stale to discard.
// - The locked cache (LC*) is the Wii's 16 KiB scratch with a DMA engine to
//   main memory. Natively its "addresses" are host pointers; LCStoreData
//   copies whole 32-byte blocks synchronously, so the DMA queue is always
//   empty and LCQueueWait returns at once.
// - Store log: the Wii GPU reads textures from memory, so after the CPU
//   rewrites a texture and stores it (DCStoreRange/DCFlushRange, then
//   GXInvalidateTexAll for the texture cache) the next draw sees the new
//   texels. Aurora uploads a texture once per texture object and data
//   version, so every store is logged by 4 KiB page with a generation, and
//   the texture loader asks whether a texture's bytes were stored since it
//   last looked (petari_dc_stored_since). SnowFloor, fur density, normal-map
//   and Mario's dissolve-mask textures depend on this.

#include <revolution/os.h>

#include <petari/host_allocation.hpp>

#include <libkern/OSCacheControl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

constexpr std::uintptr_t kBlock = 32;
std::atomic<bool> gLockedCacheEnabled{false};

constexpr unsigned kStorePageShift = 12;
std::atomic<std::uint64_t> gStoreGeneration{0};
std::mutex gStoreLock;
// Page -> generation of the last store touching it. Bounded by the pages the game
// ever stores (its heaps), a few MiB at most.
std::unordered_map<std::uintptr_t, std::uint64_t>& storePages() {
    static auto* pPages = new std::unordered_map<std::uintptr_t, std::uint64_t>();
    return *pPages;
}

void logStore(const void* addr, std::size_t nBytes) {
    if (addr == nullptr || nBytes == 0) {
        return;
    }
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(addr) >> kStorePageShift;
    const std::uintptr_t last = (reinterpret_cast<std::uintptr_t>(addr) + nBytes - 1) >> kStorePageShift;
    // DC stores come from game threads. The page map must never allocate from the game
    // heap while gStoreLock is held: the heap's mutex and the CPU baton can be held by a
    // thread that is itself waiting for gStoreLock (startup deadlock).
    PetariNative::HostAllocationScope hostAllocations;
    std::lock_guard<std::mutex> guard(gStoreLock);
    const std::uint64_t generation = gStoreGeneration.load(std::memory_order_relaxed) + 1;
    auto& pages = storePages();
    for (std::uintptr_t page = first; page <= last; ++page) {
        pages[page] = generation;
    }
    gStoreGeneration.store(generation, std::memory_order_release);
}

}  // namespace

// Generation of the latest logged store (0 before any).
extern "C" std::uint64_t petari_dc_store_generation(void) {
    return gStoreGeneration.load(std::memory_order_acquire);
}

// Nonzero when a store logged after generation `since` touched [addr, addr + nBytes).
extern "C" int petari_dc_stored_since(const void* addr, std::size_t nBytes, std::uint64_t since) {
    if (addr == nullptr || nBytes == 0 || gStoreGeneration.load(std::memory_order_acquire) <= since) {
        return 0;
    }
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(addr) >> kStorePageShift;
    const std::uintptr_t last = (reinterpret_cast<std::uintptr_t>(addr) + nBytes - 1) >> kStorePageShift;
    PetariNative::HostAllocationScope hostAllocations;  // storePages() may construct the map
    std::lock_guard<std::mutex> guard(gStoreLock);
    const auto& pages = storePages();
    for (std::uintptr_t page = first; page <= last; ++page) {
        const auto it = pages.find(page);
        if (it != pages.end() && it->second > since) {
            return 1;
        }
    }
    return 0;
}

extern "C" {

void DCEnable(void) {}  // host data caches are always on

void DCInvalidateRange(void*, u32) {
    std::atomic_thread_fence(std::memory_order_acquire);
}

void DCInvalidate(void*, u32) {
    std::atomic_thread_fence(std::memory_order_acquire);
}

void DCFlushRange(void* addr, u32 nBytes) {
    logStore(addr, nBytes);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void DCStoreRange(void* addr, u32 nBytes) {
    logStore(addr, nBytes);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void DCFlushRangeNoSync(void* addr, u32 nBytes) {
    logStore(addr, nBytes);
    std::atomic_thread_fence(std::memory_order_release);
}

void DCStoreRangeNoSync(void* addr, u32 nBytes) {
    logStore(addr, nBytes);
    std::atomic_thread_fence(std::memory_order_release);
}

void DCZeroRange(void* addr, u32 nBytes) {
    if (nBytes == 0) {
        return;
    }
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(addr) & ~(kBlock - 1);
    const std::uintptr_t end = (reinterpret_cast<std::uintptr_t>(addr) + nBytes + kBlock - 1) & ~(kBlock - 1);
    std::memset(reinterpret_cast<void*>(start), 0, end - start);
    logStore(reinterpret_cast<void*>(start), end - start);
}

void ICInvalidateRange(void* addr, u32 nBytes) {
    sys_icache_invalidate(addr, nBytes);
}

void ICFlashInvalidate(void) {}  // no run-time generated host code
void ICEnable(void) {}           // host instruction caches are always on

void LCEnable(void) {
    gLockedCacheEnabled.store(true, std::memory_order_release);
}

void LCDisable(void) {
    gLockedCacheEnabled.store(false, std::memory_order_release);
}

// num blocks of 32 bytes; 0 means the maximum of 128, as the DMA length field.
void LCStoreBlocks(void* pDest, void* pSrc, u32 num) {
    if (!gLockedCacheEnabled.load(std::memory_order_acquire)) {
        OSPanic(__FILE__, __LINE__, "LCStoreBlocks(): locked cache is not enabled (LCEnable)");
    }
    const u32 blocks = num == 0 ? LC_MAX_DMA_BLOCKS : num;
    if (blocks > LC_MAX_DMA_BLOCKS) {
        OSPanic(__FILE__, __LINE__, "LCStoreBlocks(): %u blocks exceed one DMA transaction", blocks);
    }
    std::memmove(pDest, pSrc, blocks * kBlock);
    logStore(pDest, blocks * kBlock);
}

u32 LCStoreData(void* pDest, void* pSrc, u32 num) {
    u32 blockCount = (num + 31) / 32;
    const u32 transactions = (blockCount + LC_MAX_DMA_BLOCKS - 1) / LC_MAX_DMA_BLOCKS;
    auto* dest = static_cast<u8*>(pDest);
    auto* src = static_cast<u8*>(pSrc);
    while (blockCount > 0) {
        if (blockCount < LC_MAX_DMA_BLOCKS) {
            LCStoreBlocks(dest, src, blockCount);
            blockCount = 0;
        } else {
            LCStoreBlocks(dest, src, 0);
            blockCount -= LC_MAX_DMA_BLOCKS;
            dest += LC_MAX_DMA_BYTES;
            src += LC_MAX_DMA_BYTES;
        }
    }
    return transactions;
}

void LCQueueWait(u32) {
    // Stores complete synchronously; no DMA is ever outstanding.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

}  // extern "C"

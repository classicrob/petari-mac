#include <revolution/os.h>
#include <revolution/os/OSAlloc.h>
#include <revolution/os/OSBootInfo.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "%s\n", reason); std::exit(1); }
}
int main() {
    auto* low = static_cast<u8*>(OSGetArenaLo());
    void* high = OSGetArenaHi();
    check(reinterpret_cast<uintptr_t>(low) % 32 == 0, "MEM1 alignment");
    check(OSGetMEM1ArenaLo() == low && OSGetMEM1ArenaHi() == high, "MEM1 arena aliases");
    check(OSIsMEM1Region(low) && !OSIsMEM2Region(low), "MEM1 region lookup");
    auto* boot = static_cast<OSBootInfo*>(OSPhysicalToCached(0));
    check(boot->memorySize == 128 * 1024 * 1024 && OSCachedToPhysical(boot) == 0, "Native boot info");
    check(OSPhysicalToCached(OSCachedToPhysical(low)) == low, "MEM1 address round trip");
    void* mem2 = OSGetMEM2ArenaLo();
    check(OSIsMEM2Region(mem2) && !OSIsMEM1Region(mem2), "MEM2 region lookup");
    check(OSGetPhysicalMem2Size() == 256 * 1024 * 1024, "MEM2 capacity");
    check(OSCachedToPhysical(mem2) == 0x10000000 && OSPhysicalToUncached(0x10000000) == mem2, "MEM2 address round trip");
    check(__MEM2End == reinterpret_cast<uintptr_t>(OSGetMEM2ArenaHi()), "Native MEM2 end");
    check(!OSIsMEM1Region(&low) && !OSIsMEM2Region(&low), "Host stack is not game memory");
    auto* first = static_cast<u8*>(OSAllocFromMEM1ArenaLo(33, 32));
    auto* second = static_cast<u8*>(OSAllocFromMEM1ArenaLo(16, 64));
    check(first == low && second >= first + 33 && reinterpret_cast<uintptr_t>(second) % 64 == 0, "Aligned arena allocation");
    std::memset(first, 0x55, 33); std::memset(second, 0xaa, 16);
    OSSetArenaLo(low);
    OSSetArenaHi(low + 3);
    check(!OSAllocFromMEM1ArenaLo(1, 32) && OSGetArenaLo() == low, "Failed allocation preserves arena");
    OSSetArenaHi(high);

    alignas(32) u8 storage[2048];
    void* usable = OSInitAlloc(storage, storage + sizeof(storage), 2);
    check(usable && static_cast<u8*>(usable) >= storage + 48, "Native heap descriptors preserve pointer width");
    const auto heap = OSCreateHeap(usable, static_cast<u8*>(usable) + 1024);
    check(heap >= 0 && OSSetCurrentHeap(heap) == -1, "SDK heap creation");
    void* a = OSAllocFromHeap(heap, 100);
    void* b = OSAllocFromHeap(heap, 200);
    check(a && b && a != b && reinterpret_cast<uintptr_t>(a) % 32 == 0, "SDK heap allocations");
    std::memset(a, 0x12, 100); std::memset(b, 0x34, 200);
    check(!OSAllocFromHeap(heap, 0xffffffffu) && !OSAllocFromHeap(-1, 16), "SDK heap rejects invalid requests");
    OSFreeToHeap(heap, a); OSFreeToHeap(heap, b);
    void* merged = OSAllocFromHeap(heap, 960);
    check(merged == a, "SDK heap coalesces freed blocks");
    OSFreeToHeap(heap, merged);
    check(!OSInitAlloc(storage, storage + 32, 16), "SDK heap descriptor bounds");
    std::puts("Native arenas, address translation, SDK heap alignment and coalescing passed.");
}

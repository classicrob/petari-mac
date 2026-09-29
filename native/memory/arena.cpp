#include <revolution/os.h>
#include <revolution/os/OSBootInfo.h>
#include <petari/host_allocation.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/mman.h>
#include <execinfo.h>
#include <unistd.h>

namespace {
constexpr std::size_t mem1Size = 128 * 1024 * 1024;
constexpr std::size_t mem2Size = 256 * 1024 * 1024;
constexpr std::uintptr_t mem2Physical = 0x10000000;
constexpr std::size_t bootReservation = 0x4000;
struct Arena { std::uintptr_t base, low, high; std::size_t size; };
Arena mem1{}, mem2{};
std::once_flag initialized;
std::mutex arenaMutex;

[[noreturn]] void fail(const char* reason) {
    std::fprintf(stderr, "Native arena: %s\n", reason);
    void* frames[24];
    const int count = backtrace(frames, 24);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    std::abort();
}
Arena allocateArena(std::size_t size, std::size_t reserved) {
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) fail("Could not reserve game memory");
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    return {base, base + reserved, base + size, size};
}
void init() {
    PetariNative::HostAllocationScope hostAllocations;
    std::call_once(initialized, [] {
        mem1 = allocateArena(mem1Size, bootReservation);
        mem2 = allocateArena(mem2Size, 0);
        __MEM2End = mem2.base + mem2.size;
        auto* boot = reinterpret_cast<OSBootInfo*>(mem1.base);
        boot->memorySize = mem1Size;
        boot->arenaLo = reinterpret_cast<void*>(mem1.low);
        boot->arenaHi = reinterpret_cast<void*>(mem1.high);
    });
}
bool contains(const Arena& arena, std::uintptr_t address, bool includeEnd = false) {
    return address >= arena.base && (includeEnd ? address - arena.base <= arena.size : address - arena.base < arena.size);
}
void setLow(Arena& arena, void* pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (!contains(arena, address, true) || address > arena.high) fail("Low arena boundary is outside available memory");
    arena.low = address;
}
void setHigh(Arena& arena, void* pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (!contains(arena, address, true) || address < arena.low) fail("High arena boundary is outside available memory");
    arena.high = address;
}
}

extern "C" {
uintptr_t __MEM2End;
void* OSGetArenaLo() { init(); std::lock_guard<std::mutex> lock(arenaMutex); return reinterpret_cast<void*>(mem1.low); }
void* OSGetArenaHi() { init(); std::lock_guard<std::mutex> lock(arenaMutex); return reinterpret_cast<void*>(mem1.high); }
void OSSetArenaLo(void* p) { init(); std::lock_guard<std::mutex> lock(arenaMutex); setLow(mem1, p); }
void OSSetArenaHi(void* p) { init(); std::lock_guard<std::mutex> lock(arenaMutex); setHigh(mem1, p); }
void* OSGetMEM1ArenaLo() { return OSGetArenaLo(); }
void* OSGetMEM1ArenaHi() { return OSGetArenaHi(); }
void* OSGetMEM2ArenaLo() { init(); std::lock_guard<std::mutex> lock(arenaMutex); return reinterpret_cast<void*>(mem2.low); }
void* OSGetMEM2ArenaHi() { init(); std::lock_guard<std::mutex> lock(arenaMutex); return reinterpret_cast<void*>(mem2.high); }
void OSSetMEM2ArenaHi(void* p) { init(); std::lock_guard<std::mutex> lock(arenaMutex); setHigh(mem2, p); }
u32 OSGetPhysicalMem2Size() { return mem2Size; }

void* OSAllocFromMEM1ArenaLo(u32 size, u32 alignment) {
    init();
    if (!alignment || (alignment & (alignment - 1))) fail("Allocation alignment must be a power of two");
    std::lock_guard<std::mutex> lock(arenaMutex);
    const auto start = (mem1.low + alignment - 1) & ~std::uintptr_t(alignment - 1);
    if (start > mem1.high || size > mem1.high - start) return nullptr;
    const auto end = (start + size + alignment - 1) & ~std::uintptr_t(alignment - 1);
    if (end > mem1.high) return nullptr;
    mem1.low = end;
    return reinterpret_cast<void*>(start);
}

void* PetariNativePhysicalToHost(uintptr_t address) {
    init();
    if (address < mem1.size) return reinterpret_cast<void*>(mem1.base + address);
    if (address >= mem2Physical && address - mem2Physical < mem2.size)
        return reinterpret_cast<void*>(mem2.base + address - mem2Physical);
    fail("Physical address is outside MEM1 and MEM2");
}
uintptr_t PetariNativeHostToPhysical(const void* pointer) {
    init();
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (contains(mem1, address)) return address - mem1.base;
    if (contains(mem2, address)) return address - mem2.base + mem2Physical;
    fail("Physical conversion requires an address inside a game arena");
}
BOOL PetariNativeIsMemoryRegion(const void* pointer, int region) {
    init();
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return region == 1 ? contains(mem1, address) : region == 2 && contains(mem2, address);
}
}

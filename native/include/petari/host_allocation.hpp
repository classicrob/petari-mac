#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace PetariNative {
// Petari's replacement global operator new uses the current JKR heap only on
// registered game threads, and only outside a HostAllocationScope. Every other
// thread (Dawn, SDL, libc++ and other host threads) uses the host allocator.
// Deallocation is routed by address, so memory may be freed on any thread.
// Explicit JKR allocations (new (heap, align) T, JKRHeap::alloc) are unaffected.

// Registers or unregisters the calling thread as a game thread. The native OS
// layer registers the thread that runs OSInit and every OSThread it starts.
void setGameAllocationThread(bool enabled);
bool isGameAllocationThread();

// While a scope is alive, a game thread also uses the host allocator. Wrap host
// library work on game threads (Aurora calls, platform std containers). Scopes nest.
class HostAllocationScope {
public:
    HostAllocationScope();
    ~HostAllocationScope();

    HostAllocationScope(const HostAllocationScope&) = delete;
    HostAllocationScope& operator=(const HostAllocationScope&) = delete;
};

// True when the calling thread must use the host allocator: it is not a game
// thread, a HostAllocationScope is active on it, or it has released the OS CPU
// (setGameCpuReleased). The replacement operator new asks this for every
// game-heap candidate, which also runs the allocation-site check below.
bool isHostAllocationActive();

// ---- Diagnostics (always on; PETARI_ALLOC_SITE_CHECK=0 disables the site check) ----
// Missing scopes are found at run time instead of by heap corruption:
// - Site check: each distinct call chain that allocates from the game heap is
//   symbolized once and classified by its first frame outside the allocator
//   and the C++ library. Renderer/host code there (Aurora, Dawn, SDL, fmt,
//   PetariNative, petari_*/aurora_* entry points, natively implemented SDK
//   functions) is reported as a missing HostAllocationScope; the allocation
//   still uses the game heap. Later allocations from a known chain cost a
//   frame-pointer walk and a table lookup.
// - CPU released: a game thread between petari_os_begin_host_blocking and
//   _end runs host code only, while another game thread may use the JKR
//   heaps; its unscoped allocations use the host allocator and are reported.
void setGameCpuReleased(bool released);  // the OS layer, on the calling thread
void setAllocationSiteCheck(bool enabled);
struct AllocationDiagnostics {
    unsigned long long hostSiteAllocations;  // game-heap allocations from host code
    unsigned long long releasedAllocations;  // unscoped allocations with the CPU released
    unsigned long long classified;           // distinct call chains symbolized
    int sites;                               // distinct reported sites (at most 24 kept)
};
AllocationDiagnostics allocationDiagnostics();
void reportAllocationDiagnostics(std::FILE* out);  // summary and sites, for exit and hang reports

// The classification the site check uses, for one return address (callers
// pass address - 1 semantics already applied: a return address is looked up
// just before it). Symbolizes with dladdr and demangles (allocates host
// memory): never call it while a thread that may hold the dyld or malloc lock
// is suspended.
enum class CodeKind {
    Game,         // game code (decompiled sources, the game's own classes and functions)
    Host,         // renderer/host/platform code (see the site check)
    Library,      // the C++ library or the replacement allocator
    Anonymous,    // an anonymous-namespace function: game or host, undecidable by name
    ThreadStart,  // where a thread's own code begins (OS thread trampoline, main, petari_game_main)
    Unknown,      // no symbol
};
CodeKind classifyCode(std::uintptr_t returnAddress, std::string* name = nullptr);
}  // namespace PetariNative

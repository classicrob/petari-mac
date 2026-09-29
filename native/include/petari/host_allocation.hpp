#pragma once

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
// thread, or a HostAllocationScope is active on it.
bool isHostAllocationActive();
}  // namespace PetariNative

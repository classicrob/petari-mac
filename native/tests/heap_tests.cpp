#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <petari/boot.hpp>
#include <petari/host_allocation.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Heap check failed: %s\n", message);
        std::abort();
    }
}
}

int main() {
    OSInit();
    OSThread* mainThread = OSGetCurrentThread();
    OSInit();
    check(mainThread && mainThread == OSGetCurrentThread(), "idempotent main-thread bootstrap");
    JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
    check(root && root == JKRHeap::sRootHeap, "root heap in native arena");
    check(root->check(), "initial root heap structure");
    check(JKRExpHeap::createRoot(1, false) == nullptr, "second root creation fails safely");
    const s32 originalFree = root->getTotalFreeSize();
    check(JKRExpHeap::create(sizeof(JKRExpHeap), root, false) == nullptr, "undersized child rejected");
    JKRExpHeap* child = JKRExpHeap::create(1024 * 1024, root, false);
    check(child && child->check(), "child heap created");
    child->becomeCurrentHeap();
    struct alignas(64) Aligned { unsigned char bytes[128]; };
    Aligned* aligned = new Aligned;
    check(reinterpret_cast<std::uintptr_t>(aligned) % alignof(Aligned) == 0, "aligned global new");
    check(JKRHeap::findFromRoot(aligned) == child, "new uses current game heap");
    std::memset(aligned, 0xA5, sizeof(*aligned));
    delete aligned;
    void* host;
    {
        PetariNative::HostAllocationScope hostAllocations;
        host = ::operator new(128);
        check(JKRHeap::findFromRoot(host) == nullptr, "host allocation bypasses scene heap");
    }
    constexpr unsigned count = 256;
    void* blocks[count];
    for (unsigned i = 0; i < count; ++i) {
        const unsigned size = i + 1;
        blocks[i] = child->alloc(size, i % 2 ? 4 : -32);
        check(blocks[i] && reinterpret_cast<std::uintptr_t>(blocks[i]) % 8 == 0, "head/tail native alignment");
        std::memset(blocks[i], i & 255, size);
    }
    check(child->check(), "mixed allocations preserve lists");
    for (unsigned i = 0; i < count; ++i) {
        auto* bytes = static_cast<unsigned char*>(blocks[i]);
        for (unsigned j = 0; j <= i; ++j) check(bytes[j] == (i & 255), "allocation contents do not overlap");
        child->free(blocks[i]);
    }
    check(child->check(), "freed blocks coalesce");
    child->freeAll();
    std::memset(host, 0xCC, 128);
    root->becomeCurrentHeap();
    JKRHeap::destroy(child);
    ::operator delete(host);
    check(root->check() && root->getTotalFreeSize() == originalFree, "child destroy returns allocation to root");
    std::puts("Native heap bootstrap and allocation checks passed");
}

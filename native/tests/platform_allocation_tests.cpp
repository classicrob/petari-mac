// Allocation ownership of the native platform under the game's replacement
// operator new/delete (src/JSystem/JKernel/JKRHeap.cpp).
//
// On a registered game thread, plain new allocates from the current JKR heap;
// memory is freed by address, and freeing JKR memory takes the heap's OS mutex,
// which host threads (DVD drive, DSP worker, NAND worker, GP interrupts) must
// never do. So platform state that host threads touch must be host-owned even
// when a game thread creates or mutates it, and game callbacks must still see
// the game allocator.
//
// The test registers the main thread as a game thread with a child JKR heap
// current, touches every platform service first from it, drives the host
// workers that drain and free what the game thread queued, checks that the
// child heap never changed, then destroys the child heap and runs everything
// again. Before the fix, the DSP worker freeing deque blocks allocated on the
// game thread aborted in OSLockMutex (boot 7).

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <revolution/dsp.h>
#include <revolution/dvd.h>
#include <revolution/nand.h>
#include <revolution/os.h>
#include <revolution/sc.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "petari/host_allocation.hpp"
#include "petari/platform/os_host.hpp"
#include "petari/platform/dvd.hpp"
#include "petari/platform/gx_sync.hpp"
#include "petari/platform/nand.hpp"

extern "C" void OSInit();

// JAudio2's DSP host code (dspproc.cpp) references JASDSPInterface's voice
// map. No DSP task boots in this test, so it is never reached.
u16 DSP_CreateMap2(u32) {
    OSPanic(__FILE__, __LINE__, "DSP_CreateMap2: no DSP task runs in the allocation tests");
    return 0;
}

namespace fs = std::filesystem;
namespace PDVD = PetariNative::Platform::DVD;
namespace PNAND = PetariNative::Platform::NAND;
namespace GXS = PetariNative::Platform::GXSync;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

fs::path tempDir(const char* name) {
    const char* base = std::getenv("TMPDIR");
    fs::path path = fs::path(base ? base : "/tmp") / (std::string("petari_alloc_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

// Synthetic RMGE01 disc with one data file.
fs::path makeDisc() {
    const fs::path disc = tempDir("disc");
    fs::create_directories(disc / "files" / "Data");
    fs::create_directories(disc / "sys");
    std::vector<char> data(0x10000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(i * 7);
    }
    std::ofstream(disc / "files" / "Data" / "Stream.bin", std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
    std::vector<char> boot(0x440, 0);
    std::memcpy(boot.data(), "RMGE01", 6);
    std::ofstream(disc / "sys" / "boot.bin", std::ios::binary).write(boot.data(), static_cast<std::streamsize>(boot.size()));
    return disc;
}

bool inHeap(JKRHeap* heap, const void* p) {
    return p >= heap->getStartAddr() && p < heap->getEndAddr();
}

template <class Done>
bool waitFor(Done done, int ms = 5000) {
    for (int i = 0; i < ms && !done(); ++i) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }
    return done();
}

// ---- Host workers fed from the game thread ----

void floodDsp() {
    // Before a task boots, the DSP worker consumes mails into its halted state:
    // the game thread grows the mail deque, the worker drains and frees it.
    for (int i = 0; i < 20000; ++i) {
        DSPSendMailToDSP(0x80001234u);
    }
}

std::atomic<int> gNandDone{0};
std::atomic<s32> gNandResult{0};
std::atomic<bool> gNandCallbackGameAllocator{false};
void nandCallback(s32 result, NANDCommandBlock*) {
    gNandResult = result;
    gNandDone++;
}

void nandRoundTrip(const char* label) {
    // A path (39 characters) longer than std::string's inline buffer.
    static const char kPath[] = "/title/00010000/524d4745/data/alloc.bin";
    static NANDCommandBlock block;
    const int before = gNandDone.load();
    check(NANDPrivateCreateAsync(kPath, 0x3C, 0, nandCallback, &block) == NAND_RESULT_OK, label);
    const bool created = waitFor([&] { return gNandDone.load() == before + 1; });
    if (!created || gNandResult.load() != NAND_RESULT_OK) {
        std::fprintf(stderr, "NAND create: done=%d result=%d\n", created ? 1 : 0, static_cast<int>(gNandResult.load()));
    }
    check(created && gNandResult.load() == NAND_RESULT_OK, "async NAND create completes");
    check(NANDPrivateDeleteAsync(kPath, nandCallback, &block) == NAND_RESULT_OK, "async NAND delete");
    check(waitFor([&] { return gNandDone.load() == before + 2; }) && gNandResult.load() == NAND_RESULT_OK, "async NAND delete completes");
}

std::atomic<int> gDvdDone{0};
void dvdCallback(s32 result, DVDFileInfo*) {
    if (result == 0x8000) {
        gDvdDone++;
    }
}

void dvdRead() {
    static DVDFileInfo info;
    alignas(32) static u8 buffer[0x8000];
    check(DVDOpen("/Data/Stream.bin", &info), "DVDOpen");
    const int before = gDvdDone.load();
    check(DVDReadAsyncPrio(&info, buffer, sizeof(buffer), 0x8000, dvdCallback, 2), "DVDReadAsyncPrio");
    check(waitFor([&] { return gDvdDone.load() == before + 1; }), "async DVD read completes on the drive thread");
    check(buffer[1] == static_cast<u8>((0x8000 + 1) * 7), "read data");
    DVDClose(&info);
}

std::atomic<int> gTokens{0};
void tokenCallback(u16) {
    gTokens++;
}

void gpTokens() {
    // GP interrupts: the processor (a host thread) queues, the GP interrupt
    // thread delivers.
    const int before = gTokens.load();
    std::thread processor([] {
        static std::uint64_t position = 0;
        for (int i = 0; i < 2000; ++i) {
            position += 8;
            GXS::reportToken(static_cast<std::uint16_t>(i), position);
        }
    });
    processor.join();
    check(waitFor([&] { return gTokens.load() == before + 2000; }), "GP token interrupts delivered");
}

void exerciseHostWorkers(const char* label) {
    floodDsp();
    nandRoundTrip(label);
    dvdRead();
    gpTokens();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the DSP worker drain
}

// ---- Game callbacks keep the game allocator ----

std::atomic<bool> gEntryUsesGameHeap{false};
JKRHeap* gExpectedHeap = nullptr;
void* threadEntry(void*) {
    int* p = new int(7);
    gEntryUsesGameHeap = !PetariNative::isHostAllocationActive() && inHeap(gExpectedHeap, p);
    delete p;
    return nullptr;
}

}  // namespace

// ---- Allocation-site detector (host_allocation.hpp) ----
// Allocation sites as the renderer and the game have them. noinline and the
// volatile store keep each a real frame (no inlining, no tail call).
void* volatile gSink;

namespace PetariNative::AllocationDetectorTest {
__attribute__((noinline)) void hostHelperWithoutScope() {
    gSink = new std::uint32_t[16];  // what petari_gx_pipeline_stage_wait did
}
}  // namespace PetariNative::AllocationDetectorTest

namespace PetariNative::AllocationDetectorTest {
__attribute__((noinline)) void hostBigAllocation() {
    gSink = new std::uint8_t[1024 * 1024];
}
}  // namespace PetariNative::AllocationDetectorTest

extern "C" __attribute__((noinline)) void petari_test_entry_without_scope(void) {
    gSink = new std::vector<std::uint32_t>(64);
}

__attribute__((noinline)) void gameStyleAllocation() {
    gSink = new std::uint32_t[16];
}

// os_cache.cpp's logStore shape (the startup deadlock): a helper in a
// top-level anonymous namespace, reached through a natively implemented SDK C
// function whose name is on no list, building host bookkeeping.
namespace {
__attribute__((noinline)) void bookkeepingHelper() {
    auto* pages = new std::unordered_map<std::uintptr_t, std::uint64_t>;
    (*pages)[0x1000] = 1;
    gSink = pages;
}
}  // namespace
extern "C" __attribute__((noinline)) void ZZTestStoreRange(void) {
    bookkeepingHelper();
    asm volatile("" ::: "memory");
}

JKRHeap* gNativeCallerHeap = nullptr;
std::atomic<int> gNativeCallerResult{-1};  // 1: routed to the host, 0: landed in the game heap
void* NativeCallerEntry(void*) {
    PetariNative::AllocationDetectorTest::hostHelperWithoutScope();
    gNativeCallerResult = inHeap(gNativeCallerHeap, gSink) ? 0 : 1;
    delete[] static_cast<std::uint32_t*>(gSink);
    return nullptr;
}

void testAllocationSiteDetector(JKRHeap* heap) {
    const auto before = PetariNative::allocationDiagnostics();
    gameStyleAllocation();
    check(inHeap(heap, gSink), "game code allocates from the current heap");
    delete[] static_cast<std::uint32_t*>(gSink);
    check(PetariNative::allocationDiagnostics().hostSiteAllocations == before.hostSiteAllocations,
          "game code is not reported");

    const s32 heapFree = heap->getTotalFreeSize();
    for (int i = 0; i < 3; ++i) {
        PetariNative::AllocationDetectorTest::hostHelperWithoutScope();
        check(!inHeap(heap, gSink), "an unscoped host site on a game thread is routed to the host allocator");
        delete[] static_cast<std::uint32_t*>(gSink);
    }
    const auto afterHelper = PetariNative::allocationDiagnostics();
    check(afterHelper.hostSiteAllocations == before.hostSiteAllocations + 3, "each routed allocation is counted");
    check(afterHelper.sites == before.sites + 1, "as one site");

    petari_test_entry_without_scope();
    check(!inHeap(heap, gSink), "an unscoped petari_* C entry point's std containers are routed");
    delete static_cast<std::vector<std::uint32_t>*>(gSink);
    check(PetariNative::allocationDiagnostics().hostSiteAllocations > afterHelper.hostSiteAllocations, "and counted");

    const auto beforeCEntry = PetariNative::allocationDiagnostics();
    ZZTestStoreRange();
    check(!inHeap(heap, gSink), "an anonymous-namespace helper behind an unlisted C entry point is routed (os_cache logStore)");
    delete static_cast<std::unordered_map<std::uintptr_t, std::uint64_t>*>(gSink);
    check(PetariNative::allocationDiagnostics().hostSiteAllocations > beforeCEntry.hostSiteAllocations, "and counted");
    check(heap->getTotalFreeSize() == heapFree, "none of the host sites touched the game heap");

    // Another game (OS) thread, no scope anywhere: the native call is routed too.
    gNativeCallerHeap = heap;
    static OSThread caller;
    alignas(32) static u8 callerStack[0x4000];
    OSCreateThread(&caller, NativeCallerEntry, nullptr, callerStack + sizeof(callerStack), sizeof(callerStack), 16, 0);
    OSResumeThread(&caller);
    OSJoinThread(&caller, nullptr);
    check(gNativeCallerResult.load() == 1, "a game-registered OS thread calling a native function allocates on the host");

    {
        PetariNative::HostAllocationScope scope;
        const auto scoped = PetariNative::allocationDiagnostics();
        PetariNative::AllocationDetectorTest::hostHelperWithoutScope();
        check(!inHeap(heap, gSink), "inside a scope the helper uses the host allocator");
        delete[] static_cast<std::uint32_t*>(gSink);
        check(PetariNative::allocationDiagnostics().hostSiteAllocations == scoped.hostSiteAllocations, "and is not reported");
    }

    // With the CPU released, another game thread may be using the heaps: the
    // allocation must not touch them.
    const auto beforeReleased = PetariNative::allocationDiagnostics();
    petari_os_begin_host_blocking();
    gameStyleAllocation();
    void* const released = gSink;
    petari_os_end_host_blocking();
    check(!inHeap(heap, released), "an unscoped allocation with the CPU released uses the host allocator");
    delete[] static_cast<std::uint32_t*>(released);
    check(PetariNative::allocationDiagnostics().releasedAllocations == beforeReleased.releasedAllocations + 1,
          "and is reported");

    // petari_gx_revalidate_texobj's logo abort: the current heap is nearly
    // full and a native function allocates more than it holds.
    JKRExpHeap* tiny = JKRExpHeap::create(64 * 1024, heap, false);
    JKRHeap* previous = JKRHeap::getCurrentHeap();
    tiny->becomeCurrentHeap();
    const s32 tinyFree = tiny->getTotalFreeSize();
    PetariNative::AllocationDetectorTest::hostBigAllocation();
    check(gSink != nullptr && !inHeap(tiny, gSink) && !inHeap(heap, gSink), "a native 1 MiB allocation with a 64 KiB heap current succeeds on the host");
    delete[] static_cast<std::uint8_t*>(gSink);
    check(tiny->getTotalFreeSize() == tinyFree, "and leaves the current heap untouched");
    previous->becomeCurrentHeap();
    JKRHeap::destroy(tiny);

    // Hot path: a known game chain (table hit) against the check turned off.
    constexpr int kAllocations = 200000;
    auto time = [&] {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kAllocations; ++i) {
            gameStyleAllocation();
            delete[] static_cast<std::uint32_t*>(gSink);
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kAllocations;
    };
    time();  // classify once
    double on = 1e9, off = 1e9;
    for (int round = 0; round < 3; ++round) {
        on = std::min(on, time());
        PetariNative::setAllocationSiteCheck(false);
        off = std::min(off, time());
        PetariNative::setAllocationSiteCheck(true);
    }
    std::printf("allocation site routing: %.1f ns per game-heap new+delete with it, %.1f ns without (%+.1f ns)\n", on, off,
                on - off);
    check(on - off < 250.0, "the site check costs well under a microsecond per allocation");
}

int main() {
    const fs::path disc = makeDisc();
    const fs::path nandRoot = tempDir("nand");

    OSInit();
    PetariNative::setGameAllocationThread(true);
    JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
    root->becomeCurrentHeap();
    root->becomeSystemHeap();
    JKRExpHeap* child = JKRExpHeap::create(8 * 1024 * 1024, root, false);
    child->becomeCurrentHeap();

    int* probe = new int(1);
    check(inHeap(child, probe), "a game thread's plain new allocates from the current JKR heap");
    delete probe;
    const s32 childFree = child->getTotalFreeSize();
    {
        // Detector check: an unscoped container on this thread is visible in
        // the child heap's free size (what the old DSP mail deque did).
        std::vector<std::uint32_t> unscoped(4096);
        check(child->getTotalFreeSize() < childFree, "an unscoped std container on a game thread uses the game heap");
    }
    check(child->getTotalFreeSize() == childFree, "and returns it when freed on the same thread");
    testAllocationSiteDetector(child);
    check(child->getTotalFreeSize() == childFree, "the detector's own bookkeeping never uses the game heap");

    // First use of every platform service, from the game thread.
    std::string error;
    check(PDVD::mount({disc}, &error), "mount synthetic disc");
    DVDInit();
    check(PNAND::mount(nandRoot, &error), "mount NAND root");
    check(NANDInit() == NAND_RESULT_OK, "NANDInit");
    SCInit();
    DSPInit();
    GXS::setDrawSyncCallback(tokenCallback);
    check(child->getTotalFreeSize() == childFree, "platform services first used on a game thread allocate nothing from its heap");

    exerciseHostWorkers("async NAND create (game heap current)");
    check(child->getTotalFreeSize() == childFree, "queued requests, mails, and events are host-owned");

    // Game callbacks: an OS thread entry runs with the game allocator.
    gExpectedHeap = child;
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, threadEntry, nullptr, stack + sizeof(stack), sizeof(stack), 16, 0);
    OSResumeThread(&thread);
    OSJoinThread(&thread, nullptr);
    check(gEntryUsesGameHeap.load(), "an OS thread entry allocates from the game heap, outside any host scope");

    // The game destroys the heap that was current while the platform started;
    // its memory is reused. Nothing the platform owns may live there.
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(child->getStartAddr());
    const std::size_t size = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(child->getEndAddr()) - start);
    root->becomeCurrentHeap();
    JKRHeap::destroy(child);
    void* reused = root->alloc(static_cast<u32>(size), 32);
    if (reused != nullptr) {
        std::memset(reused, 0xAA, size);
    }
    exerciseHostWorkers("async NAND create (after the heap was destroyed)");
    if (reused != nullptr) {
        root->free(reused);
    }

    GXS::shutdown();
    PNAND::shutdown();
    PDVD::shutdown();
    fs::remove_all(disc);
    fs::remove_all(nandRoot);
    OSReport("platform allocation tests passed (%d checks)\n", checks);
    std::fflush(nullptr);
    std::_Exit(0);  // DSP threads keep running, as in the game
}

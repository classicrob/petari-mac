// Tests for native ARAM (aralt) using the real native MEM2 arena and the
// game's own reservation and allocation arithmetic.

#include <revolution/aralt.h>
#include <revolution/os.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "petari/platform/aram.hpp"

extern "C" {
void __OSThreadInit(void);
void* OSGetMEM2ArenaLo(void);
void* OSGetMEM2ArenaHi(void);
void OSSetMEM2ArenaHi(void*);
}

namespace PARAM = PetariNative::Platform::ARAM;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

bool aborts(const std::function<void()>& fn) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        std::freopen("/dev/null", "w", stdout);
        fn();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

uintptr_t gArenaLo;
uintptr_t gArenaHiOriginal;

// HeapMemoryWatcher::createRootHeap (native): MEM2 start becomes ARAM.
void reserveAram(u32 bytes) {
    OSSetMEM2ArenaHi(reinterpret_cast<void*>(gArenaHiOriginal));
    OSSetMEM2ArenaHi(reinterpret_cast<void*>(gArenaLo + bytes));
    PARAM::reset();
}

struct JKRAramSizes {
    u32 reserved, total, audioSize, graphSize, aramSize, audioPtr, graphPtr, aramPtr;
};

// JKRAram::JKRAram(audioSize, graphSize = 0xFFFFFFFF), as in Overwrite.cpp.
JKRAramSizes constructJKRAram(u32 audioSize, u32 graphSize) {
    static u32 stack[3];
    JKRAramSizes s{};
    s.reserved = ARInit(stack, 3);
    ARQInit();
    s.total = ARGetSize();
    s.audioSize = audioSize;
    if (graphSize == 0xFFFFFFFF) {
        s.graphSize = s.total - audioSize - s.reserved;
        s.aramSize = 0;
    } else {
        s.graphSize = graphSize;
        s.aramSize = s.total - (audioSize + graphSize) - s.reserved;
    }
    s.audioPtr = ARAlloc(s.audioSize);
    s.graphPtr = ARAlloc(s.graphSize);
    s.aramPtr = s.aramSize != 0 ? ARAlloc(s.aramSize) : 0;
    return s;
}

void testGameLayout() {
    reserveAram(0x4000 + 0xE00000);
    check(ARGetSize() == 0 && !PARAM::isInitialized(), "no ARAM before ARInit");
    const JKRAramSizes s = constructJKRAram(0xE00000, 0xFFFFFFFF);
    check(s.reserved == 0x4000 && ARGetBaseAddress() == 0x4000, "ARInit reserves the first 0x4000 bytes");
    check(s.total == 0xE04000, "ARAM is the reserved MEM2 range");
    check(s.graphSize == 0 && s.aramSize == 0, "graph memory size is exactly zero (no wrap)");
    check(s.audioPtr == 0x4000 && s.graphPtr == 0xE04000 && PARAM::allocated() == 0xE04000, "audio ARAM at 0x4000, graph at the end");
    check(PARAM::base() == gArenaLo, "ARAM base is MEM2 arena low, the address given to setAltAramStartAdr");
    // JASKernel::setupAramHeap(ARGetBaseAddress(), 0xE00000): every audio offset is backed.
    check(PARAM::translate(0x4000, 0xE00000) == reinterpret_cast<void*>(gArenaLo + 0x4000), "audio heap range translates");
    check(PARAM::translate(0x4000, 0xE00001) == nullptr, "one byte past ARAM does not translate");
    check(ARInit(nullptr, 0) == 0x4000 && ARGetSize() == 0xE04000, "ARInit is idempotent");
}

void testWiiReservationFailsLoudly() {
    // The Wii reserved only 0xE00000 bytes; the graph size then wraps to
    // 0xFFFFC000, which natively is an ARAlloc failure rather than a bogus heap.
    check(aborts([] {
              reserveAram(0xE00000);
              constructJKRAram(0xE00000, 0xFFFFFFFF);
          }),
          "Wii-sized reservation: graph-size wrap aborts in ARAlloc");
}

void testTransfers() {
    reserveAram(0x4000 + 0xE00000);
    constructJKRAram(0xE00000, 0xFFFFFFFF);

    // Wave data upload as JKRDvdAramRipper/JASAramStream do (direction 0, main -> ARAM).
    std::vector<u8> wave(0x10000);
    for (std::size_t i = 0; i < wave.size(); ++i) {
        wave[i] = static_cast<u8>(i * 31 + 7);
    }
    const u32 offset = 0x4000 + 0x123460;
    ARStartDMA(0, reinterpret_cast<uintptr_t>(wave.data()), offset, static_cast<u32>(wave.size()));
    check(std::memcmp(reinterpret_cast<void*>(gArenaLo + offset), wave.data(), wave.size()) == 0, "main -> ARAM lands at base + offset");

    // And back (direction 1, ARAM -> main), with JKRAMCommand's src/dst order.
    std::vector<u8> back(wave.size(), 0);
    ARStartDMA(1, offset, reinterpret_cast<uintptr_t>(back.data()), static_cast<u32>(back.size()));
    check(back == wave, "ARAM -> main round trip");

    // Unaligned addresses and lengths are fine (RVL ARAM is memcpy into MEM2).
    std::vector<u8> odd(37, 0);
    ARStartDMA(1, offset + 3, reinterpret_cast<uintptr_t>(odd.data() + 1), 35);
    check(std::memcmp(odd.data() + 1, wave.data() + 3, 35) == 0 && odd[0] == 0 && odd[36] == 0, "unaligned partial transfer");

    // Last bytes of ARAM, the top of the audio heap.
    const u8 tail[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    ARStartDMA(0, reinterpret_cast<uintptr_t>(tail), 0xE04000 - 16, 16);
    u8 tailBack[16] = {};
    ARStartDMA(1, 0xE04000 - 16, reinterpret_cast<uintptr_t>(tailBack), 16);
    check(std::memcmp(tail, tailBack, 16) == 0, "transfer at the very end of ARAM");
    ARStartDMA(0, reinterpret_cast<uintptr_t>(tail), 0xE04000, 0);
    check(true, "zero-length transfer at the end is allowed");

    // Large transfer (the audio heap size itself).
    std::vector<u8> big(0xE00000);
    for (std::size_t i = 0; i < big.size(); i += 4096) {
        big[i] = static_cast<u8>(i >> 12);
    }
    ARStartDMA(0, reinterpret_cast<uintptr_t>(big.data()), 0x4000, 0xE00000);
    std::vector<u8> bigBack(0xE00000);
    ARStartDMA(1, 0x4000, reinterpret_cast<uintptr_t>(bigBack.data()), 0xE00000);
    check(big == bigBack, "full audio-heap round trip");
}

void testInvalidTransfers() {
    static u8 buffer[64];
    const uintptr_t host = reinterpret_cast<uintptr_t>(buffer);
    auto initialized = [] {
        reserveAram(0x4000 + 0xE00000);
        constructJKRAram(0xE00000, 0xFFFFFFFF);
    };
    check(aborts([&] {
              reserveAram(0x4000 + 0xE00000);
              ARStartDMA(0, host, 0x4000, 32);
          }),
          "ARStartDMA before ARInit aborts");
    check(aborts([&] {
              initialized();
              ARStartDMA(0, host, 0xE04000 - 16, 32);
          }),
          "main -> ARAM past the end aborts");
    check(aborts([&] {
              initialized();
              ARStartDMA(1, 0xE04000 - 16, host, 32);
          }),
          "ARAM -> main past the end aborts");
    check(aborts([&] {
              initialized();
              ARStartDMA(0, host, 0xFFFFFFF0ull + 0x100, 32);
          }),
          "ARAM offsets beyond 32 bits abort");
    check(aborts([&] {
              initialized();
              ARStartDMA(0, 0, 0x4000, 32);
          }),
          "null main-memory source aborts");
    check(aborts([&] {
              initialized();
              ARStartDMA(1, 0x4000, 0, 32);
          }),
          "null main-memory destination aborts");
    check(aborts([&] {
              initialized();
              ARStartDMA(2, host, 0x4000, 32);
          }),
          "unknown direction aborts");
    check(aborts([&] {
              initialized();
              ARAlloc(1);
          }),
          "allocating past the end of ARAM aborts");
}

}  // namespace

int main() {
    __OSThreadInit();
    gArenaLo = reinterpret_cast<uintptr_t>(OSGetMEM2ArenaLo());
    gArenaHiOriginal = reinterpret_cast<uintptr_t>(OSGetMEM2ArenaHi());
    check(gArenaHiOriginal - gArenaLo >= 0x4000 + 0xE00000, "MEM2 arena is large enough for ARAM");

    testGameLayout();
    testWiiReservationFailsLoudly();
    testTransfers();
    testInvalidTransfers();
    OSReport("platform ARAM tests passed (%d checks)\n", checks);
    return 0;
}

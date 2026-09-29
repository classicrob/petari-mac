// End-to-end test of the native DSP device with JAudio2's own DSP host code
// (dsptask.cpp, dspproc.cpp, osdsp.cpp, osdsp_task.cpp): microcode boot and
// handshake, table setup and VARAM commands with their acknowledgements,
// frame rendering driven by per-group release mails, per-subframe DSP
// interrupts, and the frame-end/continue exchange.

#include <revolution/aralt.h>
#include <revolution/dsp.h>
#include <revolution/os.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "JSystem/JAudio2/dspproc.hpp"
#include "JSystem/JAudio2/dsptask.hpp"
#include "JSystem/JAudio2/osdsp_task.hpp"
#include "petari/platform/aram.hpp"
#include "petari/platform/audio.hpp"
#include "petari/platform/dsp.hpp"

extern "C" {
void __OSThreadInit(void);
void* OSGetMEM2ArenaLo(void);
void OSSetMEM2ArenaHi(void*);
}
int Dsp_Running_Check();

namespace PARAM = PetariNative::Platform::ARAM;
namespace PDSP = PetariNative::Platform::DSP;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

constexpr int kVoices = 64;
constexpr int kSubframes = 7;
constexpr int kFrameSamples = kSubframes * 0x50;

alignas(32) std::uint8_t gVoices[kVoices][0x180];
alignas(32) std::uint8_t gReverb[4][0x28];
alignas(32) std::uint32_t gResFilter[320];
alignas(32) std::uint8_t gAdpcmFilter[64];
alignas(32) std::int16_t gOut[2][kFrameSamples];

OSMessageQueue gSubframeQueue;
OSMessage gSubframeSlots[16];

void set16(std::uint8_t* v, std::size_t off, int x) {
    const std::uint16_t w = static_cast<std::uint16_t>(x);
    std::memcpy(v + off, &w, 2);
}
void set32(std::uint8_t* v, std::size_t off, std::uint32_t x) {
    std::memcpy(v + off, &x, 4);
}
int get16(const std::uint8_t* v, std::size_t off) {
    std::int16_t w;
    std::memcpy(&w, v + off, 2);
    return w;
}

// JASAudioThread::DSPCallback: subframe-done mails go to the audio thread,
// command acknowledgements to DspFinishWork.
void dspRequest(void*) {
    while (DSPCheckMailFromDSP() == 0) {
    }
    const u32 mail = DSPReadMailFromDSP();
    check((mail >> 16) == 0xF355, "DSP request mails carry the F355 prefix");
    if ((mail & 0xFF00) == 0xFF00) {
        OSSendMessage(&gSubframeQueue, reinterpret_cast<OSMessage>(static_cast<uintptr_t>(mail & 0xFF)), OS_MESSAGE_NOBLOCK);
    } else {
        DspFinishWork(static_cast<u16>(mail));
    }
}

void startVoice(int index, std::uint32_t aramOffset, std::uint32_t length, std::uint16_t bus) {
    std::uint8_t* v = gVoices[index];
    std::memset(v, 0, 0x180);
    set16(v, 0x00, 1);       // active
    set16(v, 0x04, 0x1000);  // pitch 1.0
    set16(v, 0x08, 1);       // reset (playStart)
    set16(v, 0x10, bus);
    set16(v, 0x12, 0x7FFF);
    set16(v, 0x14, 0x7FFF);
    set16(v, 0x100, 16);  // PCM16
    set32(v, 0x114, length);
    set32(v, 0x118, aramOffset);
    set16(v, 0x148, 0x7FFF);
}

}  // namespace

// JASDSPInterface.cpp's DSP_CreateMap2, over this test's voice blocks.
u16 DSP_CreateMap2(u32 group) {
    u16 map = 0;
    for (int i = 0; i < 16; ++i) {
        map = static_cast<u16>(map << 1);
        if (get16(gVoices[group * 16 + i], 0) != 0) {
            map |= 1;
        }
    }
    return map;
}

int main() {
    __OSThreadInit();
    OSInitMessageQueue(&gSubframeQueue, gSubframeSlots, 16);

    // ARAM, as HeapMemoryWatcher and JKRAram set it up.
    const auto lo = reinterpret_cast<uintptr_t>(OSGetMEM2ArenaLo());
    OSSetMEM2ArenaHi(reinterpret_cast<void*>(lo + 0x4000 + 0xE00000));
    static u32 arStack[3];
    ARInit(arStack, 3);
    const u32 waveOffset = ARAlloc(0x10000);
    auto* wave = static_cast<std::uint8_t*>(PARAM::translate(waveOffset, 0x10000));
    for (int i = 0; i < 4000; ++i) {
        const int left = static_cast<int>(std::lround(std::sin(i * 0.05) * 12000));
        wave[i * 2] = static_cast<std::uint8_t>(left >> 8);  // big-endian PCM16
        wave[i * 2 + 1] = static_cast<std::uint8_t>(left);
        const int right = 5000;
        wave[0x8000 + i * 2] = static_cast<std::uint8_t>(right >> 8);
        wave[0x8000 + i * 2 + 1] = static_cast<std::uint8_t>(right);
    }

    // Resampling phases: linear interpolation between taps 1 and 2 (packed as
    // DSPRES_FILTER is: big-endian halves of u32 words); sine law after 0x200.
    for (int p = 0; p < 64; ++p) {
        const std::uint32_t t1 = static_cast<std::uint16_t>(0x8000 * (64 - p) / 64 - (p == 0 ? 1 : 0));
        const std::uint32_t t2 = static_cast<std::uint16_t>(0x8000 * p / 64);
        gResFilter[p * 2] = t1;  // taps 0, 1
        gResFilter[p * 2 + 1] = t2 << 16;
    }

    DSPInit();
    DspBoot(dspRequest);
    // The handshake arrives through the DSP interrupt thread; wait up to 10 s.
    const OSTime deadline = OSGetTime() + OSSecondsToTicks(10);
    while (!Dsp_Running_Check() && OSGetTime() < deadline) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }
    check(Dsp_Running_Check(), "microcode boots and completes the handshake");

    DsetupTable(kVoices, reinterpret_cast<uintptr_t>(gVoices), reinterpret_cast<uintptr_t>(gResFilter), reinterpret_cast<uintptr_t>(gAdpcmFilter),
                reinterpret_cast<uintptr_t>(gReverb));
    check(true, "table setup command acknowledged (DsetupTable returned)");
    DsetVARAM(PARAM::base());
    check(true, "VARAM command acknowledged (DsetVARAM returned)");

    startVoice(0, waveOffset, 4000, 0x0D00);           // front left
    startVoice(17, waveOffset + 0x8000, 4000, 0x0D60);  // front right, group 1
    DsetMixerLevel(0.5f);  // DSP_MIXERLEVEL 0x800: output volume 0.5 in 4.12
    for (int frame = 0; frame < 2; ++frame) {
        DsyncFrame2(kSubframes, reinterpret_cast<uintptr_t>(gOut[0]), reinterpret_cast<uintptr_t>(gOut[1]));
        for (int sub = 0; sub < kSubframes; ++sub) {
            // JASDSPChannel::updateAll: release each group of 16 voices in order.
            for (u32 group = 0; group < 4; ++group) {
                DSPReleaseHalt2(group);
            }
            OSMessage msg;
            OSReceiveMessage(&gSubframeQueue, &msg, OS_MESSAGE_BLOCK);
            check(reinterpret_cast<uintptr_t>(msg) == static_cast<uintptr_t>(sub), "one DSP interrupt per rendered subframe, in order");
            check(get16(gVoices[0], 0x08) == 0, "voice reset flag consumed by the DSP");
        }
        // Output volume 0x800 (0.5 in 4.12) with half-scale channel volume: samples / 4.
        bool leftOk = true, rightOk = true;
        for (int n = 8; n < kFrameSamples; ++n) {
            const int input = frame * kFrameSamples + n - 3;
            const int expectedLeft = static_cast<int>(std::lround(std::sin(input * 0.05) * 12000)) / 4;
            leftOk = leftOk && std::abs(gOut[0][n] - expectedLeft) <= 4;
            rightOk = rightOk && std::abs(gOut[1][n] - 5000 / 4) <= 3;
        }
        check(leftOk, "voice 0 renders its ARAM samples into the left output buffer");
        check(rightOk, "voice 17 (group 1) renders into the right output buffer");
    }
    check(get16(gVoices[0], 0x02) == 0, "voices still playing after two frames");
    {
        // Timing diagnostics were off for those frames: nothing recorded.
        const auto off = PetariNative::Platform::Audio::takeTimingStats();
        check(off.frameUs == 0 && off.subframeRenderUs == 0 && off.dspDeliverUs == 0, "DSP timing off: nothing recorded");
    }
    PetariNative::Platform::Audio::setTimingDiagnostics(true);

    // Let voice 0 run out: 4000 samples at one per output sample.
    for (int frame = 2; frame < 8; ++frame) {
        DsyncFrame2(kSubframes, reinterpret_cast<uintptr_t>(gOut[0]), reinterpret_cast<uintptr_t>(gOut[1]));
        for (int sub = 0; sub < kSubframes; ++sub) {
            for (u32 group = 0; group < 4; ++group) {
                DSPReleaseHalt2(group);
            }
            OSMessage msg;
            OSReceiveMessage(&gSubframeQueue, &msg, OS_MESSAGE_BLOCK);
        }
    }
    check(get16(gVoices[0], 0x02) == 1 && gOut[0][kFrameSamples - 1] == 0, "a finished voice sets its done flag and falls silent");
    {
        // Six frames with timing on: each frame's time spans its subframes'
        // renders (and the round trips between them).
        const auto on = PetariNative::Platform::Audio::takeTimingStats();
        std::printf("DSP timing: frame %lld us, subframe render %lld us, DSP interrupt delivery %lld us\n", static_cast<long long>(on.frameUs),
                    static_cast<long long>(on.subframeRenderUs), static_cast<long long>(on.dspDeliverUs));
        check(on.frameUs > 0 && on.frameUs >= on.subframeRenderUs, "DSP frame time recorded, at least the slowest subframe render");
        PetariNative::Platform::Audio::setTimingDiagnostics(false);
    }
    check(PDSP::microcodeHash(nullptr, 0) == 0, "hash of nothing is zero");

    OSReport("platform audio DSP integration tests passed (%d checks)\n", checks);
    PDSP::shutdown();
    return 0;
}

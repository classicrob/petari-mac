// Tests for the native audio DSP renderer and AI device. Synthetic tables and
// sample data only; expected values are computed by hand from the formats.

#include <revolution/ai.h>
#include <revolution/os.h>
#include <revolution/wenc.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "dsp_renderer.hpp"
#include "petari/platform/audio.hpp"

extern "C" void __OSThreadInit(void);

namespace DSP = PetariNative::Platform::DSP;
namespace PAudio = PetariNative::Platform::Audio;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

// Voice block helpers (TChannel byte offsets).
struct VoiceBlock {
    alignas(32) std::uint8_t bytes[DSP::kVoiceBlockSize] = {};
    void set16(std::size_t off, int v) {
        const std::uint16_t w = static_cast<std::uint16_t>(v);
        std::memcpy(bytes + off, &w, 2);
    }
    void set32(std::size_t off, std::uint32_t v) { std::memcpy(bytes + off, &v, 4); }
    int s16(std::size_t off) const {
        std::int16_t w;
        std::memcpy(&w, bytes + off, 2);
        return w;
    }
    std::uint32_t u32(std::size_t off) const {
        std::uint32_t v;
        std::memcpy(&v, bytes + off, 4);
        return v;
    }
    // A voice as JASDsp::TChannel::playStart/setWaveInfo leave it.
    void start(int sourceType, std::uint32_t base, std::uint32_t length, bool loop = false, std::uint32_t loopStart = 0) {
        std::memset(bytes, 0, sizeof(bytes));
        set16(0x00, 1);       // enabled
        set16(0x04, 0x1000);  // ratio 1.0
        set16(0x08, 1);       // reset
        set16(0x100, sourceType);
        set16(0x102, loop ? 1 : 0);
        set32(0x110, loopStart);
        set32(0x114, length);
        set32(0x118, base);
        set16(0x148, 0x7FFF);  // identity biquad, as initFilter
    }
    void channel(int index, int id, int target, int current) {
        set16(0x10 + index * 8, id);
        set16(0x12 + index * 8, target);
        set16(0x14 + index * 8, current);
    }
    bool done() const { return s16(0x02) != 0; }
};

DSP::Tables testTables() {
    DSP::Tables t;
    // Linear interpolation between taps 1 and 2: phase p weights (1 - p/64, p/64).
    for (int p = 0; p < 64; ++p) {
        t.resampling[p * 4 + 1] = static_cast<std::int16_t>(0x8000 * (64 - p) / 64 - (p == 0 ? 1 : 0));
        t.resampling[p * 4 + 2] = static_cast<std::int16_t>(0x8000 * p / 64);
    }
    for (int i = 0; i < 0x80; ++i) {
        t.sine[i] = static_cast<std::int16_t>(std::lround(std::sin(i * M_PI / 2 / 127.0) * 0x7FFF));
    }
    t.afc[2] = 0x0800;  // coefficient set 1: y = delta + yn1
    return t;
}

// Renders subframes of one voice into interleaved-free L/R vectors.
struct Rig {
    DSP::Renderer renderer;
    std::vector<std::uint8_t> aram = std::vector<std::uint8_t>(0x10000, 0);
    std::vector<std::int16_t> left, right;

    Rig() {
        renderer.setTables(testTables());
        renderer.setAram(aram.data(), static_cast<std::uint32_t>(aram.size()));
    }
    void render(VoiceBlock& voice, int subframes, std::uint16_t outputVolume = 0x2000) {
        for (int i = 0; i < subframes; ++i) {
            std::int16_t l[DSP::kSubframeSamples], r[DSP::kSubframeSamples];
            renderer.prepareFrame();
            renderer.addVoice(voice.bytes);
            renderer.finalizeFrame(l, r, outputVolume);
            left.insert(left.end(), l, l + DSP::kSubframeSamples);
            right.insert(right.end(), r, r + DSP::kSubframeSamples);
        }
    }
    void putBE16(std::size_t offset, int value) {
        aram[offset] = static_cast<std::uint8_t>(value >> 8);
        aram[offset + 1] = static_cast<std::uint8_t>(value);
    }
};

// Full-volume channel (0x7FFF) scales by 0x7FFF >> 16 (about 1/2); output
// volume 0x2000 (2.0 in 4.12) restores unity, less one LSB of rounding.
bool near(int a, int b, int tolerance = 2) {
    return std::abs(a - b) <= tolerance;
}

void testPcm16Playback() {
    Rig rig;
    for (int i = 0; i < 200; ++i) {
        rig.putBE16(0x100 + i * 2, i * 100 - 10000);
    }
    VoiceBlock v;
    v.start(16, 0x100, 200);
    v.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    rig.render(v, 4);
    // Linear-interpolation taps at phase 0 select src[pos + 1]; src[0..3] is
    // history, so output sample n is input sample n - 3 (three zero history samples lead).
    bool ok = true;
    for (int n = 3; n < 200; ++n) {
        ok = ok && near(rig.left[n], (n - 3) * 100 - 10000);
    }
    check(ok, "PCM16 big-endian samples play at unity pitch");
    check(rig.right[50] == 0, "channel 0 routes to front left only");
    check(v.done() && rig.left[250] == 0, "non-looping voice finishes and falls silent");

    Rig rig8;
    for (int i = 0; i < 100; ++i) {
        rig8.aram[i] = static_cast<std::uint8_t>(static_cast<std::int8_t>(i - 50));
    }
    VoiceBlock v8;
    v8.start(8, 0, 100);
    v8.channel(0, 0x0D60, 0x7FFF, 0x7FFF);
    rig8.render(v8, 2);
    check(near(rig8.right[3 + 10], (10 - 50) * 256, 3) && rig8.left[13] == 0, "PCM8 samples scale to 16 bits on front right");
}

void testPitchAndLoop() {
    Rig rig;
    for (int i = 0; i < 400; ++i) {
        rig.putBE16(i * 2, i * 50);
    }
    VoiceBlock v;
    v.start(16, 0, 400);
    v.set16(0x04, 0x2000);  // 2.0: two raw samples per output sample
    v.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    rig.render(v, 1);
    // Output n reads raw sample 2n - 3 (after the 4 history samples).
    check(near(rig.left[40], (2 * 40 - 3) * 50, 4) && near(rig.left[60], (2 * 60 - 3) * 50, 4), "pitch 2.0 advances two raw samples per output");
    check(v.u32(0x74) == 400 - 0xA0, "remaining length drops by the raw samples consumed");

    Rig loop;
    for (int i = 0; i < 100; ++i) {
        loop.putBE16(i * 2, 1000 + i);
    }
    VoiceBlock lv;
    lv.start(16, 0, 100, true, 50);  // loops back to sample 50 after sample 99
    lv.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    loop.render(lv, 3);
    // Output n = input n - 3; input 100 wraps to 50.
    // Tolerance 4: each fixed-point stage truncates (half-scale channel volume makes values even).
    check(near(loop.left[3 + 99], 1099, 4) && near(loop.left[3 + 100], 1050, 4) && near(loop.left[3 + 150], 1050, 4),
          "PCM loop wraps to the loop start");
    check(!lv.done(), "looping voice keeps playing");
}

void testAfc() {
    Rig rig;
    // Block 1: scale 4 (x16), coefficient set 0 (no prediction): deltas 1,2,...
    rig.aram[0] = 0x40;
    const std::uint8_t nibbles[8] = {0x12, 0x34, 0x56, 0x7F, 0xE1, 0x00, 0x00, 0x00};
    std::memcpy(&rig.aram[1], nibbles, 8);
    // Block 2: scale 4 (x16), coefficient set 1 (y = delta + yn1): accumulates.
    rig.aram[9] = 0x41;
    std::memset(&rig.aram[10], 0x11, 8);
    VoiceBlock v;
    v.start(9, 0, 32);
    v.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    rig.render(v, 1);
    const int expected1[10] = {16, 32, 48, 64, 80, 96, 112, -16, -32, 16};
    bool ok = true;
    for (int i = 0; i < 10; ++i) {
        ok = ok && near(rig.left[3 + i], expected1[i]);
    }
    check(ok, "4-bit AFC: scale and signed nibbles");
    // Second block continues from yn1 = last sample of block 1 (0): +16 per sample.
    check(near(rig.left[3 + 16], 16, 3) && near(rig.left[3 + 20], 80, 3) && near(rig.left[3 + 31], 256, 3), "AFC prediction uses the previous samples");
    check(v.done() && rig.left[3 + 40] == 0, "AFC voice ends after its length");

    Rig lq;
    lq.aram[0] = 0x80;  // scale 8 (x256), set 0
    // 2-bit values 1, -1, -2, 0 repeated: bits 01 11 10 00 = 0x78
    std::memset(&lq.aram[1], 0x78, 4);
    VoiceBlock w;
    w.start(5, 0, 16);
    w.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    lq.render(w, 1);
    // 2-bit delta d: (d << 14) >> 1 = d * 0x2000; * 256 >> 11 = d * 1024.
    check(near(lq.left[3], 1024, 3) && near(lq.left[4], -1024, 3) && near(lq.left[5], -2048, 3) && near(lq.left[6], 0, 3),
          "2-bit AFC deltas");
}

void testVolumeRampAndFade() {
    Rig rig;
    for (int i = 0; i < 1000; ++i) {
        rig.putBE16(i * 2, 16000);
    }
    VoiceBlock v;
    v.start(16, 0, 1000);
    v.channel(0, 0x0D00, 0x7FFF, 0);  // ramp up from silence
    rig.render(v, 1);
    check(rig.left[10] < rig.left[40] && rig.left[40] < rig.left[79], "volume ramps within the subframe");
    check(v.s16(0x14) > 0x7F00, "current volume reaches the target");
    rig.render(v, 1);
    check(near(rig.left[120], 16000, 3), "steady at target volume");

    v.set16(0x10A, 1);  // end requested (JASDsp::TChannel::forceStop)
    int subframes = 0;
    while (!v.done() && subframes < 40) {
        rig.render(v, 1);
        ++subframes;
    }
    check(v.done() && subframes > 3 && subframes < 40, "forced stop halves the volume each subframe, then finishes");

    VoiceBlock paused;
    paused.start(16, 0, 1000);
    paused.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    paused.set16(0x0C, 1);  // pause flag: constant sample
    paused.set16(0x66, 1234);
    Rig p;
    p.aram = rig.aram;
    p.renderer.setAram(p.aram.data(), static_cast<std::uint32_t>(p.aram.size()));
    p.render(paused, 1);
    check(near(p.left[5], 1234) && near(p.left[70], 1234), "pause outputs the held sample");
}

void testDolbyPanning() {
    auto pan = [](int x) {
        Rig rig;
        for (int i = 0; i < 500; ++i) {
            rig.putBE16(i * 2, 20000);
        }
        VoiceBlock v;
        v.start(16, 0, 500);
        v.set16(0x50, x << 8);  // X pan, Y = 0 (front)
        v.set16(0x54, 0x7FFF);  // dolby current volume
        v.set16(0x56, 0x7FFF);  // dolby target
        v.set16(0x58, 1);       // use auto mixer
        rig.render(v, 2);
        return std::make_pair(rig.left[120], rig.right[120]);
    };
    const auto left = pan(0), center = pan(63), right = pan(127);
    check(left.first > 10000 && std::abs(left.second) < 200, "pan 0 is left");
    check(right.second > 10000 && std::abs(right.first) < 200, "pan 127 is right");
    check(std::abs(center.first - center.second) < 400 && center.first > 6000, "center pan is balanced");
}

void testReverbSend() {
    Rig rig;
    alignas(32) static std::int16_t circular[2 * DSP::kSubframeSamples];
    std::memset(circular, 0, sizeof(circular));
    circular[5] = 8000;  // echo waiting in slot 0
    DSP::ReverbBlock blocks[4] = {};
    blocks[0].enabled = 4;  // enabled, no filtering
    blocks[0].circularBlocks = 2;
    blocks[0].buffer = circular;
    blocks[0].dest0Id = 0x0D00;
    blocks[0].dest0Volume = 0x7FFF;
    rig.renderer.setReverbBlocks(blocks);
    VoiceBlock silent;
    silent.start(16, 0, 0);
    rig.render(silent, 1, 0x1000);
    // The reverb path keeps 8 samples of history for its 8-tap filter, so a
    // slot's content is sent (and written back) 8 samples later.
    check(near(rig.left[5 + 8], 8000, 2) && rig.left[5] == 0, "reverb buffer content is sent to its destination, 8 samples delayed");
    check(circular[5 + 8] == 8000 && circular[5] == 0 && circular[DSP::kSubframeSamples + 5] == 0, "slot rewritten from its bus after rendering");
}

void testAramFaults() {
    Rig rig;
    VoiceBlock v;
    v.start(16, 0xFFF0, 0x100);  // runs past the end of ARAM
    v.channel(0, 0x0D00, 0x7FFF, 0x7FFF);
    rig.render(v, 1);
    check(v.done() && rig.renderer.aramFaults() > 0, "reading outside ARAM stops the voice and is counted");
}

// ---- AI ----

std::atomic<int> gDmaInterrupts{0};
void dmaCallback() {
    check(OSDisableInterrupts() == FALSE, "AI DMA callback runs with interrupts disabled");
    gDmaInterrupts++;
}

void testAi() {
    AIInit(nullptr);
    check(AIRegisterDMACallback(dmaCallback) == nullptr, "register DMA callback");
    AISetDSPSampleRate(0);
    check(PAudio::outputRate() == 32000 && AIGetDSPSampleRate() == 0, "32 kHz output");

    constexpr std::uint32_t kFrames = 560;  // JAudio2 frame: 7 subframes of 0x50
    static std::int16_t blockA[kFrames * 2], blockB[kFrames * 2];
    for (std::uint32_t i = 0; i < kFrames * 2; ++i) {
        blockA[i] = static_cast<std::int16_t>(i);
        blockB[i] = static_cast<std::int16_t>(-static_cast<int>(i));
    }
    static int sinkStarts = 0;
    static std::uint32_t sinkRate = 0;
    PAudio::setSink({[](std::uint32_t rate, void*) { sinkStarts++; sinkRate = rate; }, nullptr, nullptr});

    std::vector<std::int16_t> out(kFrames * 2 * 3);
    check(PAudio::pull(out.data(), 100) == 100 && out[0] == 0 && out[199] == 0, "silence before DMA starts");

    AIInitDMA(reinterpret_cast<uintptr_t>(blockA), sizeof(blockA));
    check(AIGetDMAStartAddr() == reinterpret_cast<uintptr_t>(blockA) && AIGetDMALength() == sizeof(blockA), "DMA registers");
    AIStartDMA();
    check(sinkStarts == 1 && sinkRate == 32000, "sink starts at the output rate");

    PAudio::pull(out.data(), 100);
    PAudio::drainInterrupts();
    // DMA words are R, L; pull() returns L, R: frame 0 is (R=0, L=1).
    check(gDmaInterrupts == 1 && out[0] == 1 && out[1] == 0 && out[198] == 199 && out[199] == 198,
          "block start raises one DMA interrupt and plays the block, R,L DMA swapped to L,R");
    AIInitDMA(reinterpret_cast<uintptr_t>(blockB), sizeof(blockB));  // what the callback does
    PAudio::pull(out.data(), kFrames);  // 460 more of A, then 100 of B
    PAudio::drainInterrupts();
    check(gDmaInterrupts == 2 && out[459 * 2] == static_cast<std::int16_t>(kFrames * 2 - 1) && out[460 * 2 + 1] == 0 && out[461 * 2 + 1] == -2,
          "next block is latched at the block boundary");
    PAudio::pull(out.data(), kFrames * 2);  // B repeats while no new block is registered
    PAudio::drainInterrupts();
    check(gDmaInterrupts == 4 && PAudio::pendingInterrupts() == 0, "a block repeats and interrupts once per start");
    check(PAudio::replayedBlocks() == 2, "block starts without a new AIInitDMA are counted (B played 3 times)");

    AIStopDMA();
    PAudio::pull(out.data(), 50);
    check(out[0] == 0 && out[99] == 0, "stopped DMA outputs silence");
    PAudio::shutdown();
    check(PAudio::pendingInterrupts() == 0 && PAudio::outputRate() == 32000, "shutdown resets AI");
}

// Channel order regression: JAudio2's DAC buffer is built exactly as
// JASDriver::readDspBuffer does (imixcopy(right half, left half)), with a
// left-only tone and a quieter, different right signal. The host must hear
// the tone on the left.
void testChannelOrder() {
    AIInit(nullptr);
    AIRegisterDMACallback(nullptr);
    constexpr std::uint32_t kFrames = 560;
    static std::int16_t dspOut[kFrames * 2];  // DSP output: left half, then right half
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        dspOut[i] = static_cast<std::int16_t>(((i / 20) % 2) ? 12000 : -12000);  // left: loud square wave
        dspOut[kFrames + i] = static_cast<std::int16_t>(i % 7);                 // right: small ramp
    }
    static std::int16_t dac[kFrames * 2];
    const std::int16_t* right = dspOut + kFrames;
    const std::int16_t* left = dspOut;
    for (std::uint32_t i = 0; i < kFrames; ++i) {  // JASCalc::imixcopy(right, left, dac, n)
        dac[i * 2] = right[i];
        dac[i * 2 + 1] = left[i];
    }
    AIInitDMA(reinterpret_cast<uintptr_t>(dac), sizeof(dac));
    AIStartDMA();
    std::vector<std::int16_t> out(kFrames * 2);
    PAudio::pull(out.data(), kFrames);
    PAudio::drainInterrupts();
    bool leftOk = true, rightOk = true;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        leftOk = leftOk && out[i * 2] == left[i];
        rightOk = rightOk && out[i * 2 + 1] == right[i];
    }
    check(leftOk && rightOk, "the DSP's left buffer plays on the host's left channel");
    AIStopDMA();
    PAudio::shutdown();
}

// SpkSpeakerCtrl::updateSpeaker encodes 40 samples per Wii Remote speaker
// update into a stack buffer and sends 20 bytes. The SDK encoder writes
// (samples + 1) / 2 bytes, so the buffer must hold 20: the decompiled 16-byte
// array overran it (app17, __stack_chk_fail in the speaker alarm).
void testSpeakerEncodeSize() {
    constexpr int kSamples = 40;  // SpkSpeakerCtrl.cpp
    s16 pcm[kSamples];
    for (int i = 0; i < kSamples; ++i) {
        pcm[i] = static_cast<s16>((i % 2) ? 20000 : -20000);
    }
    constexpr u8 kCanary = 0xA5;
    u8 guarded[4 + 20 + 16];
    std::memset(guarded, kCanary, sizeof(guarded));
    u8* data = guarded + 4;
    WENCInfo info;
    std::memset(&info, 0, sizeof(info));
    WENCGetEncodeData(&info, 0, pcm, kSamples, data);
    bool before = true, after = true;
    for (int i = 0; i < 4; ++i) {
        before = before && guarded[i] == kCanary;
    }
    for (int i = 20; i < 36; ++i) {
        after = after && data[i] == kCanary;
    }
    int written = 0;
    for (int i = 0; i < 20; ++i) {
        written += data[i] != kCanary;
    }
    check(before && after, "WENCGetEncodeData writes only its (40 + 1) / 2 bytes");
    check(written > 16, "40 speaker samples encode to 20 bytes, more than the Wii's 16-byte array holds");
}

// Opt-in audio-cycle timing: DMA interrupts -> AIInitDMA of the next block
// (mapping documented in native/platform/audio/audio_timing.hpp).
void testTimingDiagnostics() {
    AIInit(nullptr);
    AIRegisterDMACallback(nullptr);
    static std::int16_t blockA[64 * 2], blockB[64 * 2];  // 2 ms blocks at 32 kHz
    std::vector<std::int16_t> out(64 * 2);
    const auto interrupt = [&] {  // one block start, delivered
        PAudio::pull(out.data(), 64);
        PAudio::drainInterrupts();
    };

    // Disabled: nothing is recorded.
    PAudio::setTimingDiagnostics(false);
    PAudio::takeTimingStats();
    AIInitDMA(reinterpret_cast<uintptr_t>(blockA), sizeof(blockA));
    AIStartDMA();
    interrupt();
    AIInitDMA(reinterpret_cast<uintptr_t>(blockB), sizeof(blockB));
    const PAudio::TimingStats off = PAudio::takeTimingStats();
    check(off.latestRaiseToRegisterUs == 0 && off.dmaDeliverUs == 0 && off.registrationsMissingBlocks == 0 && off.generationsPerRegistration == 0,
          "timing off: nothing recorded");

    PAudio::setTimingDiagnostics(true);
    // Prompt: one interrupt, answered at once.
    interrupt();
    AIInitDMA(reinterpret_cast<uintptr_t>(blockA), sizeof(blockA));
    const PAudio::TimingStats quick = PAudio::takeTimingStats();
    check(quick.generationsPerRegistration == 1 && quick.registrationsMissingBlocks == 0, "a prompt registration answers one interrupt");
    check(quick.latestRaiseToRegisterUs < 2000 && quick.oldestRaiseToRegisterUs == quick.latestRaiseToRegisterUs,
          "prompt: latest and oldest are the same interrupt");

    // Late across blocks: two interrupts go by (the first block was
    // replayed), then the registration comes 20 ms after the second.
    interrupt();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    interrupt();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    AIInitDMA(reinterpret_cast<uintptr_t>(blockB), sizeof(blockB));
    AIInitDMA(reinterpret_cast<uintptr_t>(blockA), sizeof(blockA));  // a second call answers nothing new: not measured
    const PAudio::TimingStats late = PAudio::takeTimingStats();
    check(late.generationsPerRegistration == 2 && late.registrationsMissingBlocks == 1,
          "a registration after two interrupts answers two generations (one block missed), counted once");
    check(late.latestRaiseToRegisterUs >= 20000 && late.latestDeliverToRegisterUs >= 20000, "measured from the latest interrupt");
    check(late.oldestRaiseToRegisterUs >= late.latestRaiseToRegisterUs + 4000,
          "and from the oldest unanswered one, which is earlier (the latest-only delta would hide the missed block)");
    check(late.dspInterruptsBeforeRegister == 0, "no DSP interrupts in between (no DSP running)");

    // Rollover: more unanswered interrupts than the record ring holds.
    for (int i = 0; i < 70; ++i) {
        interrupt();
    }
    AIInitDMA(reinterpret_cast<uintptr_t>(blockB), sizeof(blockB));
    const PAudio::TimingStats wrapped = PAudio::takeTimingStats();
    check(wrapped.generationsPerRegistration == 70 && wrapped.registrationsMissingBlocks == 1,
          "70 unanswered interrupts: the count is exact past the 64-record ring");
    check(wrapped.oldestRaiseToRegisterUs >= wrapped.latestRaiseToRegisterUs, "the oldest is clamped to the ring, never later than the latest");
    check(wrapped.registrationsTruncated == 1 && late.registrationsTruncated == 0 && quick.registrationsTruncated == 0,
          "a registration beyond the ring is marked truncated (its oldest is retained-only); in-ring ones are not");

    PAudio::setTimingDiagnostics(false);
    AIStopDMA();
    PAudio::shutdown();
}

// The block register (AIInitDMA vs the pull latch) under contention: a torn
// snapshot would pair one block's samples with another's length. Each block
// has its own fill value and length, so every run of a value heard must be a
// whole number of that block's length (a replay repeats the whole block).
// Bounded, no timing assertions.
void testRegisterContention() {
    AIInit(nullptr);
    AIRegisterDMACallback(nullptr);
    constexpr int kBlocks = 4;
    static const std::uint32_t kFrames[kBlocks] = {48, 80, 112, 144};
    static std::int16_t blocks[kBlocks][144 * 2];
    for (int b = 0; b < kBlocks; ++b) {
        for (std::uint32_t i = 0; i < kFrames[b] * 2; ++i) {
            blocks[b][i] = static_cast<std::int16_t>(b + 1);
        }
    }
    AIInitDMA(reinterpret_cast<uintptr_t>(blocks[0]), kFrames[0] * 4);
    AIStartDMA();
    std::atomic<bool> stop{false};
    std::thread registrar([&] {
        for (int i = 0; !stop.load(); ++i) {
            const int b = i % kBlocks;
            AIInitDMA(reinterpret_cast<uintptr_t>(blocks[b]), kFrames[b] * 4);
        }
    });
    std::vector<std::int16_t> heard;
    std::vector<std::int16_t> out(97 * 2);
    for (int n = 0; n < 2000; ++n) {
        PAudio::pull(out.data(), 97);
        for (std::size_t i = 0; i < out.size(); i += 2) {
            heard.push_back(out[i]);
        }
    }
    stop = true;
    registrar.join();
    AIStopDMA();
    PAudio::shutdown();
    // Runs of equal values; the first and last may be partial.
    bool whole = true;
    std::size_t runs = 0;
    std::size_t start = 0;
    for (std::size_t i = 1; i <= heard.size(); ++i) {
        if (i == heard.size() || heard[i] != heard[start]) {
            const std::int16_t v = heard[start];
            const std::size_t length = i - start;
            if (start != 0 && i != heard.size() && v >= 1 && v <= kBlocks) {
                whole = whole && length % kFrames[v - 1] == 0;
                ++runs;
            }
            start = i;
        }
    }
    check(runs > 100, "many block latches under contention");
    check(whole, "every latched block plays whole with its own length: no torn register snapshot");
}

std::atomic<int> gSecondSession{0};
void secondSessionCallback() {
    gSecondSession++;
}
std::atomic<int> gFirstSessionLate{0};
void firstSessionCallback() {
    gFirstSessionLate++;
}

// Shutdown with interrupts still in flight, then reinitialise: the new
// session must see only its own block starts (no stale deliveries).
void testAiReinit() {
    static std::int16_t block[64 * 2];
    std::vector<std::int16_t> out(64 * 2 * 40);
    for (int session = 0; session < 20; ++session) {
        AIInit(nullptr);
        AIRegisterDMACallback(firstSessionCallback);
        AIInitDMA(reinterpret_cast<uintptr_t>(block), sizeof(block));
        AIStartDMA();
        PAudio::pull(out.data(), 64 * 40);  // 40 block starts, not drained
        PAudio::shutdown();
    }
    gFirstSessionLate = 0;
    AIInit(nullptr);
    AIRegisterDMACallback(secondSessionCallback);
    AISetDSPSampleRate(1);
    check(PAudio::outputRate() == 48000, "48 kHz after reinit");
    AIInitDMA(reinterpret_cast<uintptr_t>(block), sizeof(block));
    AIStartDMA();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let any stale wake-up surface
    check(gSecondSession == 0 && gFirstSessionLate == 0, "no stale interrupts reach the new session");
    PAudio::pull(out.data(), 64 * 3);
    PAudio::drainInterrupts();
    check(gSecondSession == 3 && gFirstSessionLate == 0, "new session gets exactly its own block starts");
    PAudio::shutdown();
}

}  // namespace

int main() {
    __OSThreadInit();
    testPcm16Playback();
    testPitchAndLoop();
    testAfc();
    testVolumeRampAndFade();
    testDolbyPanning();
    testReverbSend();
    testAramFaults();
    testAi();
    testAiReinit();
    testChannelOrder();
    testSpeakerEncodeSize();
    testTimingDiagnostics();
    testRegisterContention();
    OSReport("platform audio tests passed (%d checks)\n", checks);
    return 0;
}

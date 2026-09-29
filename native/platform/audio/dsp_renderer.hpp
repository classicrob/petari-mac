#pragma once
// Native renderer for the JAudio2 DSP microcode used by Super Mario Galaxy
// (jdsp[] in src/JSystem/JAudio2/dsptask.cpp; HashEctor 0xD643001F).
//
// Provenance: an original implementation for this repository. The microcode's
// behaviour (voice parameter block fields, mixing buffers, sample sources,
// AFC decoding, 4-tap resampling, volume ramps, Dolby positional mixing,
// filters, reverb blocks) was studied in Dolphin's HLE of this ucode family,
// Source/Core/Core/HW/DSPHLE/UCodes/Zelda.{h,cpp} at dolphin-emu/dolphin
// revision 5102a0339c2177575378107b76541e47cc52122d (GPL-2.0-or-later). No
// Dolphin code is copied. The flags for this ucode there are NO_ARAM |
// MAKE_DOLBY_LOUDER | COMBINED_CMD_0D. Unresolved fidelity, shared with that
// reference or specific to this port, is listed in native/platform/STATUS.md.
//
// Voice parameter blocks are JASDsp::TChannel (0x180 bytes). Natively the
// 16-bit fields are host-endian and the 32-bit fields are host-endian 32-bit
// integers (TChannel declares them as int/u32), not the DSP's high/low word
// pairs. Reverb parameter blocks are JASDsp::FxBuf, whose buffer pointer is a
// host pointer; ReverbBlock mirrors that native layout.

#include <array>
#include <cstddef>
#include <cstdint>

namespace PetariNative::Platform::DSP {

constexpr std::size_t kSubframeSamples = 0x50;
constexpr std::size_t kVoiceBlockSize = 0x180;
using MixBuffer = std::array<std::int16_t, kSubframeSamples>;

// Mirror of the native JASDsp::FxBuf layout (checked by static_assert in
// JASDSPInterface.cpp).
struct ReverbBlock {
    std::uint16_t enabled;         // bit 0: filter before sending, bit 1: after
    std::uint16_t circularBlocks;  // circular buffer size in 0x50-sample blocks
    std::int16_t* buffer;          // circular buffer (host memory)
    std::uint16_t dest0Id;
    std::uint16_t dest0Volume;     // 1.15
    std::uint16_t dest1Id;
    std::uint16_t dest1Volume;
    std::int16_t filter[8];
};

static_assert(offsetof(ReverbBlock, buffer) == 0x08 && offsetof(ReverbBlock, dest0Id) == 0x10 && offsetof(ReverbBlock, dest1Volume) == 0x16 &&
                  offsetof(ReverbBlock, filter) == 0x18 && sizeof(ReverbBlock) == 0x28,
              "must match the native JASDsp::FxBuf layout");

struct Tables {
    std::array<std::int16_t, 0x100> resampling{};  // 64 phases x 4 taps
    std::array<std::int16_t, 0x100> patterns{};    // 4 constant patterns of 0x40
    std::array<std::int16_t, 0x80> sine{};         // Dolby panning law
    std::array<std::int16_t, 0x20> afc{};          // 16 AFC coefficient pairs

    // From DSPRES_FILTER (u32 words whose big-endian halves are consecutive
    // s16 values) and DSPADPCM_FILTER (big-endian s16 bytes), as command 01
    // downloads them.
    void load(const std::uint32_t* resFilter, const std::uint8_t* adpcmFilter);
};

class Renderer {
public:
    // ARAM: host base and size of the ARAM that voice base addresses index.
    void setAram(const std::uint8_t* base, std::uint32_t size) {
        mAramBase = base;
        mAramSize = size;
    }
    void setTables(const Tables& tables) { mTables = tables; }
    void setReverbBlocks(ReverbBlock* blocks) { mReverb = blocks; }

    // One 0x50-sample subframe: prepareFrame, addVoice for each enabled voice,
    // finalizeFrame writes the front left/right mix, scaled by outputVolume
    // (4.12), to left/right.
    void prepareFrame();
    void addVoice(std::uint8_t* voiceBlock);
    void finalizeFrame(std::int16_t* left, std::int16_t* right, std::uint16_t outputVolume);

    // Mixing buffer for a DSP buffer address (VPB channel id), or null.
    MixBuffer* bufferForId(std::uint16_t id);

    // Set when a voice read outside ARAM; the voice is stopped instead.
    std::uint32_t aramFaults() const { return mAramFaults; }

private:
    struct Voice;
    void loadInput(MixBuffer& out, Voice& v);
    void downloadPcm(std::int16_t* dst, Voice& v, std::uint32_t count, int bytesPerSample);
    void downloadAfc(std::int16_t* dst, Voice& v, std::uint32_t count);
    void decodeAfc(Voice& v, std::int16_t* dst, std::size_t blocks);
    void resample(Voice& v, const std::int16_t* src, MixBuffer& dst);
    void applyReverb(bool postRendering);
    const std::uint8_t* aram(std::uint32_t offset, std::uint32_t size);

    const std::uint8_t* mAramBase = nullptr;
    std::uint32_t mAramSize = 0;
    std::uint32_t mAramFaults = 0;
    Tables mTables;
    ReverbBlock* mReverb = nullptr;
    bool mPrepared = false;

    MixBuffer mFrontLeft{}, mFrontRight{}, mBackLeft{}, mBackRight{};
    MixBuffer mFrontLeftReverb{}, mFrontRightReverb{}, mBackLeftReverb{}, mBackRightReverb{};
    MixBuffer mAux0Reverb{}, mAux1Reverb{}, mAux0{}, mAux1{}, mAux2{};
    std::array<std::uint16_t, 4> mReverbPosition{};
    std::array<std::array<std::int16_t, 8>, 4> mReverbHistory{};
};

}  // namespace PetariNative::Platform::DSP

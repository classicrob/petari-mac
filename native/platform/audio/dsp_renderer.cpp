// See dsp_renderer.hpp for provenance.

#include "dsp_renderer.hpp"

#include <algorithm>
#include <cstring>

namespace PetariNative::Platform::DSP {
namespace {

// Voice parameter block byte offsets (TChannel layout; DSP word offset x 2).
enum : std::size_t {
    kEnabled = 0x00,
    kDone = 0x02,
    kRatio = 0x04,  // 4.12 raw samples per output sample
    kReset = 0x08,
    kEndReached = 0x0A,
    kUseConstant = 0x0C,
    kChannels = 0x10,  // 6 x {id, target volume, current volume, flags}
    kDolbyPosition = 0x50,
    kDolbyReverb = 0x52,
    kDolbyCurrent = 0x54,
    kDolbyTarget = 0x56,
    kUseDolby = 0x58,
    kPosFrac = 0x60,
    kAfcRemaining = 0x64,
    kConstantSample = 0x66,
    kCurrentPosition = 0x68,  // u32
    kCurrentAram = 0x70,      // u32
    kRemainingLength = 0x74,  // u32
    kResampleHistory = 0x78,  // s16[4]
    kBiquadXn1 = 0xA8,
    kBiquadXn2 = 0xAA,
    kBiquadYn1 = 0xAC,
    kBiquadYn2 = 0xAE,
    kAfcSamples = 0xB0,  // s16[16]; [14] = yn2, [15] = yn1
    kLowPassYn1 = 0xD0,
    kLowPassXn1 = 0xD2,
    kSourceType = 0x100,
    kLooping = 0x102,
    kLoopYn1 = 0x104,
    kLoopYn2 = 0x106,
    kFilterFlags = 0x108,  // bits 0-4 FIR size, bit 5 biquad
    kEndRequested = 0x10A,
    kLoopAddress = 0x110,        // u32, loop start in samples
    kLoopStartPosition = 0x114,  // u32, length through the loop end
    kBaseAddress = 0x118,        // u32, ARAM offset of sample data
    kBiquadBn1 = 0x148,
    kBiquadBn2 = 0x14A,
    kBiquadAn1 = 0x14C,
    kBiquadAn2 = 0x14E,
    kLowPassCoeff = 0x150,
};

enum SourceType : std::uint16_t {
    kSquare = 0,
    kSaw = 1,
    kSquare25 = 3,
    kPattern1 = 4,
    kAfcLow = 5,  // also the bytes per 16-sample block
    kPattern0 = 7,
    kPcm8 = 8,
    kAfcHigh = 9,
    kPattern0Variable = 10,
    kPattern2 = 11,
    kPattern3 = 12,
    kPcm16 = 16,
};

// Signed value in 16.16 fixed point (multiplication: shifting a negative value is undefined).
std::int32_t fixed16(std::int32_t v) {
    return v * 65536;
}

std::int16_t clamp16(std::int64_t v) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(v, -0x8000, 0x7FFF));
}

std::int16_t saturatingAdd(std::int16_t a, std::int32_t b) {
    return clamp16(static_cast<std::int64_t>(a) + b);
}

// Adds src * volume, ramping volume (16.16) by step per sample. Returns the
// final volume.
std::int32_t addWithRamp(MixBuffer& dst, const MixBuffer& src, std::int32_t volume, std::int32_t step) {
    if (volume == 0 && step == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < kSubframeSamples; ++i) {
        dst[i] = saturatingAdd(dst[i], ((volume >> 16) * src[i]) >> 16);
        volume += step;
    }
    return volume;
}

// Adds src * volume (unsigned 1.15, as the ucode treats it).
void addWithVolume(std::int16_t* dst, const std::int16_t* src, std::size_t count, std::uint16_t volume) {
    for (std::size_t i = 0; i < count; ++i) {
        dst[i] = saturatingAdd(dst[i], clamp16((static_cast<std::int32_t>(src[i]) * volume) >> 15));
    }
}

void scale(MixBuffer& buf, std::uint16_t volume, int fractionBits) {
    for (auto& s : buf) {
        s = clamp16((static_cast<std::int64_t>(s) * volume) >> fractionBits);
    }
}

}  // namespace

struct Renderer::Voice {
    std::uint8_t* p;

    std::uint16_t u16(std::size_t off) const {
        std::uint16_t v;
        std::memcpy(&v, p + off, 2);
        return v;
    }
    std::int16_t s16(std::size_t off) const { return static_cast<std::int16_t>(u16(off)); }
    void set16(std::size_t off, std::int32_t v) {
        const std::uint16_t w = static_cast<std::uint16_t>(v);
        std::memcpy(p + off, &w, 2);
    }
    std::uint32_t u32(std::size_t off) const {
        std::uint32_t v;
        std::memcpy(&v, p + off, 4);
        return v;
    }
    void set32(std::size_t off, std::uint32_t v) { std::memcpy(p + off, &v, 4); }
    std::int16_t* samples(std::size_t off) { return reinterpret_cast<std::int16_t*>(p + off); }
};

void Tables::load(const std::uint32_t* resFilter, const std::uint8_t* adpcmFilter) {
    auto half = [&](std::size_t index) -> std::int16_t {
        const std::uint32_t word = resFilter[index / 2];
        return static_cast<std::int16_t>((index & 1) ? (word & 0xFFFF) : (word >> 16));
    };
    for (std::size_t i = 0; i < resampling.size(); ++i) {
        resampling[i] = half(i);
    }
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        patterns[i] = half(0x100 + i);
    }
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = half(0x200 + i);
    }
    for (std::size_t i = 0; i < afc.size(); ++i) {
        afc[i] = static_cast<std::int16_t>((adpcmFilter[i * 2] << 8) | adpcmFilter[i * 2 + 1]);
    }
}

const std::uint8_t* Renderer::aram(std::uint32_t offset, std::uint32_t size) {
    if (mAramBase == nullptr || static_cast<std::uint64_t>(offset) + size > mAramSize) {
        ++mAramFaults;
        return nullptr;
    }
    return mAramBase + offset;
}

MixBuffer* Renderer::bufferForId(std::uint16_t id) {
    switch (id) {
    case 0x0D00: return &mFrontLeft;
    case 0x0D60: return &mFrontRight;
    case 0x0F40: return &mBackLeft;
    case 0x0CA0: return &mBackRight;
    case 0x0E80: return &mFrontLeftReverb;
    case 0x0EE0: return &mFrontRightReverb;
    case 0x0C00: return &mBackLeftReverb;
    case 0x0C50: return &mBackRightReverb;
    case 0x0DC0: return &mAux0Reverb;
    case 0x0E20: return &mAux1Reverb;
    case 0x09A0: return &mAux0;
    case 0x0FA0: return &mAux1;
    case 0x0B00: return &mAux2;
    default: return nullptr;
    }
}

void Renderer::prepareFrame() {
    if (mPrepared) {
        return;
    }
    mFrontLeft.fill(0);
    mFrontRight.fill(0);
    scale(mBackLeft, 0x6784, 15);
    scale(mBackRight, 0x6784, 15);

    applyReverb(false);
    addWithVolume(mFrontLeftReverb.data(), mBackLeftReverb.data(), 0x50, 0x7FFF);
    addWithVolume(mFrontRightReverb.data(), mBackLeftReverb.data(), 0x50, 0xB820);
    addWithVolume(mFrontLeftReverb.data(), mBackRightReverb.data() + 0x28, 0x28, 0xB820);
    addWithVolume(mFrontRightReverb.data(), mBackRightReverb.data() + 0x28, 0x28, 0x7FFF);
    mBackLeftReverb.fill(0);
    mBackRightReverb.fill(0);

    // Patterns 2 and 3 evolve every frame (noise-like sources).
    std::int16_t* p2 = mTables.patterns.data() + 0x80;
    std::int32_t yn2 = p2[0x3E], yn1 = p2[0x3F];
    for (int i = 0; i < 0x40; i += 2) {
        std::int32_t v = yn2 * yn1 - fixed16(p2[i]);
        yn2 = yn1;
        yn1 = p2[i];
        p2[i] = static_cast<std::int16_t>(v >> 16);
        v = 2 * (yn2 * yn1 + fixed16(p2[i + 1]));
        yn2 = yn1;
        yn1 = p2[i + 1];
        p2[i + 1] = static_cast<std::int16_t>(v >> 16);
    }
    std::int16_t* p3 = mTables.patterns.data() + 0xC0;
    yn2 = p3[0x3E];
    yn1 = p3[0x3F];
    const std::int16_t acc = static_cast<std::int16_t>(yn1);
    std::int16_t step = static_cast<std::int16_t>(p3[0] + ((yn1 * yn2 + (fixed16(yn2) + yn1)) >> 16));
    step = static_cast<std::int16_t>((step & 0x1FF) | 0x2000);
    for (int i = 0; i < 0x40; ++i) {
        p3[i] = static_cast<std::int16_t>(acc + (i + 1) * step);
    }
    mPrepared = true;
}

void Renderer::applyReverb(bool postRendering) {
    if (mReverb == nullptr) {
        return;
    }
    MixBuffer* buses[4] = {&mAux0Reverb, &mAux1Reverb, &mFrontLeftReverb, &mFrontRightReverb};
    for (int r = 0; r < 4; ++r) {
        const ReverbBlock& block = mReverb[r];
        if (!block.enabled || block.buffer == nullptr || block.circularBlocks == 0) {
            continue;
        }
        std::uint16_t& position = mReverbPosition[r];
        if (position >= block.circularBlocks) {
            position = 0;
        }
        std::int16_t* slot = block.buffer + position * kSubframeSamples;
        if (!postRendering) {
            std::array<std::int16_t, 0x58> buf;
            std::copy(mReverbHistory[r].begin(), mReverbHistory[r].end(), buf.begin());
            std::copy(slot, slot + kSubframeSamples, buf.begin() + 8);
            std::copy(buf.begin() + 0x50, buf.begin() + 0x58, mReverbHistory[r].begin());
            auto filter = [&] {
                for (std::size_t i = 0; i < kSubframeSamples; ++i) {
                    std::int32_t acc = 0;
                    for (int j = 0; j < 8; ++j) {
                        acc += static_cast<std::int32_t>(buf[i + j]) * block.filter[j];
                    }
                    buf[i] = clamp16(acc >> 15);
                }
            };
            if (block.enabled & 1) {
                filter();
            }
            const std::pair<std::uint16_t, std::uint16_t> dests[2] = {{block.dest0Id, block.dest0Volume}, {block.dest1Id, block.dest1Volume}};
            for (const auto& [id, volume] : dests) {
                if (id == 0) {
                    continue;
                }
                // SEND_TABLE also names addresses 8 samples into the aux and
                // front reverb buses (0x0DC8, 0x0E28, 0x0E88, 0x0EE8): mixed
                // there with that offset. Unverified against the ucode.
                if (MixBuffer* dest = bufferForId(id)) {
                    addWithVolume(dest->data(), buf.data(), kSubframeSamples, volume);
                } else if (MixBuffer* offsetDest = bufferForId(static_cast<std::uint16_t>(id - 8))) {
                    addWithVolume(offsetDest->data() + 8, buf.data(), kSubframeSamples - 8, volume);
                }
            }
            if (block.enabled & 2) {
                filter();
            }
            std::copy(buf.begin(), buf.begin() + kSubframeSamples, buses[r]->begin());
        } else {
            std::copy(buses[r]->begin(), buses[r]->end(), slot);
            position = static_cast<std::uint16_t>((position + 1) % block.circularBlocks);
        }
    }
}

void Renderer::addVoice(std::uint8_t* block) {
    Voice v{block};
    if (!v.u16(kEnabled) || v.u16(kDone)) {
        return;
    }
    MixBuffer input;
    loadInput(input, v);

    const bool reset = v.u16(kReset) != 0;
    if (const std::int32_t coeff = v.u16(kLowPassCoeff); coeff != 0) {
        std::int32_t yn1 = reset ? 0 : v.s16(kLowPassYn1);
        std::int32_t xn1 = reset ? 0 : v.s16(kLowPassXn1);
        for (auto& s : input) {
            const std::int32_t xn0 = s;
            const std::int16_t yn0 = clamp16(((static_cast<std::int64_t>(xn0 - xn1) * coeff) >> 7) + yn1);
            s = yn0;
            yn1 = yn0;
            xn1 = xn0;
        }
        v.set16(kLowPassYn1, yn1);
        v.set16(kLowPassXn1, xn1);
    }
    const std::uint16_t filterFlags = v.u16(kFilterFlags);
    const std::int16_t bn1 = v.s16(kBiquadBn1), bn2 = v.s16(kBiquadBn2), an1 = v.s16(kBiquadAn1), an2 = v.s16(kBiquadAn2);
    if ((filterFlags & 0x20) && (an2 != 0 || an1 != 0 || bn2 != 0 || bn1 != 0x7FFF)) {
        std::int32_t xn1 = v.s16(kBiquadXn1), xn2 = v.s16(kBiquadXn2), yn1 = v.s16(kBiquadYn1), yn2 = v.s16(kBiquadYn2);
        for (auto& s : input) {
            const std::int32_t xn0 = s;
            const std::int64_t acc = static_cast<std::int64_t>(bn1) * xn1 + static_cast<std::int64_t>(bn2) * xn2 +
                                     static_cast<std::int64_t>(an1) * yn1 + static_cast<std::int64_t>(an2) * yn2;
            const std::int16_t yn0 = clamp16(acc >> 15);
            s = yn0;
            xn2 = xn1;
            xn1 = xn0;
            yn2 = yn1;
            yn1 = yn0;
        }
        v.set16(kBiquadXn1, xn1);
        v.set16(kBiquadXn2, xn2);
        v.set16(kBiquadYn1, yn1);
        v.set16(kBiquadYn2, yn2);
    }

    if (v.u16(kUseDolby)) {
        std::int16_t current = v.s16(kDolbyCurrent);
        std::int16_t target = v.s16(kDolbyTarget);
        if (v.u16(kEndRequested)) {
            target = static_cast<std::int16_t>(current / 2);
            v.set16(kDolbyTarget, target);
            if (target == 0) {
                v.set16(kDone, 1);
            }
        }
        const std::uint16_t position = v.u16(kDolbyPosition);
        const int x = (position >> 8) & 0x7F;
        const int y = position & 0x7F;
        const std::int32_t right = mTables.sine[x], back = mTables.sine[y];
        const std::int32_t left = mTables.sine[x ^ 0x7F], front = mTables.sine[y ^ 0x7F];
        constexpr int kShift = 15;  // MAKE_DOLBY_LOUDER
        std::int16_t quadrant[4] = {
            static_cast<std::int16_t>((left * front) >> kShift),
            static_cast<std::int16_t>((left * back) >> kShift),
            static_cast<std::int16_t>((right * front) >> kShift),
            static_cast<std::int16_t>((right * back) >> kShift),
        };
        const std::int16_t delta = static_cast<std::int16_t>(target - current);
        std::int16_t quadrantDelta[4];
        for (int i = 0; i < 4; ++i) {
            quadrantDelta[i] = static_cast<std::int16_t>((static_cast<std::uint16_t>(quadrant[i]) * delta) >> kShift);
            quadrant[i] = static_cast<std::int16_t>((quadrant[i] * current) >> kShift);
        }
        const std::int16_t reverbFactor = v.s16(kDolbyReverb);
        MixBuffer* dry[4] = {&mFrontLeft, &mBackLeft, &mFrontRight, &mBackRight};
        MixBuffer* wet[4] = {&mFrontLeftReverb, &mBackLeftReverb, &mFrontRightReverb, &mBackRightReverb};
        for (int i = 0; i < 4; ++i) {
            addWithRamp(*dry[i], input, fixed16(quadrant[i]), fixed16(quadrantDelta[i]) / static_cast<std::int32_t>(kSubframeSamples));
            const std::int16_t wetVolume = static_cast<std::int16_t>((quadrant[i] * reverbFactor) >> kShift);
            const std::int16_t wetDelta = static_cast<std::int16_t>((quadrantDelta[i] * reverbFactor) >> kShift);
            addWithRamp(*wet[i], input, fixed16(wetVolume), fixed16(wetDelta) / static_cast<std::int32_t>(kSubframeSamples));
        }
        v.set16(kDolbyCurrent, target);
    } else {
        constexpr int kChannelCount = 6;
        if (v.u16(kEndRequested)) {
            bool allMute = true;
            for (int i = 0; i < kChannelCount; ++i) {
                const std::size_t c = kChannels + i * 8;
                const std::int16_t target = static_cast<std::int16_t>(v.s16(c + 4) / 2);
                v.set16(c + 2, target);
                allMute = allMute && target == 0;
            }
            if (allMute) {
                v.set16(kDone, 1);
            }
        }
        for (int i = 0; i < kChannelCount; ++i) {
            const std::size_t c = kChannels + i * 8;
            const std::uint16_t id = v.u16(c);
            if (id == 0) {
                continue;
            }
            const std::int16_t target = v.s16(c + 2);
            const std::int16_t current = v.s16(c + 4);
            const std::int32_t step = fixed16(static_cast<std::int16_t>(target - current)) / static_cast<std::int32_t>(kSubframeSamples);
            if (current == 0 && step == 0) {
                continue;
            }
            MixBuffer* dest = bufferForId(id);
            if (dest == nullptr) {
                continue;
            }
            const std::int32_t volume = addWithRamp(*dest, input, fixed16(current), step);
            v.set16(c + 4, volume >> 16);
        }
    }
    if (!v.u16(kUseConstant)) {
        v.set16(kReset, 0);
    }
}

void Renderer::finalizeFrame(std::int16_t* left, std::int16_t* right, std::uint16_t outputVolume) {
    scale(mFrontLeft, outputVolume, 12);
    scale(mFrontRight, outputVolume, 12);
    std::copy(mFrontLeft.begin(), mFrontLeft.end(), left);
    std::copy(mFrontRight.begin(), mFrontRight.end(), right);
    applyReverb(true);
    mPrepared = false;
}

void Renderer::loadInput(MixBuffer& out, Voice& v) {
    // 4 history samples, then up to 0x500 raw samples (ratio 0xFFFF), plus a
    // block of AFC rounding slack.
    std::array<std::int16_t, 4 + 0x500 + 0x20> raw{};
    std::memcpy(raw.data(), v.samples(kResampleHistory), 8);
    if (v.u16(kUseConstant)) {
        out.fill(v.s16(kConstantSample));
        return;
    }
    const std::uint16_t ratio = v.u16(kRatio);
    const std::uint16_t type = v.u16(kSourceType);
    switch (type) {
    case kSquare:
    case kSquare25: {
        const std::uint32_t shift = type == kSquare ? 1 : 2;
        const std::uint32_t mask = (1u << shift) - 1;
        const std::uint32_t step = static_cast<std::uint32_t>(ratio) << (shift - 1);
        std::uint32_t pos = static_cast<std::uint32_t>(v.u16(kPosFrac)) << shift;
        for (auto& s : out) {
            s = ((pos >> 16) & mask) ? static_cast<std::int16_t>(0xC000) : static_cast<std::int16_t>(0x4000);
            pos += step;
        }
        v.set16(kPosFrac, (pos >> shift) & 0xFFFF);
        return;
    }
    case kSaw: {
        std::uint32_t pos = v.u16(kPosFrac);
        for (auto& s : out) {
            s = static_cast<std::int16_t>(pos & 0xFFFF);
            pos += ratio >> 1;
        }
        v.set16(kPosFrac, pos & 0xFFFF);
        return;
    }
    case kPattern0:
    case kPattern0Variable:
    case kPattern1:
    case kPattern2:
    case kPattern3: {
        const int index = type == kPattern1 ? 1 : type == kPattern2 ? 2 : type == kPattern3 ? 3 : 0;
        const std::int16_t* pattern = mTables.patterns.data() + index * 0x40;
        std::uint32_t pos = static_cast<std::uint32_t>(v.u16(kPosFrac)) << 6;
        const std::uint32_t step = static_cast<std::uint32_t>(ratio) << 5;
        for (std::size_t i = 0; i < kSubframeSamples; ++i) {
            out[i] = pattern[pos >> 16];
            pos = (pos + step) % (0x40u << 16);
            if (type == kPattern0Variable) {
                pos = static_cast<std::uint32_t>((static_cast<std::int64_t>(pos) * 1024 + mBackRight[i] * ratio) >> 10) % (0x40u << 16);
            }
        }
        v.set16(kPosFrac, pos >> 6);
        return;
    }
    case kPcm8:
    case kPcm16:
    case kAfcLow:
    case kAfcHigh: {
        const std::uint32_t needed = (v.u16(kPosFrac) + kSubframeSamples * ratio) >> 12;
        if (type == kPcm8) {
            downloadPcm(raw.data() + 4, v, needed, 1);
        } else if (type == kPcm16) {
            downloadPcm(raw.data() + 4, v, needed, 2);
        } else {
            downloadAfc(raw.data() + 4, v, needed);
        }
        resample(v, raw.data(), out);
        return;
    }
    default:
        // Unknown source types (2, 6, 13-15, MRAM sources) are not used by JAudio2.
        ++mAramFaults;
        out.fill(0);
        v.set16(kDone, 1);
        return;
    }
}

void Renderer::downloadPcm(std::int16_t* dst, Voice& v, std::uint32_t count, int bytesPerSample) {
    if (v.u16(kDone)) {
        std::fill(dst, dst + count, 0);
        return;
    }
    if (v.u16(kReset)) {
        const std::uint32_t position = v.u32(kCurrentPosition);
        v.set32(kRemainingLength, v.u32(kLoopStartPosition) - position);
        v.set32(kCurrentAram, v.u32(kBaseAddress) + position * bytesPerSample);
    }
    v.set16(kEndReached, 0);
    while (count) {
        if (v.u16(kEndReached)) {
            v.set16(kEndReached, 0);
            if (!v.u16(kLooping)) {
                std::fill(dst, dst + count, 0);
                v.set16(kDone, 1);
                return;
            }
            const std::uint32_t position = v.u32(kLoopAddress);
            v.set32(kCurrentPosition, position);
            v.set32(kRemainingLength, v.u32(kLoopStartPosition) - position);
            v.set32(kCurrentAram, v.u32(kBaseAddress) + position * bytesPerSample);
        }
        const std::uint32_t n = std::min(v.u32(kRemainingLength), count);
        const std::uint8_t* src = aram(v.u32(kCurrentAram), n * bytesPerSample);
        if (src == nullptr) {
            std::fill(dst, dst + count, 0);
            v.set16(kDone, 1);
            return;
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            // Sample data is big-endian, as stored on the disc.
            *dst++ = bytesPerSample == 1 ? static_cast<std::int16_t>(static_cast<std::int8_t>(src[i]) * 256)
                                         : static_cast<std::int16_t>((src[i * 2] << 8) | src[i * 2 + 1]);
        }
        v.set32(kRemainingLength, v.u32(kRemainingLength) - n);
        v.set32(kCurrentAram, v.u32(kCurrentAram) + n * bytesPerSample);
        count -= n;
        if (v.u32(kRemainingLength) == 0) {
            v.set16(kEndReached, 1);
        }
    }
}

void Renderer::decodeAfc(Voice& v, std::int16_t* dst, std::size_t blocks) {
    const std::uint16_t type = v.u16(kSourceType);  // bytes per block
    const std::uint32_t addr = v.u32(kCurrentAram);
    const std::uint8_t* src = aram(addr, static_cast<std::uint32_t>(blocks * type));
    v.set32(kCurrentAram, addr + static_cast<std::uint32_t>(blocks * type));
    if (src == nullptr) {
        std::fill(dst, dst + blocks * 16, 0);
        v.set16(kDone, 1);
        return;
    }
    std::int16_t* history = v.samples(kAfcSamples);
    std::int32_t yn1 = history[15], yn2 = history[14];
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::int32_t scaleFactor = 1 << ((src[0] >> 4) & 0xF);
        const int coefIndex = src[0] & 0xF;
        ++src;
        std::int16_t deltas[16];
        if (type == kAfcHigh) {
            for (int i = 0; i < 16; i += 2) {
                deltas[i] = static_cast<std::int16_t>(static_cast<std::int16_t>(src[0] << 8 & 0xF000) >> 1);
                deltas[i + 1] = static_cast<std::int16_t>(static_cast<std::int16_t>(src[0] << 12) >> 1);
                ++src;
            }
        } else {
            for (int i = 0; i < 16; i += 4) {
                for (int k = 0; k < 4; ++k) {
                    const int bits = (src[0] >> (6 - 2 * k)) & 3;
                    deltas[i + k] = static_cast<std::int16_t>(static_cast<std::int16_t>(bits << 14) >> 1);
                }
                ++src;
            }
        }
        const std::int32_t c0 = mTables.afc[coefIndex * 2];
        const std::int32_t c1 = mTables.afc[coefIndex * 2 + 1];
        for (std::int16_t delta : deltas) {
            const std::int32_t sample = std::clamp((scaleFactor * delta + yn1 * c0 + yn2 * c1) >> 11, -0x8000, 0x7FFF);
            *dst++ = static_cast<std::int16_t>(sample);
            yn2 = yn1;
            yn1 = sample;
        }
    }
    history[14] = static_cast<std::int16_t>(yn2);
    history[15] = static_cast<std::int16_t>(yn1);
}

void Renderer::downloadAfc(std::int16_t* dst, Voice& v, std::uint32_t count) {
    std::int16_t* cache = v.samples(kAfcSamples);
    if (v.u16(kReset)) {
        cache[14] = 0;
        cache[15] = 0;
        v.set16(kAfcRemaining, 0);
        v.set32(kRemainingLength, v.u32(kLoopStartPosition));
        v.set32(kCurrentAram, v.u32(kBaseAddress));
    }
    if (v.u16(kDone)) {
        std::fill(dst, dst + count, 0);
        return;
    }
    if (v.u16(kAfcRemaining) > 16) {
        v.set16(kAfcRemaining, 16);
    }
    while (true) {
        // Samples left over from the previously decoded block.
        const std::uint32_t cached = std::min<std::uint32_t>(v.u16(kAfcRemaining), count);
        const std::int16_t* from = cache + (16 - v.u16(kAfcRemaining));
        std::copy(from, from + cached, dst);
        dst += cached;
        v.set16(kAfcRemaining, v.u16(kAfcRemaining) - cached);
        count -= cached;
        if (count == 0) {
            return;
        }
        const std::uint32_t remaining = v.u32(kRemainingLength);
        if (count <= remaining) {
            const std::uint32_t blocks = (count + 15) >> 4;
            const std::uint32_t decoded = blocks << 4;
            if (decoded < remaining) {
                v.set16(kAfcRemaining, decoded - count);
                v.set32(kRemainingLength, remaining - decoded);
            } else {
                v.set16(kAfcRemaining, remaining - count);
                v.set32(kRemainingLength, 0);
            }
            decodeAfc(v, dst, blocks);
            if (v.u16(kAfcRemaining)) {
                std::copy(dst + decoded - 16, dst + decoded, cache);
                const std::uint32_t loopLength = v.u32(kLoopStartPosition);
                if (v.u32(kRemainingLength) == 0 && loopLength) {
                    // Realign the cached tail for the next loop iteration.
                    const std::int16_t* base = cache + ((loopLength + 15) & 15);
                    for (std::uint32_t i = 0; i < v.u16(kAfcRemaining); ++i) {
                        cache[15 - i] = *base--;
                    }
                }
            }
            return;
        }
        if (remaining) {
            count -= remaining;
            decodeAfc(v, dst, (remaining + 15) >> 4);
            dst += remaining;
        }
        if (!v.u16(kLooping)) {
            v.set16(kDone, 1);
            std::fill(dst, dst + count, 0);
            return;
        }
        // Loop: restart at the loop block with the stored predictor history.
        const std::uint32_t loopSample = v.u32(kLoopAddress);
        const std::uint16_t type = v.u16(kSourceType);
        v.set32(kCurrentAram, v.u32(kBaseAddress) + (loopSample >> 4) * type);
        cache[14] = v.s16(kLoopYn2);
        cache[15] = v.s16(kLoopYn1);
        decodeAfc(v, cache, 1);
        if (v.u16(kDone)) {
            std::fill(dst, dst + count, 0);
            return;
        }
        v.set16(kAfcRemaining, 16 - (loopSample & 15));
        v.set32(kRemainingLength, v.u32(kLoopStartPosition) - v.u16(kAfcRemaining) - loopSample);
    }
}

void Renderer::resample(Voice& v, const std::int16_t* src, MixBuffer& dst) {
    const std::uint32_t ratio = v.u16(kRatio);
    std::uint32_t pos = v.u16(kPosFrac);
    if ((ratio >> 12) >= 4) {
        for (auto& s : dst) {
            pos += ratio;
            s = src[pos >> 12];
        }
    } else {
        for (auto& s : dst) {
            const std::int16_t* taps = &mTables.resampling[((pos & 0xFFF) >> 6) * 4];
            const std::int16_t* in = &src[pos >> 12];
            std::int64_t acc = 0;
            for (int i = 0; i < 4; ++i) {
                acc += static_cast<std::int64_t>(2) * taps[i] * in[i];
            }
            s = clamp16(acc >> 16);
            pos += ratio;
        }
    }
    std::memcpy(v.samples(kResampleHistory), &src[pos >> 12], 8);
    v.set16(kConstantSample, dst[kSubframeSamples - 1]);
    v.set16(kPosFrac, pos & 0xFFF);
}

}  // namespace PetariNative::Platform::DSP

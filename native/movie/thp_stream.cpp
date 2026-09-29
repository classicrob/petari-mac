// THP 1.1 container, frame and audio record validation. See petari/movie_thp.hpp.

#include <petari/movie_thp.hpp>

#include <petari/endian.hpp>

#include <cmath>
#include <cstring>

namespace PetariNative::Movie {
namespace {

// Largest per-frame read buffer and audio frame accepted. The retail movies use
// 0xCA40 bytes and 0x222 samples; the limits only reject absurd sizes before
// they reach the game heap.
constexpr std::uint32_t kMaxBufferSize = 16u << 20;
constexpr std::uint32_t kMaxAudioSamples = 1u << 20;
constexpr std::uint32_t kReadBufferCount = 20;

float readF32BE(const void* data) {
    const std::uint32_t bits = readU32BE(data);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint64_t roundUp32(std::uint64_t value) {
    return (value + 31) & ~std::uint64_t{31};
}

}  // namespace

const char* parseThpHeader(const void* data, std::size_t size, ThpHeader* out) {
    if (size < kThpHeaderSize) {
        return "THP header is truncated";
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    ThpHeader h{};
    std::memcpy(h.magic, p, sizeof(h.magic));
    h.version = readU32BE(p + 0x04);
    h.bufSize = readU32BE(p + 0x08);
    h.audioMaxSamples = readU32BE(p + 0x0C);
    h.frameRate = readF32BE(p + 0x10);
    h.numFrames = readU32BE(p + 0x14);
    h.firstFrameSize = readU32BE(p + 0x18);
    h.movieDataSize = readU32BE(p + 0x1C);
    h.compInfoDataOffsets = readU32BE(p + 0x20);
    h.offsetDataOffsets = readU32BE(p + 0x24);
    h.movieDataOffsets = readU32BE(p + 0x28);
    h.finalFrameDataOffsets = readU32BE(p + 0x2C);
    if (std::memcmp(h.magic, "THP\0", 4) != 0) {
        return "THP magic is not \"THP\"";
    }
    if (h.version != kThpVersion11) {
        return "THP version is not 1.1 (0x11000)";
    }
    *out = h;
    return nullptr;
}

const char* parseThpFrameCompInfo(const void* data, std::size_t size, std::uint32_t* numComponents,
                                  std::uint8_t kinds[kThpMaxComponents]) {
    if (size < kThpFrameCompInfoSize) {
        return "THP component table is truncated";
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint32_t count = readU32BE(p);
    if (count == 0 || count > kThpMaxComponents) {
        return "THP component count is outside 1..16";
    }
    for (std::uint32_t i = 0; i < count; i++) {
        if (p[4 + i] != kThpComponentVideo && p[4 + i] != kThpComponentAudio) {
            return "THP component kind is neither video nor audio";
        }
    }
    *numComponents = count;
    std::memcpy(kinds, p + 4, kThpMaxComponents);
    return nullptr;
}

const char* parseThpVideoInfo(const void* data, std::size_t size, ThpVideoInfo* out) {
    if (size < kThpVideoInfoSize) {
        return "THP video info is truncated";
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->xSize = readU32BE(p);
    out->ySize = readU32BE(p + 4);
    out->videoType = readU32BE(p + 8);
    return nullptr;
}

const char* parseThpAudioInfo(const void* data, std::size_t size, ThpAudioInfo* out) {
    if (size < kThpAudioInfoSize) {
        return "THP audio info is truncated";
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    out->sndChannels = readU32BE(p);
    out->sndFrequency = readU32BE(p + 4);
    out->sndNumSamples = readU32BE(p + 8);
    out->sndNumTracks = readU32BE(p + 12);
    return nullptr;
}

const char* parseThpComponents(const void* data, std::size_t size, ThpComponents* out) {
    ThpComponents c{};
    if (const char* error = parseThpFrameCompInfo(data, size, &c.numComponents, c.kinds)) {
        return error;
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t offset = kThpFrameCompInfoSize;
    for (std::uint32_t i = 0; i < c.numComponents; i++) {
        if (c.kinds[i] == kThpComponentVideo) {
            if (c.hasVideo) {
                return "THP has more than one video component";
            }
            if (const char* error = parseThpVideoInfo(p + offset, size - offset, &c.video)) {
                return error;
            }
            c.hasVideo = true;
            offset += kThpVideoInfoSize;
        } else {
            if (c.hasAudio) {
                return "THP has more than one audio component";
            }
            if (const char* error = parseThpAudioInfo(p + offset, size - offset, &c.audio)) {
                return error;
            }
            c.hasAudio = true;
            offset += kThpAudioInfoSize;
        }
    }
    c.byteSize = static_cast<std::uint32_t>(offset);
    *out = c;
    return nullptr;
}

std::uint32_t thpFrameHeaderSize(const ThpComponents& components) {
    return 8 + 4 * components.numComponents;
}

std::size_t thpI8TextureBytes(std::uint32_t width, std::uint32_t height) {
    return static_cast<std::size_t>((width + 7) / 8) * ((height + 3) / 4) * 32;
}

std::size_t thpWrapperLumaBytes(const ThpVideoInfo& video) {
    return roundUp32(std::uint64_t{video.xSize} * video.ySize);
}

std::size_t thpWrapperChromaBytes(const ThpVideoInfo& video) {
    return roundUp32(std::uint64_t{video.xSize} * video.ySize / 4);
}

const char* validateThpStream(const ThpHeader& header, const ThpComponents& components, std::uint64_t fileSize) {
    if (header.numFrames == 0) {
        return "THP has no frames";
    }
    if (!std::isfinite(header.frameRate) || header.frameRate <= 0.0f) {
        return "THP frame rate is not a positive number";
    }
    if (header.bufSize == 0 || header.bufSize > kMaxBufferSize) {
        return "THP frame buffer size is outside the supported range";
    }
    if (header.compInfoDataOffsets < kThpHeaderSize ||
        std::uint64_t{header.compInfoDataOffsets} + components.byteSize > header.movieDataOffsets) {
        return "THP component table overlaps the header or movie data";
    }
    if ((header.compInfoDataOffsets & 3) != 0 || (header.movieDataOffsets & 3) != 0) {
        return "THP table offsets are not 4-byte aligned";
    }
    const std::uint64_t movieEnd = std::uint64_t{header.movieDataOffsets} + header.movieDataSize;
    if (movieEnd > fileSize) {
        return "THP movie data extends past the end of the file";
    }
    if (header.finalFrameDataOffsets < header.movieDataOffsets || header.finalFrameDataOffsets >= movieEnd) {
        return "THP final frame offset is outside the movie data";
    }
    if (header.offsetDataOffsets != 0 && header.offsetDataOffsets >= fileSize) {
        return "THP frame offset table is outside the file";
    }
    if (const char* error = checkThpFrameRead(header, components, fileSize, header.movieDataOffsets,
                                              header.firstFrameSize)) {
        return error;
    }
    if (!components.hasVideo) {
        return "THP has no video component";
    }
    const ThpVideoInfo& video = components.video;
    if (video.xSize == 0 || video.ySize == 0 || video.xSize > kThpMaxDimension || video.ySize > kThpMaxDimension) {
        return "THP video dimensions are outside 1..1024";
    }
    if (video.videoType != 0) {
        return "THP interlaced video is not supported by the simple player";
    }
    if (thpI8TextureBytes(video.xSize, video.ySize) > thpWrapperLumaBytes(video) ||
        thpI8TextureBytes((video.xSize + 1) / 2, (video.ySize + 1) / 2) > thpWrapperChromaBytes(video)) {
        return "THP video dimensions produce textures larger than the player's buffers";
    }
    if (components.hasAudio) {
        const ThpAudioInfo& audio = components.audio;
        if (audio.sndChannels != 1 && audio.sndChannels != 2) {
            return "THP audio channel count is not 1 or 2";
        }
        if (audio.sndNumTracks == 0) {
            return "THP audio has no tracks";
        }
        if (audio.sndFrequency == 0) {
            return "THP audio frequency is zero";
        }
        if (header.audioMaxSamples == 0 || header.audioMaxSamples > kMaxAudioSamples) {
            return "THP audio frame size is outside the supported range";
        }
    }
    // calcNeedMemory() keeps its sum in u32.
    std::uint64_t need = roundUp32(header.bufSize) * kReadBufferCount + roundUp32(std::uint64_t{video.xSize} * video.ySize * 2) +
                         2 * roundUp32(std::uint64_t{video.xSize} * video.ySize / 2) + 0x1000;
    if (components.hasAudio) {
        need += roundUp32(std::uint64_t{header.audioMaxSamples} * 4) * kReadBufferCount;
    }
    if (need > 0xFFFFFFFFu) {
        return "THP player memory requirement overflows 32 bits";
    }
    return nullptr;
}

const char* checkThpFrameRead(const ThpHeader& header, const ThpComponents& components, std::uint64_t fileSize,
                              std::uint32_t offset, std::uint32_t size) {
    if (size < thpFrameHeaderSize(components)) {
        return "THP frame is smaller than its header";
    }
    if (size > header.bufSize) {
        return "THP frame is larger than the read buffer";
    }
    if ((offset & 3) != 0) {
        return "THP frame offset is not 4-byte aligned";
    }
    const std::uint64_t end = std::uint64_t{offset} + size;
    if (offset < header.movieDataOffsets || end > std::uint64_t{header.movieDataOffsets} + header.movieDataSize ||
        end > fileSize) {
        return "THP frame lies outside the movie data";
    }
    return nullptr;
}

const char* parseThpFrame(const void* data, std::size_t size, const ThpComponents& components, ThpFrame* out) {
    const std::uint32_t headerSize = thpFrameHeaderSize(components);
    if (size < headerSize) {
        return "THP frame header is truncated";
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    ThpFrame frame{};
    frame.nextFrameSize = readU32BE(p);
    frame.prevFrameSize = readU32BE(p + 4);
    std::uint64_t offset = headerSize;
    for (std::uint32_t i = 0; i < components.numComponents; i++) {
        const std::uint32_t componentSize = readU32BE(p + 8 + 4 * i);
        std::uint64_t extent = componentSize;
        if (components.kinds[i] == kThpComponentAudio) {
            extent *= components.audio.sndNumTracks;
        }
        if (componentSize == 0 || offset + extent > size) {
            return "THP frame component lies outside the frame";
        }
        frame.componentSizes[i] = componentSize;
        frame.componentOffsets[i] = static_cast<std::uint32_t>(offset);
        offset += componentSize;
    }
    *out = frame;
    return nullptr;
}

std::size_t thpAdpcmChannelBytes(std::uint32_t samples) {
    // Each 8-byte ADPCM frame holds a predictor/scale byte and 14 nibbles.
    const std::size_t full = samples / 14;
    const std::size_t rest = samples % 14;
    const std::size_t bytes = full * 8 + (rest != 0 ? 1 + (rest + 1) / 2 : 0);
    // The decoder reads the first header byte even for an empty channel.
    return bytes != 0 ? bytes : 1;
}

const char* validateThpAudioComponent(const void* data, std::size_t size, const ThpAudioInfo& audio,
                                      std::uint32_t audioMaxSamples, std::uint32_t* samples) {
    (void)audio;
    if (size < kThpAudioRecordHeaderSize) {
        return "THP audio record header is truncated";
    }
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::uint32_t nextChannel = readU32BE(p);
    const std::uint32_t count = readU32BE(p + 4);
    if (count == 0) {
        // The player's mixer never advances past an empty audio buffer.
        return "THP audio record has no samples";
    }
    if (count > audioMaxSamples) {
        return "THP audio record exceeds the stream's maximum samples per frame";
    }
    const std::uint64_t channelBytes = thpAdpcmChannelBytes(count);
    if (kThpAudioRecordHeaderSize + channelBytes > size) {
        return "THP audio left channel is truncated";
    }
    if (nextChannel != 0 && kThpAudioRecordHeaderSize + std::uint64_t{nextChannel} + channelBytes > size) {
        return "THP audio right channel is truncated";
    }
    *samples = count;
    return nullptr;
}

}  // namespace PetariNative::Movie

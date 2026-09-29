#pragma once
// Bounded readers for THP 1.1 movie files (MovieData/*.thp).
//
// THP containers are big-endian. These functions decode the serialized fields
// into host-order structures and check every size and offset against the bytes
// that are actually available. The SDK-compatible decoders (THPVideoDecode,
// THPAudioDecode) take no input length, so a frame must pass
// validateThpVideoComponent / validateThpAudioComponent before it is decoded.
//
// Every function returns nullptr on success or a static error description.

#include <cstddef>
#include <cstdint>

namespace PetariNative::Movie {

constexpr std::uint32_t kThpVersion11 = 0x11000;
constexpr std::size_t kThpHeaderSize = 0x30;
constexpr std::size_t kThpFrameCompInfoSize = 0x14;
constexpr std::size_t kThpVideoInfoSize = 12;
constexpr std::size_t kThpAudioInfoSize = 16;
constexpr std::size_t kThpMaxComponents = 16;
constexpr std::size_t kThpAudioRecordHeaderSize = 80;
// GX I8 textures are limited to 1024x1024 texels.
constexpr std::uint32_t kThpMaxDimension = 1024;

enum ThpComponentKind : std::uint8_t {
    kThpComponentVideo = 0,
    kThpComponentAudio = 1,
    kThpComponentNone = 0xFF,
};

struct ThpHeader {
    char magic[4];
    std::uint32_t version;
    std::uint32_t bufSize;
    std::uint32_t audioMaxSamples;
    float frameRate;
    std::uint32_t numFrames;
    std::uint32_t firstFrameSize;
    std::uint32_t movieDataSize;
    std::uint32_t compInfoDataOffsets;
    std::uint32_t offsetDataOffsets;
    std::uint32_t movieDataOffsets;
    std::uint32_t finalFrameDataOffsets;
};

struct ThpVideoInfo {
    std::uint32_t xSize;
    std::uint32_t ySize;
    std::uint32_t videoType;
};

struct ThpAudioInfo {
    std::uint32_t sndChannels;
    std::uint32_t sndFrequency;
    std::uint32_t sndNumSamples;
    std::uint32_t sndNumTracks;
};

struct ThpComponents {
    std::uint32_t numComponents;
    std::uint8_t kinds[kThpMaxComponents];
    bool hasVideo;
    bool hasAudio;
    ThpVideoInfo video;
    ThpAudioInfo audio;
    // Serialized size of the component table and its info records.
    std::uint32_t byteSize;
};

struct ThpFrame {
    std::uint32_t nextFrameSize;
    std::uint32_t prevFrameSize;
    std::uint32_t componentSizes[kThpMaxComponents];
    // Offsets from the start of the frame.
    std::uint32_t componentOffsets[kThpMaxComponents];
};

// Serialized records. Each reads exactly the record size from `size` bytes.
const char* parseThpHeader(const void* data, std::size_t size, ThpHeader* out);
const char* parseThpFrameCompInfo(const void* data, std::size_t size, std::uint32_t* numComponents,
                                  std::uint8_t kinds[kThpMaxComponents]);
const char* parseThpVideoInfo(const void* data, std::size_t size, ThpVideoInfo* out);
const char* parseThpAudioInfo(const void* data, std::size_t size, ThpAudioInfo* out);

// The component table and the info records that follow it, in file order.
const char* parseThpComponents(const void* data, std::size_t size, ThpComponents* out);

// Cross-checks header and components against the file length: frame data
// bounds, buffer sizes, supported component kinds, audio format and the
// texture sizes used by THPSimplePlayerWrapper::setBuffer.
const char* validateThpStream(const ThpHeader& header, const ThpComponents& components, std::uint64_t fileSize);

// Bytes the frame header occupies (next/prev sizes plus component sizes).
std::uint32_t thpFrameHeaderSize(const ThpComponents& components);

// Checks a frame read before it is issued: size fits the read buffer and file.
const char* checkThpFrameRead(const ThpHeader& header, const ThpComponents& components, std::uint64_t fileSize,
                              std::uint32_t offset, std::uint32_t size);

// Decodes the frame header and checks that every component (all audio tracks)
// lies within `size`.
const char* parseThpFrame(const void* data, std::size_t size, const ThpComponents& components, ThpFrame* out);

// Tiled GX I8 image size, as written by THPVideoDecode.
std::size_t thpI8TextureBytes(std::uint32_t width, std::uint32_t height);
// Buffer sizes allocated by THPSimplePlayerWrapper::setBuffer.
std::size_t thpWrapperLumaBytes(const ThpVideoInfo& video);
std::size_t thpWrapperChromaBytes(const ThpVideoInfo& video);

// Checks one video component: JPEG markers, frame dimensions against the
// stream's video info, table references, and that the entropy-coded scan is
// complete within `size`. The walk consumes bits exactly as THPVideoDecode
// does, so a validated component cannot make the decoder read past `size`.
const char* validateThpVideoComponent(const void* data, std::size_t size, std::uint32_t width, std::uint32_t height);

// Checks one audio track record: sample count against the stream maximum and
// the ADPCM bytes of each channel against `size`.
const char* validateThpAudioComponent(const void* data, std::size_t size, const ThpAudioInfo& audio,
                                      std::uint32_t audioMaxSamples, std::uint32_t* samples);

// ADPCM bytes read by THPAudioDecode for one channel of `samples` samples.
std::size_t thpAdpcmChannelBytes(std::uint32_t samples);

}  // namespace PetariNative::Movie

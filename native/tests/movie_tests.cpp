// THP movie component tests (native/movie, aurora::thp).
//
// Default: synthetic container/frame/audio records, malformed-input rejection, and
// an independent ADPCM reference check of THPAudioDecode's PCM layout. With the
// extracted disc present (or --movies DIR): every frame of every MovieData/*.thp is
// walked through the container, frame, JPEG and ADPCM validators, and the audio of
// every frame is decoded. Video frames are decoded for a bounded sample, or for all
// frames with --all-video. Components are copied to exact-size heap blocks and
// decoded into exact-size textures, so AddressSanitizer reports any read or write
// beyond the validated bytes. When ffmpeg is on PATH, decoded PCM and the first
// frames' Y/U/V planes are compared with ffmpeg's independent THP decoders.
// A byte-mutation pass checks that every mutated video component either fails
// validation or decodes within bounds.
//
// Links: petari_movie, aurora::thp.
#include <petari/endian.hpp>
#include <petari/movie_thp.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
// aurora::thp. The Petari SDK header declares the same C symbols without const.
std::int32_t THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work);
std::uint32_t THPAudioDecode(std::int16_t* audioBuffer, const std::uint8_t* audioFrame, std::int32_t flag);
int THPInit(void);
}

namespace Movie = PetariNative::Movie;
using Bytes = std::vector<std::uint8_t>;

namespace {

int sFailures = 0;

void check(bool condition, const char* text, const char* detail = nullptr) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s%s%s\n", text, detail ? ": " : "", detail ? detail : "");
        ++sFailures;
    }
}

void put32(Bytes& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
    }
}

void put16(Bytes& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value >> 8);
    data[offset + 1] = static_cast<std::uint8_t>(value);
}

// Exact-size heap copy, so ASan reports any access past `size`.
struct Exact {
    explicit Exact(const std::uint8_t* data, std::size_t size) : bytes(new std::uint8_t[size]), size(size) {
        std::memcpy(bytes.get(), data, size);
    }
    std::unique_ptr<std::uint8_t[]> bytes;
    std::size_t size;
};

// ---------------------------------------------------------------------------
// Synthetic records

Bytes makeHeader() {
    Bytes h(0x30);
    std::memcpy(h.data(), "THP\0", 4);
    put32(h, 0x04, Movie::kThpVersion11);
    put32(h, 0x08, 0x1000);        // bufSize
    put32(h, 0x0C, 28);            // audioMaxSamples
    put32(h, 0x10, 0x426FC28F);    // 59.94f
    put32(h, 0x14, 2);             // numFrames
    put32(h, 0x18, 0x100);         // firstFrameSize
    put32(h, 0x1C, 0x200);         // movieDataSize
    put32(h, 0x20, 0x30);          // compInfoDataOffsets
    put32(h, 0x24, 0);             // offsetDataOffsets
    put32(h, 0x28, 0x60);          // movieDataOffsets
    put32(h, 0x2C, 0x160);         // finalFrameDataOffsets
    return h;
}

Bytes makeComponents(std::uint32_t channels = 2, std::uint32_t tracks = 1) {
    Bytes c(0x14 + 12 + 16, 0xFF);
    put32(c, 0, 2);
    c[4] = Movie::kThpComponentVideo;
    c[5] = Movie::kThpComponentAudio;
    put32(c, 0x14, 32);
    put32(c, 0x18, 16);
    put32(c, 0x1C, 0);
    put32(c, 0x20, channels);
    put32(c, 0x24, 32000);
    put32(c, 0x28, 56);
    put32(c, 0x2C, tracks);
    return c;
}

void testHeaderAndComponents() {
    Movie::ThpHeader header{};
    Bytes h = makeHeader();
    check(Movie::parseThpHeader(h.data(), h.size(), &header) == nullptr, "synthetic header parses");
    check(header.bufSize == 0x1000 && header.numFrames == 2 && header.movieDataOffsets == 0x60,
          "header fields are read big-endian");
    check(std::fabs(header.frameRate - 59.94f) < 1e-4f, "header frame rate is read big-endian");
    check(Movie::parseThpHeader(h.data(), h.size() - 1, &header) != nullptr, "truncated header is rejected");
    Bytes bad = h;
    bad[2] = 'X';
    check(Movie::parseThpHeader(bad.data(), bad.size(), &header) != nullptr, "bad magic is rejected");
    bad = h;
    put32(bad, 4, 0x10000);
    check(Movie::parseThpHeader(bad.data(), bad.size(), &header) != nullptr, "THP 1.0 is rejected");

    Movie::ThpComponents components{};
    Bytes c = makeComponents();
    check(Movie::parseThpComponents(c.data(), c.size(), &components) == nullptr, "synthetic components parse");
    check(components.hasVideo && components.hasAudio && components.video.xSize == 32 &&
              components.video.ySize == 16 && components.audio.sndNumTracks == 1 && components.byteSize == 0x30,
          "component info is read big-endian");
    check(Movie::parseThpComponents(c.data(), c.size() - 1, &components) != nullptr,
          "truncated audio info is rejected");
    bad = c;
    put32(bad, 0, 0);
    check(Movie::parseThpComponents(bad.data(), bad.size(), &components) != nullptr, "zero components rejected");
    put32(bad, 0, 17);
    check(Movie::parseThpComponents(bad.data(), bad.size(), &components) != nullptr, "17 components rejected");
    bad = c;
    bad[5] = 2;
    check(Movie::parseThpComponents(bad.data(), bad.size(), &components) != nullptr, "unknown kind rejected");
    bad = c;
    bad[5] = Movie::kThpComponentVideo;
    check(Movie::parseThpComponents(bad.data(), bad.size(), &components) != nullptr, "two video comps rejected");

    Movie::parseThpHeader(h.data(), h.size(), &header);
    Movie::parseThpComponents(c.data(), c.size(), &components);
    check(Movie::validateThpStream(header, components, 0x260) == nullptr, "synthetic stream validates");
    check(Movie::validateThpStream(header, components, 0x25F) != nullptr, "short file is rejected");
    Movie::ThpHeader h2 = header;
    h2.firstFrameSize = 0x1001;
    check(Movie::validateThpStream(h2, components, 0x260) != nullptr, "first frame above bufSize rejected");
    h2 = header;
    h2.movieDataOffsets = 0x50;
    check(Movie::validateThpStream(h2, components, 0x260) != nullptr, "movie data overlapping table rejected");
    h2 = header;
    h2.numFrames = 0;
    check(Movie::validateThpStream(h2, components, 0x260) != nullptr, "empty movie rejected");
    h2 = header;
    h2.frameRate = NAN;
    check(Movie::validateThpStream(h2, components, 0x260) != nullptr, "NaN frame rate rejected");
    Movie::ThpComponents c2 = components;
    c2.video.xSize = 2048;
    check(Movie::validateThpStream(header, c2, 0x260) != nullptr, "2048-wide video rejected");
    c2 = components;
    c2.video.xSize = 12;
    c2.video.ySize = 6;
    check(Movie::validateThpStream(header, c2, 0x260) != nullptr,
          "dimensions whose tiled textures exceed the player's buffers are rejected");
    c2 = components;
    c2.audio.sndChannels = 3;
    check(Movie::validateThpStream(header, c2, 0x260) != nullptr, "3-channel audio rejected");

    check(Movie::checkThpFrameRead(header, components, 0x260, 0x60, 0x100) == nullptr, "frame read accepted");
    check(Movie::checkThpFrameRead(header, components, 0x260, 0x62, 0x100) != nullptr, "unaligned read rejected");
    check(Movie::checkThpFrameRead(header, components, 0x260, 0x160, 0x104) != nullptr,
          "read past the movie data rejected");
    check(Movie::checkThpFrameRead(header, components, 0x260, 0x60, 4) != nullptr, "read below header rejected");

    // Texture sizes for the retail 640x368 movies and an odd size.
    check(Movie::thpI8TextureBytes(640, 368) == 640 * 368, "640x368 luma tiles exactly");
    check(Movie::thpI8TextureBytes(320, 184) == 320 * 184, "320x184 chroma tiles exactly");
    check(Movie::thpI8TextureBytes(12, 6) == 2 * 2 * 32, "partial tiles round up");
}

void testFrames() {
    Movie::ThpComponents components{};
    Bytes c = makeComponents(2, 2);
    Movie::parseThpComponents(c.data(), c.size(), &components);
    Bytes frame(0x40, 0);
    put32(frame, 0, 0x80);
    put32(frame, 4, 0x40);
    put32(frame, 8, 0x10);
    put32(frame, 12, 0x10);
    Movie::ThpFrame parsed{};
    // Header 16 bytes + video 16 + two 16-byte audio tracks = 64 bytes.
    check(Movie::parseThpFrame(frame.data(), frame.size(), components, &parsed) == nullptr, "frame parses");
    check(parsed.nextFrameSize == 0x80 && parsed.prevFrameSize == 0x40 && parsed.componentOffsets[0] == 16 &&
              parsed.componentOffsets[1] == 32 && parsed.componentSizes[1] == 0x10,
          "frame sizes and offsets are big-endian");
    check(Movie::parseThpFrame(frame.data(), 0x3F, components, &parsed) != nullptr,
          "second audio track past the frame is rejected");
    put32(frame, 8, 0);
    check(Movie::parseThpFrame(frame.data(), frame.size(), components, &parsed) != nullptr,
          "zero-size component is rejected");
    put32(frame, 8, 0xFFFFFFF0u);
    check(Movie::parseThpFrame(frame.data(), frame.size(), components, &parsed) != nullptr,
          "overflowing component size is rejected");
    check(Movie::parseThpFrame(frame.data(), 15, components, &parsed) != nullptr, "truncated frame header rejected");
}

// ---------------------------------------------------------------------------
// ADPCM reference. Written from the DSP ADPCM definition, independent of the
// decoder under test: 8-byte frames of a predictor/scale byte and 14 nibbles.

// `truncate` selects ffmpeg's adpcm_thp arithmetic ((acc + 0) >> 11, clamped)
// instead of the SDK's rounding ((acc * 32 + 0x8000) >> 16, 32-bit saturated).
void referenceChannel(const std::uint8_t* data, std::uint32_t samples, const std::int16_t coef[8][2],
                      std::int16_t yn1, std::int16_t yn2, std::vector<std::int16_t>& out, bool truncate = false) {
    for (std::uint32_t i = 0; i < samples; i++) {
        const std::uint8_t* frame = data + (i / 14) * 8;
        const unsigned k = i % 14;
        const unsigned predictor = (frame[0] >> 4) & 7;
        const unsigned scale = frame[0] & 15;
        const std::uint8_t byte = frame[1 + k / 2];
        int nibble = (k & 1) ? (byte & 15) : (byte >> 4);
        if (nibble >= 8) {
            nibble -= 16;
        }
        std::int64_t acc = std::int64_t{coef[predictor][0]} * yn1 + std::int64_t{coef[predictor][1]} * yn2 +
                           std::int64_t{nibble} * (std::int64_t{1} << scale) * 2048;
        if (truncate) {
            acc = std::clamp<std::int64_t>(acc >> 11, INT16_MIN, INT16_MAX);
        } else {
            acc = std::clamp<std::int64_t>(acc * 32 + 0x8000, INT32_MIN, INT32_MAX) >> 16;
        }
        const auto value = static_cast<std::int16_t>(acc);
        out.push_back(value);
        yn2 = yn1;
        yn1 = value;
    }
}

struct AudioRecord {
    std::uint32_t samples;
    std::vector<std::int16_t> left;
    std::vector<std::int16_t> right;
};

AudioRecord referenceRecord(const std::uint8_t* record, bool truncate = false) {
    AudioRecord r{};
    const std::uint32_t next = PetariNative::readU32BE(record);
    r.samples = PetariNative::readU32BE(record + 4);
    std::int16_t lc[8][2], rc[8][2];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 2; j++) {
            lc[i][j] = static_cast<std::int16_t>(PetariNative::readU16BE(record + 8 + (i * 2 + j) * 2));
            rc[i][j] = static_cast<std::int16_t>(PetariNative::readU16BE(record + 40 + (i * 2 + j) * 2));
        }
    }
    const auto s16 = [&](std::size_t o) { return static_cast<std::int16_t>(PetariNative::readU16BE(record + o)); };
    referenceChannel(record + 80, r.samples, lc, s16(72), s16(74), r.left, truncate);
    if (next == 0) {
        r.right = r.left;
    } else {
        referenceChannel(record + 80 + next, r.samples, rc, s16(76), s16(78), r.right, truncate);
    }
    return r;
}

void testAudioSynthetic() {
    check(Movie::thpAdpcmChannelBytes(0) == 1, "empty channel reads its header byte");
    check(Movie::thpAdpcmChannelBytes(1) == 2, "one sample reads two bytes");
    check(Movie::thpAdpcmChannelBytes(14) == 8, "one full frame is eight bytes");
    check(Movie::thpAdpcmChannelBytes(15) == 10, "fifteen samples read ten bytes");
    check(Movie::thpAdpcmChannelBytes(546) == 39 * 8, "546 samples read 39 frames");

    // Two channels, 20 samples each, varied predictors and scales.
    const std::uint32_t samples = 20;
    const std::size_t channelBytes = Movie::thpAdpcmChannelBytes(samples);
    Bytes record(80 + 2 * 16, 0);
    put32(record, 0, 16);
    put32(record, 4, samples);
    std::mt19937 rng(7);
    for (int i = 0; i < 16; i++) {
        put16(record, 8 + i * 2, static_cast<std::uint16_t>(rng() & 0x0FFF));
        put16(record, 40 + i * 2, static_cast<std::uint16_t>(-(static_cast<int>(rng() & 0x07FF))));
    }
    put16(record, 72, 100);
    put16(record, 74, static_cast<std::uint16_t>(-50));
    put16(record, 76, static_cast<std::uint16_t>(-7));
    put16(record, 78, 3000);
    for (std::size_t i = 80; i < record.size(); i++) {
        record[i] = static_cast<std::uint8_t>(rng());
    }
    record[80] = 0x35;
    record[88] = 0x6B;
    record[96] = 0x12;
    record[104] = 0x7C;

    Movie::ThpAudioInfo info{2, 32000, samples, 1};
    std::uint32_t decoded = 0;
    check(Movie::validateThpAudioComponent(record.data(), record.size(), info, samples, &decoded) == nullptr &&
              decoded == samples,
          "synthetic audio record validates");
    check(Movie::validateThpAudioComponent(record.data(), 80 + 16 + channelBytes - 1, info, samples, &decoded) !=
              nullptr,
          "truncated right channel is rejected");
    check(Movie::validateThpAudioComponent(record.data(), record.size(), info, samples - 1, &decoded) != nullptr,
          "record above audioMaxSamples is rejected");
    check(Movie::validateThpAudioComponent(record.data(), 79, info, samples, &decoded) != nullptr,
          "truncated record header is rejected");
    Bytes empty = record;
    put32(empty, 4, 0);
    check(Movie::validateThpAudioComponent(empty.data(), empty.size(), info, samples, &decoded) != nullptr,
          "empty record is rejected");

    const AudioRecord ref = referenceRecord(record.data());
    Exact input(record.data(), 80 + 16 + channelBytes);
    std::vector<std::int16_t> interleaved(samples * 2);
    check(THPAudioDecode(interleaved.data(), input.bytes.get(), 0) == samples, "interleaved decode sample count");
    bool rightLeft = true;
    for (std::uint32_t i = 0; i < samples; i++) {
        rightLeft = rightLeft && interleaved[i * 2] == ref.right[i] && interleaved[i * 2 + 1] == ref.left[i];
    }
    check(rightLeft, "flag 0 writes interleaved right,left pairs matching the ADPCM reference");
    std::vector<std::int16_t> planar(samples * 2);
    check(THPAudioDecode(planar.data(), input.bytes.get(), 1) == samples, "planar decode sample count");
    check(std::equal(ref.right.begin(), ref.right.end(), planar.begin()) &&
              std::equal(ref.left.begin(), ref.left.end(), planar.begin() + samples),
          "flag 1 writes the right plane then the left plane");

    Bytes mono = record;
    put32(mono, 0, 0);
    Exact monoInput(mono.data(), 80 + channelBytes);
    std::vector<std::int16_t> monoOut(samples * 2);
    THPAudioDecode(monoOut.data(), monoInput.bytes.get(), 0);
    const AudioRecord monoRef = referenceRecord(mono.data());
    bool duplicated = true;
    for (std::uint32_t i = 0; i < samples; i++) {
        duplicated = duplicated && monoOut[i * 2] == monoRef.left[i] && monoOut[i * 2 + 1] == monoRef.left[i];
    }
    check(duplicated, "a record without a second channel duplicates the left channel");
}

// ---------------------------------------------------------------------------
// Real movies

struct Mapped {
    explicit Mapped(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return;
        }
        struct stat st {};
        if (::fstat(fd, &st) == 0 && st.st_size > 0) {
            void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                data = static_cast<const std::uint8_t*>(p);
                size = static_cast<std::size_t>(st.st_size);
            }
        }
        ::close(fd);
    }
    ~Mapped() {
        if (data) {
            ::munmap(const_cast<std::uint8_t*>(data), size);
        }
    }
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct Options {
    std::string movies;
    std::string only;
    std::uint32_t videoFrames = 24;
    std::uint32_t videoStride = 500;
    bool allVideo = false;
    bool ffmpeg = true;
    std::uint32_t reference = 8;
    std::uint32_t mutations = 3000;
    std::uint32_t bench = 0;
    std::uint32_t frameLimit = 0;  // 0 = every frame
};

struct VideoPlanes {
    std::vector<std::uint8_t> y, u, v;
};

// Converts a GX I8 tiled image (8x4 tiles) to rows.
std::vector<std::uint8_t> untile(const std::uint8_t* tiled, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> out(std::size_t{width} * height);
    const std::uint32_t tilesPerRow = (width + 7) / 8;
    for (std::uint32_t y = 0; y < height; y++) {
        for (std::uint32_t x = 0; x < width; x++) {
            const std::size_t tile = std::size_t{y / 4} * tilesPerRow + x / 8;
            out[std::size_t{y} * width + x] = tiled[tile * 32 + (y & 3) * 8 + (x & 7)];
        }
    }
    return out;
}

bool decodeVideo(const std::uint8_t* component, std::size_t size, const Movie::ThpVideoInfo& video,
                 VideoPlanes* planes, std::int32_t* result) {
    Exact input(component, size);
    const std::uint32_t cw = (video.xSize + 1) / 2;
    const std::uint32_t ch = (video.ySize + 1) / 2;
    auto y = std::make_unique<std::uint8_t[]>(Movie::thpI8TextureBytes(video.xSize, video.ySize));
    auto u = std::make_unique<std::uint8_t[]>(Movie::thpI8TextureBytes(cw, ch));
    auto v = std::make_unique<std::uint8_t[]>(Movie::thpI8TextureBytes(cw, ch));
    *result = THPVideoDecode(input.bytes.get(), y.get(), u.get(), v.get(), nullptr);
    if (planes != nullptr) {
        planes->y = untile(y.get(), video.xSize, video.ySize);
        planes->u = untile(u.get(), cw, ch);
        planes->v = untile(v.get(), cw, ch);
    }
    return *result == 0;
}

// ffmpeg's THP demuxer and decoders are an implementation independent of Aurora.
bool runFfmpeg(const std::string& args, std::vector<std::uint8_t>& out) {
    const std::string command = "ffmpeg -nostdin -v error " + args;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    std::uint8_t buffer[1 << 16];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) != 0) {
        out.insert(out.end(), buffer, buffer + n);
    }
    return ::pclose(pipe) == 0;
}

double psnr(const std::vector<std::uint8_t>& a, const std::uint8_t* b, std::size_t n, int* maxDiff) {
    double sum = 0;
    *maxDiff = 0;
    for (std::size_t i = 0; i < n; i++) {
        const int d = int{a[i]} - int{b[i]};
        sum += double(d) * d;
        *maxDiff = std::max(*maxDiff, std::abs(d));
    }
    const double mse = sum / double(n);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void mutateVideo(const std::uint8_t* component, std::size_t size, const Movie::ThpVideoInfo& video,
                 std::uint32_t iterations, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uint32_t accepted = 0;
    std::uint32_t decodeFailures = 0;
    Bytes work(component, component + size);
    for (std::uint32_t i = 0; i < iterations; i++) {
        std::copy(component, component + size, work.begin());
        std::size_t length = size;
        switch (rng() % 4) {
        case 0:  // header bytes
            for (int k = 0, n = 1 + rng() % 3; k < n; k++) {
                work[rng() % std::min<std::size_t>(size, 0x260)] = static_cast<std::uint8_t>(rng());
            }
            break;
        case 1:  // scan bytes
            for (int k = 0, n = 1 + rng() % 8; k < n; k++) {
                work[rng() % size] = static_cast<std::uint8_t>(rng());
            }
            break;
        case 2:  // truncation
            length = rng() % size;
            break;
        default:  // truncation plus corruption near the new end
            length = 1 + rng() % size;
            work[length - 1] = static_cast<std::uint8_t>(rng());
            break;
        }
        if (Movie::validateThpVideoComponent(work.data(), length, video.xSize, video.ySize) != nullptr) {
            continue;
        }
        accepted++;
        std::int32_t result;
        if (!decodeVideo(work.data(), length, video, nullptr, &result)) {
            decodeFailures++;
        }
    }
    std::printf("    mutations: %u, accepted and decoded in bounds: %u (decoder errors %u)\n", iterations, accepted,
                decodeFailures);
}

// Per-frame cost of validation and decoding on the game thread (use a build
// without sanitizers). Decodes into reused buffers from the mapped file.
void benchMovie(const std::string& path, std::uint32_t frames) {
    Mapped file(path);
    Movie::ThpHeader header{};
    Movie::ThpComponents components{};
    if (file.data == nullptr || Movie::parseThpHeader(file.data, file.size, &header) != nullptr ||
        Movie::parseThpComponents(file.data + header.compInfoDataOffsets, file.size - header.compInfoDataOffsets,
                                  &components) != nullptr) {
        check(false, "bench movie loads", path.c_str());
        return;
    }
    const Movie::ThpVideoInfo& video = components.video;
    std::vector<std::uint8_t> y(Movie::thpI8TextureBytes(video.xSize, video.ySize));
    std::vector<std::uint8_t> u(Movie::thpI8TextureBytes((video.xSize + 1) / 2, (video.ySize + 1) / 2)), v(u.size());
    std::vector<std::int16_t> pcm(std::size_t{header.audioMaxSamples} * 2);
    using Clock = std::chrono::steady_clock;
    Clock::duration validate{}, decodeVideo{}, decodeAudio{};
    std::uint32_t offset = header.movieDataOffsets, size = header.firstFrameSize, count = 0;
    for (; count < std::min(frames, header.numFrames); count++) {
        const std::uint8_t* frameData = file.data + offset;
        Movie::ThpFrame frame{};
        const auto t0 = Clock::now();
        bool ok = Movie::parseThpFrame(frameData, size, components, &frame) == nullptr;
        for (std::uint32_t c = 0; ok && c < components.numComponents; c++) {
            const std::uint8_t* comp = frameData + frame.componentOffsets[c];
            std::uint32_t samples;
            ok = (components.kinds[c] == Movie::kThpComponentVideo
                      ? Movie::validateThpVideoComponent(comp, frame.componentSizes[c], video.xSize, video.ySize)
                      : Movie::validateThpAudioComponent(comp, frame.componentSizes[c], components.audio,
                                                         header.audioMaxSamples, &samples)) == nullptr;
        }
        const auto t1 = Clock::now();
        check(ok, "bench frame validates");
        for (std::uint32_t c = 0; c < components.numComponents; c++) {
            const std::uint8_t* comp = frameData + frame.componentOffsets[c];
            const auto a = Clock::now();
            if (components.kinds[c] == Movie::kThpComponentVideo) {
                THPVideoDecode(comp, y.data(), u.data(), v.data(), nullptr);
                decodeVideo += Clock::now() - a;
            } else {
                THPAudioDecode(pcm.data(), comp, 0);
                decodeAudio += Clock::now() - a;
            }
        }
        validate += t1 - t0;
        offset += size;
        size = frame.nextFrameSize;
    }
    const auto ms = [&](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count() / count; };
    std::printf("  bench %s: %u frames, per frame: validate %.3f ms, video decode %.3f ms, audio decode %.3f ms\n",
                std::filesystem::path(path).filename().c_str(), count, ms(validate), ms(decodeVideo), ms(decodeAudio));
}

void testMovie(const std::string& path, const Options& options) {
    const std::string name = std::filesystem::path(path).filename().string();
    Mapped file(path);
    check(file.data != nullptr, "movie maps", name.c_str());
    if (file.data == nullptr) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    Movie::ThpHeader header{};
    Movie::ThpComponents components{};
    const char* error = Movie::parseThpHeader(file.data, file.size, &header);
    check(error == nullptr, "movie header parses", error);
    if (error) {
        return;
    }
    error = Movie::parseThpComponents(file.data + header.compInfoDataOffsets,
                                      file.size - header.compInfoDataOffsets, &components);
    check(error == nullptr, "movie components parse", error);
    if (error) {
        return;
    }
    error = Movie::validateThpStream(header, components, file.size);
    check(error == nullptr, "movie stream validates", error);
    if (error) {
        return;
    }
    const Movie::ThpVideoInfo& video = components.video;
    std::printf("  %s: %ux%u %.2f fps, %u frames, buf 0x%X, audio %s %u ch %u Hz %u tracks max %u\n", name.c_str(),
                video.xSize, video.ySize, header.frameRate, header.numFrames, header.bufSize,
                components.hasAudio ? "yes" : "no", components.audio.sndChannels, components.audio.sndFrequency,
                components.audio.sndNumTracks, header.audioMaxSamples);

    // PCM in L,R order for comparison with ffmpeg.
    std::vector<std::int16_t> pcm, pcmTruncated;
    pcm.reserve(std::size_t{components.audio.sndNumSamples} * 2);
    std::vector<VideoPlanes> firstFrames;
    std::uint64_t totalSamples = 0;
    std::uint32_t firstRecordSamples = 0;
    std::uint32_t maxSamples = 0, maxFrame = 0, videoDecoded = 0, pcmMismatch = 0;
    std::uint32_t offset = header.movieDataOffsets;
    std::uint32_t size = header.firstFrameSize;
    std::uint32_t previousSize = 0;
    std::uint32_t lastOffset = 0;
    bool frameErrors = false;
    const std::uint32_t frames = options.frameLimit != 0 ? std::min(options.frameLimit, header.numFrames) : header.numFrames;
    const bool complete = frames == header.numFrames;
    for (std::uint32_t index = 0; index < frames && !frameErrors; index++) {
        char where[96];
        std::snprintf(where, sizeof(where), "%s frame %u", name.c_str(), index);
        if ((error = Movie::checkThpFrameRead(header, components, file.size, offset, size)) != nullptr) {
            check(false, error, where);
            break;
        }
        const std::uint8_t* frameData = file.data + offset;
        Movie::ThpFrame frame{};
        if ((error = Movie::parseThpFrame(frameData, size, components, &frame)) != nullptr) {
            check(false, error, where);
            break;
        }
        if (index != 0 && frame.prevFrameSize != previousSize) {
            check(false, "previous frame size field matches the previous frame", where);
        }
        maxFrame = std::max(maxFrame, size);
        for (std::uint32_t c = 0; c < components.numComponents; c++) {
            const std::uint8_t* comp = frameData + frame.componentOffsets[c];
            const std::uint32_t compSize = frame.componentSizes[c];
            if (components.kinds[c] == Movie::kThpComponentVideo) {
                if ((error = Movie::validateThpVideoComponent(comp, compSize, video.xSize, video.ySize)) != nullptr) {
                    check(false, error, where);
                    frameErrors = true;
                    break;
                }
                const bool sampled = index < options.videoFrames || index % options.videoStride == 0 ||
                                     index + 1 == header.numFrames;
                if (options.allVideo || sampled) {
                    std::int32_t result;
                    VideoPlanes planes;
                    const bool keep = index < options.reference;
                    decodeVideo(comp, compSize, video, keep ? &planes : nullptr, &result);
                    check(result == 0, "validated video frame decodes", where);
                    videoDecoded++;
                    if (keep) {
                        firstFrames.push_back(std::move(planes));
                    }
                }
                if (index == 0 && options.mutations != 0) {
                    mutateVideo(comp, compSize, video, options.mutations, 1234);
                }
            } else {
                for (std::uint32_t track = 0; track < components.audio.sndNumTracks; track++) {
                    const std::uint8_t* record = comp + std::size_t{compSize} * track;
                    std::uint32_t samples = 0;
                    if ((error = Movie::validateThpAudioComponent(record, compSize, components.audio,
                                                                  header.audioMaxSamples, &samples)) != nullptr) {
                        check(false, error, where);
                        frameErrors = true;
                        break;
                    }
                    // The player's audio buffer holds audioMaxSamples stereo frames.
                    Exact input(record, compSize);
                    auto out = std::make_unique<std::int16_t[]>(std::size_t{header.audioMaxSamples} * 2);
                    const std::uint32_t decoded = THPAudioDecode(out.get(), input.bytes.get(), 0);
                    if (decoded != samples) {
                        check(false, "audio decode returns the record's sample count", where);
                    }
                    if (track == 0) {
                        const AudioRecord ref = referenceRecord(record);
                        if (options.ffmpeg && index == 0) {
                            const AudioRecord truncated = referenceRecord(record, true);
                            for (std::uint32_t i = 0; i < truncated.samples; i++) {
                                pcmTruncated.push_back(truncated.left[i]);
                                pcmTruncated.push_back(truncated.right[i]);
                            }
                        }
                        for (std::uint32_t i = 0; i < decoded; i++) {
                            if (out[i * 2] != ref.right[i] || out[i * 2 + 1] != ref.left[i]) {
                                pcmMismatch++;
                            }
                            pcm.push_back(out[i * 2 + 1]);
                            pcm.push_back(out[i * 2]);
                        }
                        if (index == 0) {
                            firstRecordSamples = decoded;
                        }
                        totalSamples += decoded;
                        maxSamples = std::max(maxSamples, decoded);
                    }
                }
            }
        }
        previousSize = size;
        lastOffset = offset;
        offset += size;
        size = frame.nextFrameSize;
    }
    if (frameErrors) {
        return;
    }
    check(pcmMismatch == 0, "THPAudioDecode matches the ADPCM reference for every sample", name.c_str());
    if (complete) {
        check(lastOffset == header.finalFrameDataOffsets, "last frame starts at finalFrameDataOffsets", name.c_str());
        check(offset == header.movieDataOffsets + header.movieDataSize, "frames exactly cover the movie data",
              name.c_str());
    }
    if (components.hasAudio && complete) {
        check(totalSamples == components.audio.sndNumSamples, "audio frames sum to sndNumSamples", name.c_str());
        check(maxSamples <= header.audioMaxSamples, "no audio frame exceeds audioMaxSamples", name.c_str());
    }
    check(maxFrame <= header.bufSize, "no frame exceeds bufSize", name.c_str());
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("    frames validated: %u of %u, largest 0x%X; video decoded: %u; audio samples: %llu (max %u/frame); "
                "%.1f s\n",
                frames, header.numFrames, maxFrame, videoDecoded, static_cast<unsigned long long>(totalSamples), maxSamples,
                seconds);

    if (!options.ffmpeg) {
        return;
    }
    std::vector<std::uint8_t> ffAudio;
    if (!runFfmpeg("-i '" + path + "' -map 0:a:0 -f s16le -acodec pcm_s16le -", ffAudio)) {
        check(false, "ffmpeg decodes the movie audio", name.c_str());
    } else {
        // ffmpeg's adpcm_thp truncates where the SDK rounds, and it carries its own
        // filter history across packets instead of loading each record's stored
        // history, so it diverges from the SDK after the first record. It is an
        // independent check of nibble order, predictors, coefficients and channel
        // order on record 0 only, compared with the truncating reference. Every
        // record's THPAudioDecode output was compared with the SDK-rounding
        // reference above.
        const std::size_t ffSamples = ffAudio.size() / 2;
        const std::size_t n = std::min({ffSamples, pcmTruncated.size(), std::size_t{firstRecordSamples} * 2});
        std::size_t exact = 0;
        for (std::size_t i = 0; i < n; i++) {
            std::int16_t ff;
            std::memcpy(&ff, ffAudio.data() + i * 2, 2);
            exact += ff == pcmTruncated[i];
        }
        std::printf("    ffmpeg audio: %zu vs %zu interleaved samples; record 0: %zu/%zu identical to the "
                    "truncating reference\n",
                    ffSamples, pcm.size(), exact, n);
        if (complete) {
            check(ffSamples == pcm.size(), "ffmpeg and THPAudioDecode produce the same sample count", name.c_str());
        }
        check(n != 0 && exact == n, "ffmpeg's record-0 L,R PCM equals the reference decode", name.c_str());
    }
    if (firstFrames.empty()) {
        return;
    }
    std::vector<std::uint8_t> ffVideo;
    const std::size_t frameBytes = std::size_t{video.xSize} * video.ySize * 3 / 2;
    if (!runFfmpeg("-i '" + path + "' -map 0:v:0 -frames:v " + std::to_string(firstFrames.size()) +
                       " -f rawvideo -pix_fmt yuv420p -",
                   ffVideo) ||
        ffVideo.size() != frameBytes * firstFrames.size()) {
        check(false, "ffmpeg decodes the first video frames", name.c_str());
        return;
    }
    double worst = 99.0;
    int worstMax = 0;
    const std::size_t ySize = std::size_t{video.xSize} * video.ySize;
    for (std::size_t i = 0; i < firstFrames.size(); i++) {
        const std::uint8_t* ref = ffVideo.data() + i * frameBytes;
        int maxY, maxU, maxV;
        const double py = psnr(firstFrames[i].y, ref, ySize, &maxY);
        const double pu = psnr(firstFrames[i].u, ref + ySize, ySize / 4, &maxU);
        const double pv = psnr(firstFrames[i].v, ref + ySize + ySize / 4, ySize / 4, &maxV);
        worst = std::min({worst, py, pu, pv});
        worstMax = std::max({worstMax, maxY, maxU, maxV});
    }
    std::printf("    ffmpeg video: %zu frames, worst plane PSNR %.1f dB, max texel diff %d\n", firstFrames.size(),
                worst, worstMax);
    check(worst >= 40.0, "decoded Y/U/V planes match ffmpeg within IDCT rounding (PSNR >= 40 dB)", name.c_str());
}

std::string defaultMovies() {
    std::vector<std::string> roots;
    if (const char* env = std::getenv("PETARI_SOURCE_DIR")) {
        roots.emplace_back(env);
    }
    roots.emplace_back(".");
    for (const std::string& root : roots) {
        const std::string candidate = root + "/build/game-data/RMGE01/files/MovieData";
        if (std::filesystem::is_directory(candidate)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    options.movies = defaultMovies();
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--movies" && i + 1 < argc) {
            options.movies = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            options.only = argv[++i];
        } else if (arg == "--video-frames" && i + 1 < argc) {
            options.videoFrames = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--all-video") {
            options.allVideo = true;
        } else if (arg == "--no-ffmpeg") {
            options.ffmpeg = false;
        } else if (arg == "--mutations" && i + 1 < argc) {
            options.mutations = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--frame-limit" && i + 1 < argc) {
            options.frameLimit = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--bench" && i + 1 < argc) {
            options.bench = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--synthetic") {
            options.movies.clear();
        } else {
            std::fprintf(stderr,
                         "usage: %s [--movies DIR] [--only NAME] [--video-frames N] [--all-video] [--no-ffmpeg] "
                         "[--mutations N] [--frame-limit N] [--bench FRAMES] [--synthetic]\n",
                         argv[0]);
            return 2;
        }
    }
    if (options.ffmpeg && std::system("ffmpeg -version > /dev/null 2>&1") != 0) {
        std::printf("ffmpeg not found; independent reference comparisons are not run\n");
        options.ffmpeg = false;
    }

    THPInit();
    testHeaderAndComponents();
    testFrames();
    testAudioSynthetic();
    std::printf("synthetic THP tests done\n");

    if (options.movies.empty()) {
        std::printf("MovieData not found; real-movie tests were not run\n");
    } else {
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::directory_iterator(options.movies)) {
            if (entry.path().extension() == ".thp" &&
                (options.only.empty() || entry.path().filename().string().find(options.only) != std::string::npos)) {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        check(!files.empty(), "MovieData contains THP files");
        std::printf("real movies: %zu files in %s\n", files.size(), options.movies.c_str());
        for (const std::string& file : files) {
            if (options.bench != 0) {
                benchMovie(file, options.bench);
            } else {
                testMovie(file, options);
            }
        }
    }
    if (sFailures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", sFailures);
        return 1;
    }
    std::printf("movie tests passed\n");
    return 0;
}

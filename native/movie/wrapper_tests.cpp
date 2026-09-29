// Integration test of the game's THPSimplePlayerWrapper on the native platform.
//
// Runs the original wrapper (src/Game/Screen/THPSimplePlayerWrapper.cpp, native
// path) with the real native DVD service and OS interrupt lock, the real
// Nerve/Spine executor, petari_movie validation and aurora::thp decoding, driven
// the way MoviePlayerSimple drives it: open, calcNeedMemory, setBuffer, preLoad,
// then decode/drawCurrentFrame per frame and the JAS interleave mix callback for
// audio. Every decoded texture set is compared with an independent decode of the
// same frame read directly from the host file, and every mixed PCM sample with
// the frame's decoded samples (volume 127 is a unity gain).
//
// Test doubles, not game code: MR::zeroMemory (memset), JKR operator new[] with
// alignment (host aligned allocation), JASDriver::registerMixCallback (records the
// callback), SCGetSoundMode, and the three THPGX draw entry points (record their
// arguments; the GX draw itself needs the Aurora backend and is not run here).
//
// Malformed movies run in forked children, each mounting its own temporary disc,
// and must end in the wrapper's OSPanic with the expected reason.
//
// --os runs the playbacks on an initialized native OS, the test thread as the OS
// thread holding the CPU (as the game thread is), and checks that a ready
// lower-priority OS thread makes progress during frame decodes, which it can only
// do while the decoding thread has given up the CPU.
#include "Game/Screen/THPSimplePlayerWrapper.hpp"

#include <JSystem/JAudio2/JASAiCtrl.hpp>
#include <petari/endian.hpp>
#include <petari/movie_thp.hpp>
#include <petari/platform/dvd.hpp>
#include <revolution/dvd.h>
#include <revolution/os.h>
#include <revolution/sc.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Movie = PetariNative::Movie;
namespace PDVD = PetariNative::Platform::DVD;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Test doubles

namespace MR {
void zeroMemory(void* pDst, u32 size) {
    std::memset(pDst, 0, size);
}
}  // namespace MR

void* operator new[](std::size_t size, int alignment) {
    const std::size_t align = alignment > 0 ? static_cast<std::size_t>(alignment) : sizeof(void*);
    return std::aligned_alloc(align, (size + align - 1) / align * align);
}

namespace {
JASDriver::MixCallback sRecordedMixCallback;
JASMixMode sRecordedMixMode;
u8 sSoundMode = SC_SOUND_MODE_STEREO;

struct DrawCall {
    u8* y;
    u8* u;
    u8* v;
    s16 x, yPos, width, height, polyW, polyH;
    int setups;
    int restores;
};
DrawCall sDraw;
}  // namespace

void JASDriver::registerMixCallback(MixCallback callback, JASMixMode mode) {
    sRecordedMixCallback = callback;
    sRecordedMixMode = mode;
}

extern "C" {
u8 SCGetSoundMode(void) {
    return sSoundMode;
}
void THPGXYuv2RgbSetup(GXRenderModeObj*) {
    sDraw.setups++;
}
void THPGXYuv2RgbDraw(u8* y, u8* u, u8* v, s16 x, s16 yPos, s16 width, s16 height, s16 polyW, s16 polyH) {
    sDraw.y = y;
    sDraw.u = u;
    sDraw.v = v;
    sDraw.x = x;
    sDraw.yPos = yPos;
    sDraw.width = width;
    sDraw.height = height;
    sDraw.polyW = polyW;
    sDraw.polyH = polyH;
}
void THPGXRestore(void) {
    sDraw.restores++;
}
}

// ---------------------------------------------------------------------------

namespace {

int sFailures = 0;

// --os: a priority-24 OS thread, ready throughout, counts while it holds the CPU. The
// priority-16 test thread keeps the CPU except when it gives it up, so the count can only
// advance during a decode that released the CPU. The count undercounts releases: when the
// host does not run the spinner's thread before the decode ends, the CPU returns to the
// test thread without the spinner having counted.
bool sOsMode = false;
std::atomic<bool> sSpinnerStop{false};
std::atomic<std::uint64_t> sSpins{0};
std::uint32_t sDecodedFrames = 0;
std::uint32_t sReleasingDecodes = 0;

void* spinner(void*) {
    while (!sSpinnerStop.load()) {
        sSpins.fetch_add(1);
        const BOOL enabled = OSDisableInterrupts();
        sSpins.fetch_add(1);
        OSRestoreInterrupts(enabled);
    }
    return nullptr;
}

void check(bool condition, const char* text, const std::string& detail = {}) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s%s%s\n", text, detail.empty() ? "" : ": ", detail.c_str());
        ++sFailures;
    }
}

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

// Host-side view of a movie used to compute expected results.
struct Reference {
    Movie::ThpHeader header{};
    Movie::ThpComponents components{};
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> sizes;
};

bool loadReference(const std::uint8_t* data, std::size_t size, Reference* ref) {
    if (Movie::parseThpHeader(data, size, &ref->header) != nullptr ||
        Movie::parseThpComponents(data + ref->header.compInfoDataOffsets, size - ref->header.compInfoDataOffsets,
                                  &ref->components) != nullptr) {
        return false;
    }
    std::uint32_t offset = ref->header.movieDataOffsets;
    std::uint32_t frameSize = ref->header.firstFrameSize;
    for (std::uint32_t i = 0; i < ref->header.numFrames; i++) {
        if (offset + std::uint64_t{frameSize} > size) {
            return false;
        }
        ref->offsets.push_back(offset);
        ref->sizes.push_back(frameSize);
        offset += frameSize;
        frameSize = PetariNative::readU32BE(data + ref->offsets.back());
    }
    return true;
}

struct ExpectedFrame {
    std::vector<std::uint8_t> y, u, v;
    std::vector<std::int16_t> pcm;  // R,L interleaved, as THPAudioDecode flag 0
};

ExpectedFrame expectedFrame(const std::uint8_t* file, const Reference& ref, std::uint32_t index) {
    ExpectedFrame e;
    const std::uint8_t* frame = file + ref.offsets[index];
    Movie::ThpFrame layout{};
    Movie::parseThpFrame(frame, ref.sizes[index], ref.components, &layout);
    const Movie::ThpVideoInfo& video = ref.components.video;
    e.y.resize(Movie::thpI8TextureBytes(video.xSize, video.ySize));
    e.u.resize(Movie::thpI8TextureBytes((video.xSize + 1) / 2, (video.ySize + 1) / 2));
    e.v.resize(e.u.size());
    for (std::uint32_t c = 0; c < ref.components.numComponents; c++) {
        const std::uint8_t* comp = frame + layout.componentOffsets[c];
        if (ref.components.kinds[c] == Movie::kThpComponentVideo) {
            // The SDK declarations (revolution/thp.h) take non-const input.
            THPVideoDecode(const_cast<std::uint8_t*>(comp), e.y.data(), e.u.data(), e.v.data(), nullptr);
        } else {
            e.pcm.resize(std::size_t{ref.header.audioMaxSamples} * 2);
            const std::uint32_t samples = THPAudioDecode(e.pcm.data(), const_cast<std::uint8_t*>(comp), 0);
            e.pcm.resize(std::size_t{samples} * 2);
        }
    }
    return e;
}

template <class Predicate>
bool pump(THPSimplePlayerWrapper& player, Predicate done, int timeoutMs = 20000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        player.updateNerve();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

u32 pendingSamples(const THPSimplePlayerWrapper& player) {
    u32 total = 0;
    for (const THPAudioBuffer& buffer : player.mAudioBuffer) {
        total += buffer.validSample;
    }
    return total;
}

struct PlaybackResult {
    bool opened = false;
    std::uint32_t framesChecked = 0;
    std::uint64_t samplesChecked = 0;
};

// Plays up to `frames` frames through the wrapper as MoviePlayerSimple does and
// compares every decoded frame and mixed sample. `file` may be null (death
// tests), in which case nothing is compared.
PlaybackResult play(const char* dvdPath, const std::uint8_t* file, const Reference* ref, std::uint32_t frames,
                    const std::string& name, bool loop = false) {
    PlaybackResult result;
    auto player = std::make_unique<THPSimplePlayerWrapper>("THP test");
    if (!player->init(0)) {
        check(false, "wrapper init", name);
        return result;
    }
    check(sRecordedMixCallback == THPSimplePlayerStaticAudio::audioCallback && sRecordedMixMode == MIX_MODE_INTERLEAVE,
          "init registers the interleaved JAS mix callback", name);
    if (!player->open(dvdPath)) {
        check(false, "wrapper opens the movie", name);
        return result;
    }
    if (!pump(*player, [&] { return player->_9 != 0; })) {
        check(false, "wrapper finishes reading the movie header", name);
        return result;
    }
    result.opened = true;

    THPVideoInfo info;
    check(player->getVideoInfo(&info), "getVideoInfo succeeds once open", name);
    if (ref != nullptr) {
        check(info.xSize == ref->components.video.xSize && info.ySize == ref->components.video.ySize &&
                  info.videoType == ref->components.video.videoType,
              "video info is decoded from big-endian", name);
        check(player->getTotalFrame() == static_cast<s32>(ref->header.numFrames), "total frames", name);
        check(player->getFrameRate() == ref->header.frameRate, "frame rate", name);
        check(player->mAudioExist == (ref->components.hasAudio ? 1 : 0) &&
                  player->mAudioInfo.sndNumTracks == ref->components.audio.sndNumTracks &&
                  player->mAudioInfo.sndFrequency == ref->components.audio.sndFrequency,
              "audio info is decoded from big-endian", name);
        check(player->mHeader.movieDataOffsets == ref->header.movieDataOffsets &&
                  player->mHeader.firstFrameSize == ref->header.firstFrameSize,
              "header offsets are decoded from big-endian", name);
    }

    // Exact-size block: ASan reports any wrapper access outside calcNeedMemory().
    const u32 need = player->calcNeedMemory();
    auto buffer = std::unique_ptr<u8, void (*)(void*)>(static_cast<u8*>(std::aligned_alloc(32, (need + 31) & ~31u)),
                                                       std::free);
    check(player->setBuffer(buffer.get()), "setBuffer", name);
    check(static_cast<u8*>(player->mTHPWork) <= buffer.get() + need, "buffer layout fits calcNeedMemory", name);
    check(player->preLoad(loop ? 1 : 0), "preLoad starts", name);
    if (!pump(*player, [&] { return !player->isPreLoading(); })) {
        check(false, "preload completes", name);
        return result;
    }
    // Without looping the preload stops at the last frame; with looping it wraps
    // to frame 0 and always fills the 20 read buffers.
    const s32 movieFrames = player->getTotalFrame();
    const s32 expectedReadFrame = loop ? 20 % movieFrames : std::min<s32>(20, movieFrames);
    check(player->mTotalReadFrame == expectedReadFrame, "preload read position",
          name + ": " + std::to_string(player->mTotalReadFrame) + " vs " + std::to_string(expectedReadFrame));

    std::vector<std::int16_t> expectedPcm;
    std::size_t pcmCursor = 0;
    GXRenderModeObj rmode{};
    rmode.fbWidth = 640;
    rmode.efbHeight = 456;
    const std::uint32_t total =
        loop ? frames : std::min<std::uint32_t>(frames, static_cast<std::uint32_t>(player->getTotalFrame()));
    const auto mix = [&] {
        const u32 before = pendingSamples(*player);
        s16* out = sRecordedMixCallback(560);
        const u32 consumed = before - pendingSamples(*player);
        if (out == nullptr) {
            check(false, "mix callback returns a buffer while a movie with audio is open", name);
            return;
        }
        if (file == nullptr) {
            return;
        }
        bool match = true;
        for (u32 i = 0; i < 560 * 2; i++) {
            const s16 expected = i < consumed * 2 ? expectedPcm[pcmCursor + i] : 0;
            match = match && out[i] == expected;
        }
        if (!match) {
            check(false, "mixed PCM equals the decoded frame samples (unity volume) then silence",
                  name + " at sample " + std::to_string(pcmCursor / 2));
        }
        pcmCursor += std::size_t{consumed} * 2;
        result.samplesChecked += consumed;
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60 + total / 10);
    for (std::uint32_t index = 0; index < total;) {
        if (std::chrono::steady_clock::now() > deadline) {
            check(false, "playback makes progress", name + " at frame " + std::to_string(index));
            break;
        }
        player->updateNerve();
        const std::uint64_t spinsBefore = sSpins.load();
        const s32 status = player->decode(0);
        if (sOsMode && status == 0) {
            sDecodedFrames++;
            sReleasingDecodes += sSpins.load() != spinsBefore;
        }
        if (status == 2) {
            // Next frame not read yet.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        if (status == 3) {
            // Audio ring full: the audio thread would consume a DAC frame.
            mix();
            continue;
        }
        if (status != 0) {
            check(false, "decode returns success", name + " frame " + std::to_string(index));
            break;
        }
        std::unique_ptr<ExpectedFrame> expected;
        if (file != nullptr) {
            expected = std::make_unique<ExpectedFrame>(expectedFrame(file, *ref, index % movieFrames));
            expectedPcm.insert(expectedPcm.end(), expected->pcm.begin(), expected->pcm.end());
        }
        const int setups = sDraw.setups;
        const s32 drawn = player->drawCurrentFrame(&rmode, 0, 44, 832, info.ySize);
        check(drawn == static_cast<s32>(index % movieFrames), "drawCurrentFrame returns the decoded frame number",
              name + " frame " + std::to_string(index));
        check(sDraw.setups == setups + 1 && sDraw.restores == setups + 1 && sDraw.width == static_cast<s16>(info.xSize) &&
                  sDraw.height == static_cast<s16>(info.ySize) && sDraw.polyW == 832 && sDraw.yPos == 44,
              "drawCurrentFrame sets up, draws the movie-sized textures, and restores", name);
        if (expected) {
            const bool same = std::memcmp(sDraw.y, expected->y.data(), expected->y.size()) == 0 &&
                              std::memcmp(sDraw.u, expected->u.data(), expected->u.size()) == 0 &&
                              std::memcmp(sDraw.v, expected->v.data(), expected->v.size()) == 0;
            check(same, "drawn Y/U/V textures equal an independent decode of the same frame",
                  name + " frame " + std::to_string(index));
        }
        mix();
        result.framesChecked++;
        index++;
    }

    check(player->loadStop(), "loadStop succeeds", name);
    check(player->close(), "close succeeds after loadStop", name);
    player->quit();
    check(sRecordedMixCallback == nullptr, "quit unregisters the mix callback", name);
    return result;
}

// Stops during preload and immediately frees the buffers, as MoviePlayerSimple::stop
// does. Any DVD transfer still targeting the freed buffers would be an ASan report.
void stopDuringPreload(const char* dvdPath, const std::string& name) {
    for (int step = 1; step <= 6; step++) {
        auto player = std::make_unique<THPSimplePlayerWrapper>("THP stop test");
        player->init(0);
        player->open(dvdPath);
        if (!pump(*player, [&] { return player->_9 != 0; })) {
            check(false, "stop test opens", name);
            return;
        }
        const u32 need = player->calcNeedMemory();
        u8* buffer = static_cast<u8*>(std::aligned_alloc(32, (need + 31) & ~31u));
        player->setBuffer(buffer);
        player->preLoad(0);
        for (int i = 0; i < step; i++) {
            player->updateNerve();
        }
        check(player->loadStop(), "loadStop during preload succeeds", name);
        check(player->close(), "close during preload succeeds", name);
        std::free(buffer);
        for (int i = 0; i < 20; i++) {
            player->updateNerve();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(!player->isPreLoading() && player->mOpen == 0, "no preload continues after stop", name);
        player->quit();
    }
}

// ---------------------------------------------------------------------------
// Malformed movies

struct Clip {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> offsets;  // frame offsets in `bytes`
};

// The first `frames` frames of a real movie as a self-consistent THP file.
Clip makeClip(const std::uint8_t* file, const Reference& ref, std::uint32_t frames) {
    Clip clip;
    const std::uint32_t start = ref.header.movieDataOffsets;
    const std::uint32_t end = ref.offsets[frames - 1] + ref.sizes[frames - 1];
    clip.bytes.assign(file, file + end);
    for (std::uint32_t i = 0; i < frames; i++) {
        clip.offsets.push_back(ref.offsets[i]);
    }
    auto put32 = [&](std::size_t offset, std::uint32_t value) {
        for (int i = 0; i < 4; i++) {
            clip.bytes[offset + i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
        }
    };
    put32(0x14, frames);
    put32(0x1C, end - start);
    put32(0x2C, ref.offsets[frames - 1]);
    return clip;
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; i++) {
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (24 - i * 8));
    }
}

struct DeathCase {
    const char* name;
    const char* expect;  // substring of the panic; null means the clip must play
    void (*mutate)(Clip&, const Reference&);
};

// Offset of component `c` of frame `f` in the clip.
std::size_t componentOffset(const Clip& clip, const Reference& ref, std::uint32_t f, std::uint32_t c) {
    Movie::ThpFrame layout{};
    Movie::parseThpFrame(clip.bytes.data() + clip.offsets[f], ref.sizes[f], ref.components, &layout);
    return clip.offsets[f] + layout.componentOffsets[c];
}

std::uint32_t kindIndex(const Reference& ref, std::uint8_t kind) {
    for (std::uint32_t c = 0; c < ref.components.numComponents; c++) {
        if (ref.components.kinds[c] == kind) {
            return c;
        }
    }
    return 0;
}

const DeathCase kDeathCases[] = {
    {"valid clip", nullptr, [](Clip&, const Reference&) {}},
    {"THP 1.0 version", "THP version is not 1.1", [](Clip& clip, const Reference&) { put32(clip.bytes, 4, 0x10000); }},
    {"first frame above bufSize", "larger than the read buffer",
     [](Clip& clip, const Reference& ref) { put32(clip.bytes, 0x18, ref.header.bufSize + 4); }},
    {"truncated file", "extends past the end of the file",
     [](Clip& clip, const Reference&) { clip.bytes.resize(clip.bytes.size() - 64); }},
    {"44.1 kHz audio", "does not resample",
     [](Clip& clip, const Reference&) { put32(clip.bytes, 0x30 + 0x14 + 12 + 4, 44100); }},
    {"frame 3 JPEG width", "frame 3: THP JPEG frame dimensions differ",
     [](Clip& clip, const Reference& ref) {
         const std::size_t comp = componentOffset(clip, ref, 3, kindIndex(ref, Movie::kThpComponentVideo));
         for (std::size_t i = comp; i + 8 < clip.bytes.size(); i++) {
             if (clip.bytes[i] == 0xFF && clip.bytes[i + 1] == 0xC0) {
                 clip.bytes[i + 7] ^= 0x10;  // SOF width, low byte of 640
                 break;
             }
         }
     }},
    {"frame 2 audio samples", "frame 2: THP audio record exceeds",
     [](Clip& clip, const Reference& ref) {
         const std::size_t comp = componentOffset(clip, ref, 2, kindIndex(ref, Movie::kThpComponentAudio));
         put32(clip.bytes, comp + 4, ref.header.audioMaxSamples + 1);
     }},
    {"frame 4 truncated scan", "frame 4: THP JPEG",
     [](Clip& clip, const Reference& ref) {
         const std::uint32_t video = kindIndex(ref, Movie::kThpComponentVideo);
         // Shrink the video component to 64 bytes; the remaining bytes become padding.
         put32(clip.bytes, clip.offsets[4] + 8 + 4 * video, 64);
         (void)clip;
     }},
    {"frame 25 next size", "larger than the read buffer",
     [](Clip& clip, const Reference&) { put32(clip.bytes, clip.offsets[25], 0x7FFFFFF0u); }},
};

// Each case runs in a child with its own mounted temporary disc.
void runDeathCase(const DeathCase& testCase, const std::uint8_t* file, const Reference& ref) {
    Clip clip = makeClip(file, ref, 40);
    testCase.mutate(clip, ref);
    char pattern[] = "/tmp/petari-movie-XXXXXX";
    const char* tmp = std::getenv("TMPDIR");
    std::string root = std::string(tmp ? tmp : "/tmp") + "/petari-movie-XXXXXX";
    std::vector<char> rootChars(root.begin(), root.end());
    rootChars.push_back(0);
    (void)pattern;
    if (::mkdtemp(rootChars.data()) == nullptr) {
        check(false, "create temporary disc", testCase.name);
        return;
    }
    root = rootChars.data();
    fs::create_directories(root + "/files/MovieData");
    {
        std::ofstream out(root + "/files/MovieData/Clip.thp", std::ios::binary);
        out.write(reinterpret_cast<const char*>(clip.bytes.data()), static_cast<std::streamsize>(clip.bytes.size()));
    }

    std::fflush(nullptr);
    int pipeFds[2];
    ::pipe(pipeFds);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[0]);
        std::string error;
        if (!PDVD::mount({root}, &error)) {
            std::fprintf(stderr, "mount failed: %s\n", error.c_str());
            std::_Exit(3);
        }
        DVDInit();
        const PlaybackResult r = play("/MovieData/Clip.thp", nullptr, nullptr, 40, testCase.name);
        std::fflush(nullptr);
        std::_Exit(r.opened && r.framesChecked == 40 && sFailures == 0 ? 0 : 4);
    }
    ::close(pipeFds[1]);
    std::string output;
    char chunk[512];
    ssize_t n;
    while ((n = ::read(pipeFds[0], chunk, sizeof(chunk))) > 0) {
        output.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(pipeFds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    fs::remove_all(root);

    if (testCase.expect == nullptr) {
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "valid 40-frame clip plays in a child", output);
    } else {
        const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
        const bool reason = output.find(testCase.expect) != std::string::npos && output.find("THP movie") != std::string::npos;
        check(aborted && reason, testCase.name, "expected OSPanic containing \"" + std::string(testCase.expect) + "\", got: " + output);
    }
    std::printf("  malformed: %-26s -> %s\n", testCase.name,
                testCase.expect ? (WIFSIGNALED(status) ? "panicked" : "did not panic") : (WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "played" : "failed"));
}

// Looping playback of a short clip in a child with its own disc: every drawn
// frame and mixed sample is compared with frame (index mod clip length).
void runLoopCase(const std::uint8_t* file, const Reference& ref, std::uint32_t clipFrames, std::uint32_t playFrames) {
    const Clip clip = makeClip(file, ref, clipFrames);
    Reference clipRef;
    const std::string name = std::to_string(clipFrames) + "-frame clip looped for " + std::to_string(playFrames);
    if (!loadReference(clip.bytes.data(), clip.bytes.size(), &clipRef)) {
        check(false, "clip reference loads", name);
        return;
    }
    const char* tmp = std::getenv("TMPDIR");
    std::string root = std::string(tmp ? tmp : "/tmp") + "/petari-loop-XXXXXX";
    std::vector<char> rootChars(root.begin(), root.end());
    rootChars.push_back(0);
    if (::mkdtemp(rootChars.data()) == nullptr) {
        check(false, "create temporary disc", name);
        return;
    }
    root = rootChars.data();
    fs::create_directories(root + "/files/MovieData");
    {
        std::ofstream out(root + "/files/MovieData/Loop.thp", std::ios::binary);
        out.write(reinterpret_cast<const char*>(clip.bytes.data()), static_cast<std::streamsize>(clip.bytes.size()));
    }
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::string error;
        if (!PDVD::mount({root}, &error)) {
            std::fprintf(stderr, "mount failed: %s\n", error.c_str());
            std::_Exit(3);
        }
        DVDInit();
        const PlaybackResult r = play("/MovieData/Loop.thp", clip.bytes.data(), &clipRef, playFrames, name, true);
        std::printf("  loop: %s: %u frames drawn and compared (%u wraps), %llu stereo samples mixed and compared\n",
                    name.c_str(), r.framesChecked, r.framesChecked / clipFrames,
                    static_cast<unsigned long long>(r.samplesChecked));
        std::fflush(nullptr);
        std::_Exit(r.opened && r.framesChecked == playFrames && sFailures == 0 ? 0 : 4);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    fs::remove_all(root);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "looping playback matches the clip frames", name);
}

}  // namespace

int main(int argc, char** argv) {
    std::string discRoot;
    if (const char* env = std::getenv("PETARI_SOURCE_DIR")) {
        discRoot = std::string(env) + "/build/game-data/RMGE01";
    } else {
        discRoot = "build/game-data/RMGE01";
    }
    std::uint32_t frames = 150;
    std::string only;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--disc" && i + 1 < argc) {
            discRoot = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--all") {
            frames = UINT32_MAX;
        } else if (arg == "--only" && i + 1 < argc) {
            only = argv[++i];
        } else if (arg == "--os") {
            sOsMode = true;
        } else {
            std::fprintf(stderr, "usage: %s [--disc ROOT] [--frames N | --all] [--only NAME] [--os]\n", argv[0]);
            return 2;
        }
    }
    const std::string movieDir = discRoot + "/files/MovieData";
    if (!fs::is_directory(movieDir)) {
        std::printf("%s not found; wrapper tests were not run\n", movieDir.c_str());
        return 0;
    }
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(movieDir)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".thp" && (only.empty() || name.find(only) != std::string::npos)) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    if (names.empty()) {
        std::printf("no movies selected\n");
        return 1;
    }

    // Forked death tests first, while this process has no DVD threads.
    {
        Mapped file(movieDir + "/" + names.front());
        Reference ref;
        if (file.data == nullptr || !loadReference(file.data, file.size, &ref)) {
            check(false, "reference movie loads", names.front());
        } else {
            std::printf("malformed clips from %s:\n", names.front().c_str());
            for (const DeathCase& testCase : kDeathCases) {
                runDeathCase(testCase, file.data, ref);
            }
            runLoopCase(file.data, ref, 30, 75);
            runLoopCase(file.data, ref, 12, 40);
        }
    }

    // After the forked cases, which must not inherit OS threads.
    static OSThread spinnerThread;
    alignas(16) static std::uint8_t spinnerStack[64 * 1024];
    if (sOsMode) {
        __OSThreadInit();  // the test thread becomes the default OS thread (priority 16)
        OSCreateThread(&spinnerThread, spinner, nullptr, spinnerStack + sizeof(spinnerStack), sizeof(spinnerStack), 24, 0);
        OSResumeThread(&spinnerThread);
        check(OSGetThreadPriority(OSGetCurrentThread()) < 24, "the test thread outranks the spinner");
    }

    std::string error;
    if (!PDVD::mount({discRoot}, &error)) {
        std::fprintf(stderr, "mount failed: %s\n", error.c_str());
        return 1;
    }
    DVDInit();

    for (const std::string& name : names) {
        Mapped file(movieDir + "/" + name);
        Reference ref;
        if (file.data == nullptr || !loadReference(file.data, file.size, &ref)) {
            check(false, "reference movie loads", name);
            continue;
        }
        const std::string dvdPath = "/MovieData/" + name;
        const auto started = std::chrono::steady_clock::now();
        const PlaybackResult r = play(dvdPath.c_str(), file.data, &ref, frames, name);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::printf("  %s: %u frames drawn and compared, %llu stereo samples mixed and compared, %.1f s\n",
                    name.c_str(), r.framesChecked, static_cast<unsigned long long>(r.samplesChecked), seconds);
        stopDuringPreload(dvdPath.c_str(), name);
    }

    // SC mono mode folds both channels (0.707 * mean) in the mixer.
    sSoundMode = SC_SOUND_MODE_MONO;
    {
        auto player = std::make_unique<THPSimplePlayerWrapper>("THP mono");
        player->init(0);
        check(player->_30C == 1, "SC mono mode selects the mono fold-down");
        player->quit();
    }

    if (sOsMode) {
        // The join gives the spinner the CPU; it sees the stop flag and returns.
        sSpinnerStop.store(true);
        OSJoinThread(&spinnerThread, nullptr);
        std::printf("  OS mode: %u frame decodes, lower-priority thread progressed during %u (undercounts releases)\n",
                    sDecodedFrames, sReleasingDecodes);
        check(sDecodedFrames > 0 && sReleasingDecodes > 0, "a ready lower-priority OS thread makes progress during frame decodes");
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", sFailures);
        return 1;
    }
    std::printf("wrapper tests passed\n");
    return 0;
}

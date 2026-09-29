// Wii Remote speaker resource test: the disc's AudioRes/SpkRes/SpkRes.arc (spktable.bct,
// spkwave.csw) through the game's SpkTable, SpkWave and SpkMixingBuffer, which read the
// big-endian resources in place. Checks every table entry resolves to a wave inside the
// file with a valid loop, and that mixed samples are the file's big-endian samples: the
// 600 Hz test tone (CS_TEST_SIN_600HZ) must repeat every 10 samples (6000 Hz speaker rate),
// which byte-swapped samples do not.
//
// Usage: petari_speaker_tests --assets GAME_FILES_DIR
// Links: src/Game/Speaker/{SpkTable,SpkWave,SpkMixingBuffer}.cpp, petari_j3d (JKernel, JASCalc),
// native/tests/heap_diagnostics.cpp.
#include "archive.hpp"
#include "Game/Speaker/SpkMixingBuffer.hpp"
#include "Game/Speaker/SpkTable.hpp"
#include "Game/Speaker/SpkWave.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <petari/boot.hpp>
#include <petari/endian.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// SpkMixingBuffer::update calls into the sound holder, which this test does not link.
class SpkSoundHolder {
public:
    bool update(s32);
};
bool SpkSoundHolder::update(s32) {
    std::abort();
}

using Buffer = std::vector< std::uint8_t >;

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

int main(int argc, char** argv) {
    if (argc != 3 || std::strcmp(argv[1], "--assets") != 0) {
        std::fprintf(stderr, "Usage: %s --assets GAME_FILES_DIR\n", argv[0]);
        return 2;
    }

    std::ifstream file(std::filesystem::path(argv[2]) / "AudioRes/SpkRes/SpkRes.arc", std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "FAIL: AudioRes/SpkRes/SpkRes.arc not found\n");
        return 1;
    }
    const Buffer bytes((std::istreambuf_iterator< char >(file)), std::istreambuf_iterator< char >());
    const PetariNative::Resource::Archive archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
    Buffer table, wave;
    for (std::size_t i = 0; i < archive.entries().size(); i++) {
        if (archive.entries()[i].name == "spktable.bct") {
            table = archive.resourceData(i);
        } else if (archive.entries()[i].name == "spkwave.csw") {
            wave = archive.resourceData(i);
        }
    }
    if (table.size() < 0x10 || wave.size() < 8) {
        std::fprintf(stderr, "FAIL: spktable.bct or spkwave.csw missing\n");
        return 1;
    }

    OSInit();
    JKRExpHeap::createRoot(1, false);

    SpkTable spkTable;
    spkTable.setResource(table.data());
    SpkWave spkWave;
    spkWave.setResource(wave.data());
    const u32 waveCount = PetariNative::readU32BE(&wave[4]);
    check(spkTable.mInitialized && spkTable.mResourceCount == 34 && waveCount >= 34, "table and wave counts");

    s32 sineID = -1;
    for (s32 i = 0; i < spkTable.mResourceCount; i++) {
        const SpkParameters* pParams = spkTable.getParameters(i);
        const u16 waveID = pParams->mWaveID;
        const std::string name = spkTable.mNames[i];
        if (name.compare(0, 3, "CS_") != 0 || waveID >= waveCount) {
            std::fprintf(stderr, "FAIL: entry %d (%s): wave ID %u out of range\n", i, name.c_str(), waveID);
            ++sFailures;
            continue;
        }
        const u8* pWaveData = reinterpret_cast< const u8* >(spkWave.getWaveData(waveID));
        const std::size_t offset = static_cast< std::size_t >(pWaveData - wave.data());
        const u32 size = spkWave.getWaveSize(waveID);
        const s32 loopStart = spkWave.getLoopStartPos(waveID);
        const s32 loopEnd = spkWave.getLoopEndPos(waveID);
        const bool loopOk = (loopStart == -1 && loopEnd == -1) || (loopStart >= 0 && loopStart < loopEnd && static_cast< u32 >(loopEnd) <= size / 2);
        if (offset + 12 + size > wave.size() || size == 0 || !loopOk || pParams->mReleaseTime > 1000) {
            std::fprintf(stderr, "FAIL: entry %d (%s): wave %u at 0x%zx size %u loop %d..%d release %u\n", i, name.c_str(), waveID, offset,
                         size, loopStart, loopEnd, static_cast< u32 >(pParams->mReleaseTime));
            ++sFailures;
        }
        if (name == "CS_TEST_SIN_600HZ") {
            sineID = waveID;
        }
    }

    // Mix the looping test tone at full volume into an empty buffer, as SpkSound does, and
    // compare with the file's big-endian samples; a 600 Hz tone repeats every 10 samples at
    // the speaker's 6000 Hz rate.
    check(sineID >= 0, "CS_TEST_SIN_600HZ present");
    if (sineID >= 0) {
        SpkMixingBuffer mix(JKRHeap::sRootHeap);
        s16* pSamples = spkWave.getWave(sineID);
        mix.mix(0, pSamples, 40, 1.0f, 0);
        const s16* pMixed = mix.getSamples(0);
        bool sameAsFile = true, periodic = true;
        s32 peak = 0;
        for (s32 i = 0; i < 40; i++) {
            const s16 expected = static_cast< s16 >(PetariNative::readU16BE(&pSamples[i]));
            sameAsFile = sameAsFile && pMixed[i] == expected;
            peak = std::max(peak, std::abs(static_cast< s32 >(pMixed[i])));
            if (i + 10 < 40) {
                periodic = periodic && pMixed[i] == static_cast< s16 >(PetariNative::readU16BE(&pSamples[i + 10]));
            }
        }
        check(sameAsFile, "mixed samples are the file's big-endian samples");
        check(periodic && peak > 20000, "600 Hz test tone repeats every 10 samples at full scale");
    }

    std::printf("Speaker: %d table entries, %u waves checked\n", spkTable.mResourceCount, waveCount);
    if (sFailures != 0) {
        std::fprintf(stderr, "%d speaker check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("Speaker tests passed");
    return 0;
}

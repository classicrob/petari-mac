// Rhythm chord tables (.cit in AudioRes/Info/JaiChord.arc) parsed on native pointers.
// Usage: aud_chord_tests --assets <files dir>
#include "Game/RhythmLib/AudChordInfo.hpp"
#include "JSystem/JKernel/JKRHeap.hpp"
#include <archive.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" void OSInit();

static int sFailures;
static int sChecks;

#define CHECK(expr)                                                                                                                                  \
    do {                                                                                                                                             \
        sChecks++;                                                                                                                                   \
        if (!(expr)) {                                                                                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                                                          \
            sFailures++;                                                                                                                             \
        }                                                                                                                                            \
    } while (0)

namespace {
using PetariNative::readU16BE;
using PetariNative::readU32BE;

std::vector< u8 > readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector< u8 > bytes((std::istreambuf_iterator< char >(stream)), std::istreambuf_iterator< char >());
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "Yaz0", 4) == 0) {
        JKRHeap::HostAllocationScope host;
        bytes = PetariNative::Resource::decompress({bytes.data(), bytes.size()});
    }
    return bytes;
}

// Every table resolves to the records its big-endian offsets name, and every
// chord and scale can be selected.
bool checkTable(AudChordInfo& info, std::vector< u8 >& res) {
    u8* base = res.data();
    if (!info.mTable.setChordTableResource(base)) {
        return false;
    }
    info.mTable.mLoaded = true;
    const u32 chords = readU16BE(base + 12);
    const u32 scales = readU16BE(base + 14);
    bool ok = static_cast< u32 >(info.mTable.mChordCount) == chords && static_cast< u32 >(info.mTable.mScaleCount) == scales && chords > 0 &&
              scales > 0;
    for (u32 i = 0; ok && i < chords; i++) {
        ok &= reinterpret_cast< u8* >(info.mTable.mChordPtr[i]) == base + readU32BE(base + 16 + i * 4);
        ok &= info.setCurChord(i) && info.getRoot() < 12 && info.getBassNote() < 12;
    }
    for (u32 i = 0; ok && i < scales; i++) {
        const u8* scale = base + readU32BE(base + 16 + (chords + i) * 4);
        ok &= info.mTable.mScalePtr[i]->up == base + readU32BE(scale) && info.mTable.mScalePtr[i]->down == base + readU32BE(scale + 4);
        ok &= info.setCurScale(i);
    }
    // The resource itself is left big-endian and unrelocated.
    ok &= readU32BE(base) == 0;
    return ok;
}

void testCorruptTables(AudChordInfo& info, const std::vector< u8 >& good) {
    const s32 chords = info.mTable.mChordCount;
    AudChordData** chordPtr = info.mTable.mChordPtr;

    std::vector< u8 > bad = good;
    bad[8] = bad[9] = bad[10] = 0;
    bad[11] = 8;  // size smaller than the offset tables
    CHECK(!info.mTable.setChordTableResource(bad.data()));

    bad = good;
    bad[16] = 0x7f;  // first chord offset far outside the resource
    CHECK(!info.mTable.setChordTableResource(bad.data()));

    bad = good;
    const u32 scale = readU32BE(bad.data() + 16 + readU16BE(bad.data() + 12) * 4);
    bad[scale] = 0x10;  // scale "up" list outside the resource
    CHECK(!info.mTable.setChordTableResource(bad.data()));

    bad = good;
    bad[5] = 'X';
    CHECK(!info.mTable.setChordTableResource(bad.data()));

    // A rejected table leaves the loaded one in place.
    CHECK(info.mTable.mChordCount == chords && info.mTable.mChordPtr == chordPtr);
}
}  // namespace

int main(int argc, char** argv) {
    const char* assets = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--assets") == 0) {
            assets = argv[i + 1];
        }
    }
    if (assets == nullptr) {
        std::puts("aud chord tests skipped (no --assets)");
        return 0;
    }
    OSInit();

    std::vector< u8 > bytes = readFile(std::filesystem::path(assets) / "AudioRes/Info/JaiChord.arc");
    CHECK(bytes.size() >= 4 && std::memcmp(bytes.data(), "RARC", 4) == 0);
    JKRHeap::HostAllocationScope host;
    PetariNative::Resource::Archive archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});

    static AudChordInfo info;
    std::vector< std::vector< u8 > > tables;
    int chords = 0;
    for (size_t i = 0; i < archive.entries().size(); i++) {
        const auto& entry = archive.entries()[i];
        if (entry.isDirectory() || entry.name.size() < 5 || entry.name.compare(entry.name.size() - 4, 4, ".cit") != 0) {
            continue;
        }
        // Tables stay alive: the parsed pointers refer into them.
        tables.push_back(archive.resourceData(i));
        bool ok = checkTable(info, tables.back());
        CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "  bad chord table %s\n", entry.name.c_str());
        }
        chords += info.mTable.mChordCount;
    }
    std::printf("chord tables: %zu files, %d chords\n", tables.size(), chords);
    CHECK(tables.size() > 50);
    if (!tables.empty()) {
        testCorruptTables(info, tables.back());
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d chord check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("aud chord tests passed (%d checks)\n", sChecks);
    return 0;
}

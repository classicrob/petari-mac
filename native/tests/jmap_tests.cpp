// JMapInfo (BCSV) tests for the native build.
//
// Links: src/Game/Util/JMapInfo.cpp, src/JSystem/JGadget/hashcode.cpp, petari_resources.
// JMapInfo.cpp only needs MR::isEqualStringCase from StringUtil.cpp, which drags in the
// message system; this file supplies the same one-line definition instead.
//
// Usage: petari_jmap_tests                 synthetic big-endian records
//        petari_jmap_tests --assets FILES  also read every BCSV in StageData/*.arc and
//                                          the BCSVs embedded in the executable (read-only)
#include "archive.hpp"
#include "Game/Util/JMapInfo.hpp"
#include <JSystem/JGadget/hashcode.hpp>
#include <strings.h>
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace MR {
    bool isEqualStringCase(const char* pStr1, const char* pStr2) {
        return strcasecmp(pStr1, pStr2) == 0;
    }
}  // namespace MR

using Buffer = std::vector< std::uint8_t >;

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

static void put32(Buffer& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast< std::uint8_t >(value >> (24 - i * 8));
    }
}

static void put16(Buffer& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast< std::uint8_t >(value >> 8);
    data[offset + 1] = static_cast< std::uint8_t >(value);
}

static std::uint32_t floatBits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

namespace {
    struct Field {
        const char* mName;
        std::uint32_t mMask;
        std::uint16_t mOffset;
        std::uint8_t mShift;
        std::uint8_t mType;
    };

    const Field cFields[] = {
        {"id", 0xFFFFFFFF, 0x00, 0, JMAP_VALUE_TYPE_LONG},
        {"flags", 0x0000FF00, 0x04, 8, JMAP_VALUE_TYPE_LONG},
        {"scale", 0xFFFFFFFF, 0x08, 0, JMAP_VALUE_TYPE_FLOAT},
        {"count", 0xFFFF, 0x0C, 0, JMAP_VALUE_TYPE_SHORT},
        {"tiny", 0xFF, 0x0E, 0, JMAP_VALUE_TYPE_BYTE},
        {"name", 0xFFFFFFFF, 0x10, 0, JMAP_VALUE_TYPE_STRING_PTR},
        {"label", 0xFFFFFFFF, 0x14, 0, JMAP_VALUE_TYPE_STRING},
        {"enabled", 0x00000001, 0x04, 0, JMAP_VALUE_TYPE_LONG},
    };

    const int cFieldNum = sizeof(cFields) / sizeof(cFields[0]);
    const int cEntrySize = 0x1C;

    struct Row {
        std::uint32_t mId;
        std::uint32_t mFlagsWord;
        float mScale;
        std::uint16_t mCount;
        std::uint8_t mTiny;
        const char* mName;
        const char* mLabel;
    };

    // Sorted by name so findElementBinary can be exercised.
    const Row cRows[] = {
        {0x01020304, 0x00001201, 1.5f, 0xFFFE, 0x80, "Alpha", "lab0"},
        {0x7FFFFFFF, 0x0000AB00, -0.25f, 0x0005, 0x7F, "Bravo", "lab1"},
        {0xFFFFFFFF, 0xFFFFFFFF, 1000.0f, 0x8000, 0x01, "Charlie", "lab2"},
    };

    const int cRowNum = sizeof(cRows) / sizeof(cRows[0]);
}  // namespace

// Builds a BCSV exactly as stored on disc: big-endian header, field records, fixed-size
// rows and a trailing string table referenced by STRING_PTR offsets.
static Buffer buildSyntheticBcsv() {
    const std::size_t dataOffset = 0x10 + cFieldNum * 0xC;
    Buffer data(dataOffset + cRowNum * cEntrySize);
    put32(data, 0x0, cRowNum);
    put32(data, 0x4, cFieldNum);
    put32(data, 0x8, static_cast< std::uint32_t >(dataOffset));
    put32(data, 0xC, cEntrySize);

    for (int i = 0; i < cFieldNum; i++) {
        const std::size_t record = 0x10 + i * 0xC;
        put32(data, record + 0x0, JGadget::getHashCode(cFields[i].mName));
        put32(data, record + 0x4, cFields[i].mMask);
        put16(data, record + 0x8, cFields[i].mOffset);
        data[record + 0xA] = cFields[i].mShift;
        data[record + 0xB] = cFields[i].mType;
    }

    std::string strings;
    for (int i = 0; i < cRowNum; i++) {
        const std::size_t row = dataOffset + i * cEntrySize;
        put32(data, row + 0x00, cRows[i].mId);
        put32(data, row + 0x04, cRows[i].mFlagsWord);
        put32(data, row + 0x08, floatBits(cRows[i].mScale));
        put16(data, row + 0x0C, cRows[i].mCount);
        data[row + 0x0E] = cRows[i].mTiny;
        put32(data, row + 0x10, static_cast< std::uint32_t >(strings.size()));
        strings += cRows[i].mName;
        strings += '\0';
        std::memcpy(&data[row + 0x14], cRows[i].mLabel, std::strlen(cRows[i].mLabel) + 1);
    }

    data.insert(data.end(), strings.begin(), strings.end());
    return data;
}

static void testSynthetic() {
    const Buffer data = buildSyntheticBcsv();
    JMapInfo info;
    check(!info.attach(nullptr), "attach(nullptr) must fail");
    check(info.attach(data.data()), "attach failed");
    check(info.getNumEntries() == cRowNum, "entry count not decoded from big-endian header");
    check(info.getNumFields() == cFieldNum, "field count not decoded from big-endian header");
    check(info.mData->mDataOffset == 0x10 + cFieldNum * 0xC, "data offset not decoded");
    check(info.mData->mEntrySize == cEntrySize, "entry size not decoded");
    check(info.searchItemInfo("scale") == 2, "field hash lookup failed");
    check(info.searchItemInfo("missing") == -1, "missing field was found");
    check(info.getValueType("name") == JMAP_VALUE_TYPE_STRING_PTR, "field type not decoded");
    check(info.getValueType("missing") == JMAP_VALUE_TYPE_NULL, "missing field type is not NULL");

    for (int i = 0; i < cRowNum; i++) {
        const Row& row = cRows[i];
        s32 id = 0;
        u32 idU = 0;
        check(info.getValue< s32 >(i, "id", &id) && id == static_cast< s32 >(row.mId), "s32 LONG value");
        check(info.getValue< u32 >(i, "id", &idU) && idU == row.mId, "u32 LONG value");

        u32 flags = 0;
        s32 flagsS = 0;
        check(info.getValue< u32 >(i, "flags", &flags) && flags == ((row.mFlagsWord & 0xFF00) >> 8), "masked/shifted u32");
        check(!info.getValue< s32 >(i, "flags", &flagsS), "s32 read of a shifted field must fail as on the Wii");

        bool enabled = false;
        check(info.getValue< bool >(i, "enabled", &enabled) && enabled == ((row.mFlagsWord & 1) != 0), "bool mask");

        f32 scale = 0.0f;
        check(info.getValue< f32 >(i, "scale", &scale) && scale == row.mScale, "big-endian f32 value");

        s32 count = 0;
        u32 countU = 0;
        check(info.getValue< s32 >(i, "count", &count) && count == static_cast< s16 >(row.mCount), "sign-extended SHORT");
        check(info.getValue< u32 >(i, "count", &countU) && countU == row.mCount, "unsigned SHORT");

        s32 tiny = 0;
        u32 tinyU = 0;
        check(info.getValue< s32 >(i, "tiny", &tiny) && tiny == static_cast< s8 >(row.mTiny), "sign-extended BYTE");
        check(info.getValue< u32 >(i, "tiny", &tinyU) && tinyU == row.mTiny, "unsigned BYTE");

        const char* name = nullptr;
        const char* label = nullptr;
        check(info.getValue< const char* >(i, "name", &name) && std::strcmp(name, row.mName) == 0, "STRING_PTR value");
        check(info.getValue< const char* >(i, "label", &label) && std::strcmp(label, row.mLabel) == 0, "inline STRING value");

        u32 nameAsInt = 0;
        check(!info.getValue< u32 >(i, "name", &nameAsInt), "integer read of a string field must fail");

        JMapInfoIter iter(&info, i);
        f32 iterScale = 0.0f;
        check(iter.isValid() && iter.getValue< f32 >("scale", &iterScale) && iterScale == row.mScale, "iterator getValue");
    }

    check(!JMapInfoIter(&info, cRowNum).isValid() && info.end() == JMapInfoIter(&info, cRowNum), "end iterator");
    check(info.findElement< s32 >("id", 0x7FFFFFFF, 0).mIndex == 1, "findElement<s32>");
    check(info.findElement< const char* >("name", "Charlie", 0).mIndex == 2, "findElement<const char*>");
    check(info.findElement< s32 >("id", 42, 0) == info.end(), "findElement miss");
    check(info.findElementBinary("name", "Bravo").mIndex == 1, "findElementBinary");
    check(info.findElementBinary("name", "Delta") == info.end(), "findElementBinary miss");
    check(MR::findJMapInfoElementNoCase(&info, "name", "cHaRlIe", 0).mIndex == 2, "findJMapInfoElementNoCase");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

// A resource is treated as BCSV when its header describes field records that end exactly
// at the row data and rows that fit in the resource. Returns false for other file types.
static bool looksLikeBcsv(const Buffer& data) {
    if (data.size() < 0x10) {
        return false;
    }
    const std::uint32_t entries = PetariNative::readU32BE(&data[0x0]);
    const std::uint32_t fields = PetariNative::readU32BE(&data[0x4]);
    const std::uint32_t dataOffset = PetariNative::readU32BE(&data[0x8]);
    const std::uint32_t entrySize = PetariNative::readU32BE(&data[0xC]);
    return fields > 0 && fields < 0x400 && dataOffset == 0x10 + fields * 0xC && entrySize > 0 && entrySize < 0x10000 &&
           entries < 0x100000 && dataOffset + static_cast< std::uint64_t >(entries) * entrySize <= data.size();
}

struct SmokeStats {
    std::size_t mArchives = 0;
    std::size_t mTables = 0;
    std::size_t mCells = 0;
    std::size_t mWithName = 0;
};

// Reads every cell through the JMapInfo API and checks each value stays inside the resource.
static void smokeTable(const Buffer& data, const std::string& where, SmokeStats* pStats) {
    JMapInfo info;
    info.attach(data.data());
    const JMapData* pData = info.mData;
    const std::size_t rowsEnd = pData->mDataOffset + static_cast< std::size_t >(pData->mNumEntries) * pData->mEntrySize;

    for (int field = 0; field < info.getNumFields(); field++) {
        const JMapItem& item = pData->mItems[field];
        const std::size_t width = item.mType == JMAP_VALUE_TYPE_SHORT ? 2 : item.mType == JMAP_VALUE_TYPE_BYTE ? 1 : 4;
        if (item.mType > JMAP_VALUE_TYPE_STRING_PTR || item.mOffsData + width > pData->mEntrySize) {
            std::fprintf(stderr, "FAIL: %s field %d has type %u at offset %u\n", where.c_str(), field, item.mType,
                         static_cast< unsigned >(item.mOffsData));
            ++sFailures;
            return;
        }

        for (int entry = 0; entry < info.getNumEntries(); entry++) {
            bool ok = true;
            if (item.mType == JMAP_VALUE_TYPE_FLOAT) {
                f32 value;
                ok = info.getValueFast(entry, field, &value);
            } else if (item.mType == JMAP_VALUE_TYPE_STRING || item.mType == JMAP_VALUE_TYPE_STRING_PTR) {
                const char* value = nullptr;
                ok = info.getValueFast(entry, field, &value);
                const std::uint8_t* start = reinterpret_cast< const std::uint8_t* >(value);
                ok = ok && start >= data.data() && start < data.data() + data.size();
                if (ok && item.mType == JMAP_VALUE_TYPE_STRING_PTR) {
                    ok = start >= data.data() + rowsEnd && std::memchr(start, 0, data.data() + data.size() - start) != nullptr;
                }
            } else {
                u32 value;
                ok = info.getValueFast(entry, field, &value);
            }
            if (!ok) {
                std::fprintf(stderr, "FAIL: %s entry %d field %d is out of range\n", where.c_str(), entry, field);
                ++sFailures;
                return;
            }
            pStats->mCells++;
        }
    }

    if (info.searchItemInfo("name") >= 0) {
        pStats->mWithName++;
    }
    pStats->mTables++;
}

// The embedded tables live in the source tree. Tests may be compiled from a generated
// copy of this file, so search PETARI_SOURCE_DIR, the working directory and the parents of
// this file and of the asset directory.
static std::filesystem::path findRepoRoot(const char* pAssetDir) {
    namespace fs = std::filesystem;
    std::vector< fs::path > candidates;
    if (const char* pEnv = std::getenv("PETARI_SOURCE_DIR")) {
        candidates.push_back(pEnv);
    }
    candidates.push_back(fs::current_path());
    for (fs::path start : {fs::path(__FILE__), fs::absolute(pAssetDir)}) {
        for (fs::path dir = start.parent_path(); !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
            candidates.push_back(dir);
        }
    }
    for (const fs::path& dir : candidates) {
        if (fs::exists(dir / "src/Game/System/GalaxyID.bcsv")) {
            return dir;
        }
    }
    return fs::current_path();
}

static void testEmbeddedTables(const std::filesystem::path& repoRoot, SmokeStats* pStats) {
    const char* cEmbedded[] = {"src/Game/System/GalaxyID.bcsv", "src/Game/System/StoryEvent.bcsv"};
    for (const char* pPath : cEmbedded) {
        Buffer data;
        if (!readFile(repoRoot / pPath, &data)) {
            std::fprintf(stderr, "FAIL: cannot read %s\n", pPath);
            ++sFailures;
            continue;
        }
        check(looksLikeBcsv(data), "embedded table is not a BCSV");
        smokeTable(data, pPath, pStats);

        if (std::strstr(pPath, "GalaxyID") != nullptr) {
            // Same lookups as GameDataConst::getPowerStarNumToOpenGalaxy/getGrandGalaxyNo.
            JMapInfo info;
            info.attach(data.data());
            JMapInfoIter astro = info.findElement< const char* >("name", "AstroGalaxy", 0);
            u32 powerStarNum = 0xFFFFFFFF;
            u32 grandGalaxyNo = 0xFFFFFFFF;
            check(astro.mIndex == 0 && astro.getValue< u32 >("PowerStarNum", &powerStarNum) && powerStarNum == 0 &&
                      astro.getValue< u32 >("GrandGalaxyNo", &grandGalaxyNo) && grandGalaxyNo == 0,
                  "GalaxyID.bcsv AstroGalaxy lookup");
            check(info.findElement< const char* >("name", "AstroDome", 0).isValid(), "GalaxyID.bcsv AstroDome lookup");
        }
    }
}

static void testAssets(const std::filesystem::path& filesRoot, SmokeStats* pStats) {
    namespace fs = std::filesystem;
    const fs::path stageData = filesRoot / "StageData";
    if (!fs::is_directory(stageData)) {
        std::fprintf(stderr, "FAIL: %s is not a directory\n", stageData.c_str());
        ++sFailures;
        return;
    }

    std::vector< fs::path > archives;
    for (const fs::directory_entry& entry : fs::directory_iterator(stageData)) {
        if (entry.is_regular_file() && entry.path().extension() == ".arc") {
            archives.push_back(entry.path());
        }
    }
    std::sort(archives.begin(), archives.end());

    for (const fs::path& path : archives) {
        Buffer bytes;
        if (!readFile(path, &bytes)) {
            std::fprintf(stderr, "FAIL: cannot read %s\n", path.c_str());
            ++sFailures;
            continue;
        }
        try {
            const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
            for (std::size_t i = 0; i < archive.entries().size(); i++) {
                const auto& entry = archive.entries()[i];
                if (entry.isDirectory()) {
                    continue;
                }
                Buffer resource = archive.resourceData(i);
                if (looksLikeBcsv(resource)) {
                    smokeTable(resource, path.filename().string() + ":" + entry.name, pStats);
                }
            }
            pStats->mArchives++;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s: %s\n", path.c_str(), error.what());
            ++sFailures;
        }
    }
}

int main(int argc, char** argv) {
    testSynthetic();

    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        SmokeStats stats;
        testEmbeddedTables(findRepoRoot(argv[2]), &stats);
        testAssets(argv[2], &stats);
        std::printf("BCSV smoke: %zu archives, %zu tables (%zu with a 'name' field), %zu cells read\n", stats.mArchives,
                    stats.mTables, stats.mWithName, stats.mCells);
        check(stats.mTables > 2, "no BCSV tables found in assets");
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d JMap check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("JMap tests passed");
    return 0;
}

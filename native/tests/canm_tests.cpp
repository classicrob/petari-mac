// Camera animation (.canm) decoding tests: the native CanmFileHeader / frame-info structs in
// Game/Camera/CameraAnim.hpp read big-endian fields on access (BigEndianValue).
//
// Default: a synthetic key-framed file. With --assets FILES: every .canm in ObjectData and
// StageData archives is decoded and its component tables are bounds- and value-checked.
// Header-only use of the game header; links petari_resources (archive.cpp) only.
#include "archive.hpp"
#include "Game/Camera/CameraAnim.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

static std::uint32_t floatBits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Checks a decoded file: returns false if any component table is out of bounds or a value
// used by the accessors is not a plausible float.
static bool checkCanm(const Buffer& data, std::string* pError) {
    if (data.size() < sizeof(CanmFileHeader) + sizeof(CanmKeyFrameInfo) + 4) {
        *pError = "file too small";
        return false;
    }
    const CanmFileHeader* pHeader = reinterpret_cast< const CanmFileHeader* >(data.data());
    if (std::memcmp(pHeader->mMagic, "ANDO", 4) != 0) {
        *pError = "bad magic";
        return false;
    }
    const bool isKey = std::memcmp(pHeader->mType, "CKAN", 4) == 0;
    if (!isKey && std::memcmp(pHeader->mType, "CANM", 4) != 0) {
        *pError = "bad type";
        return false;
    }
    const u32 frames = pHeader->mNrFrames;
    const u32 valueOffset = pHeader->mValueOffset;
    const std::uint8_t* pEntry = data.data() + sizeof(CanmFileHeader);
    if (frames == 0 || frames > 100000 || sizeof(CanmFileHeader) + valueOffset + 4 > data.size()) {
        *pError = "bad frame count or value offset";
        return false;
    }
    const u32 valueCount = PetariNative::readU32BE(pEntry + valueOffset) / 4;
    if (sizeof(CanmFileHeader) + valueOffset + 4 + static_cast< std::size_t >(valueCount) * 4 > data.size()) {
        *pError = "value table outside the file";
        return false;
    }
    const CanmValue* pValues = reinterpret_cast< const CanmValue* >(pEntry + valueOffset + 4);

    for (int component = 0; component < 8; component++) {
        u32 count, offset, stride = 1;
        if (isKey) {
            const CanmKeyFrameComponentInfo& info = (&reinterpret_cast< const CanmKeyFrameInfo* >(pEntry)->mPosX)[component];
            count = info.mCount;
            offset = info.mOffset;
            stride = count == 1 ? 1 : (static_cast< u32 >(info.mType) == 0 ? 3 : 4);
        } else {
            const CamnFrameComponentInfo& info = (&reinterpret_cast< const CanmFrameInfo* >(pEntry)->mPosX)[component];
            count = info.mCount;
            offset = info.mOffset;
        }
        if (count == 0 || offset + (count - 1) * stride >= valueCount) {
            *pError = "component table outside the value table";
            return false;
        }
        for (u32 i = 0; i < count * stride && offset + i < valueCount; i++) {
            const f32 value = pValues[offset + i];
            if (!std::isfinite(value) || std::fabs(value) > 1.0e7f) {
                *pError = "implausible value";
                return false;
            }
        }
    }
    return true;
}

static void testSynthetic() {
    // Key-framed: 8 components; position X has two keys (time, value, tangent), the others
    // one value each.
    const float values[] = {0.0f, 100.0f, 0.5f, 60.0f, -200.0f, 0.25f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 45.0f};
    const u32 valueCount = sizeof(values) / sizeof(values[0]);
    Buffer data(sizeof(CanmFileHeader) + sizeof(CanmKeyFrameInfo) + 4 + valueCount * 4);
    std::memcpy(data.data(), "ANDOCKAN", 8);
    put32(data, 0x08, 1);
    put32(data, 0x0C, 0);
    put32(data, 0x10, 1);
    put32(data, 0x18, 61);
    put32(data, 0x1C, sizeof(CanmKeyFrameInfo));
    const std::size_t info = sizeof(CanmFileHeader);
    put32(data, info + 0, 2);
    put32(data, info + 4, 0);
    put32(data, info + 8, 0);
    for (int c = 1; c < 8; c++) {
        put32(data, info + c * 12 + 0, 1);
        put32(data, info + c * 12 + 4, 5 + c);
        put32(data, info + c * 12 + 8, 0);
    }
    const std::size_t table = info + sizeof(CanmKeyFrameInfo);
    put32(data, table, valueCount * 4);
    for (u32 i = 0; i < valueCount; i++) {
        put32(data, table + 4 + i * 4, floatBits(values[i]));
    }

    const CanmFileHeader* pHeader = reinterpret_cast< const CanmFileHeader* >(data.data());
    check(static_cast< u32 >(pHeader->mNrFrames) == 61 && static_cast< s32 >(pHeader->_10) == 1, "header fields decode");
    const CanmKeyFrameInfo* pInfo = reinterpret_cast< const CanmKeyFrameInfo* >(data.data() + info);
    check(static_cast< u32 >(pInfo->mPosX.mCount) == 2 && static_cast< u32 >(pInfo->mFovy.mOffset) == 12, "component info decodes");
    const CanmValue* pValues = reinterpret_cast< const CanmValue* >(data.data() + table + 4);
    check(static_cast< f32 >(pValues[1]) == 100.0f && static_cast< f32 >(pValues[4]) == -200.0f && static_cast< f32 >(pValues[12]) == 45.0f,
          "values decode as host floats");
    std::string error;
    check(checkCanm(data, &error), error.c_str());

    Buffer bad = data;
    put32(bad, info + 7 * 12 + 4, 1000);
    check(!checkCanm(bad, &error), "out-of-range component offset detected");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

static void testAssets(const std::filesystem::path& filesRoot) {
    namespace fs = std::filesystem;
    std::vector< fs::path > archives;
    for (const char* pDir : {"ObjectData", "StageData"}) {
        if (fs::is_directory(filesRoot / pDir)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(filesRoot / pDir)) {
                if (entry.path().extension() == ".arc") {
                    archives.push_back(entry.path());
                }
            }
        }
    }
    std::sort(archives.begin(), archives.end());

    std::size_t decoded = 0, failed = 0;
    for (const fs::path& path : archives) {
        Buffer bytes;
        if (!readFile(path, &bytes)) {
            continue;
        }
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            const auto& entry = archive.entries()[i];
            if (entry.isDirectory() || entry.name.size() < 5 || entry.name.substr(entry.name.size() - 5) != ".canm") {
                continue;
            }
            std::string error;
            if (!checkCanm(archive.resourceData(i), &error)) {
                std::fprintf(stderr, "FAIL: %s:%s: %s\n", path.filename().c_str(), entry.name.c_str(), error.c_str());
                failed++;
            } else {
                decoded++;
            }
        }
    }
    std::printf("Camera animations: %zu decoded, %zu failed\n", decoded, failed);
    check(decoded > 0, "no .canm files found");
    sFailures += static_cast< int >(failed);
}

int main(int argc, char** argv) {
    testSynthetic();
    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        testAssets(argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }
    if (sFailures != 0) {
        std::fprintf(stderr, "%d camera animation check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("Camera animation tests passed");
    return 0;
}

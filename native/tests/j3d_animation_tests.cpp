// J3D animation resource conversion tests (native/resource/j3d_animation.cpp), no JSystem link.
//
// Default: a synthetic big-endian BCK (ANK1) with shared value tables and a BTP (TPT1) with
// a name table, plus malformed-input rejection. With --assets FILES: every J3D1 file in
// ObjectData and StageData archives is converted and checked structurally.
// Links: native/resource/j3d_animation.cpp, petari_resources.
#include "archive.hpp"
#include <petari/endian.hpp>
#include <petari/j3d_animation.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using Buffer = std::vector< std::uint8_t >;
namespace J3D = PetariNative::J3D;

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

template < typename T >
static T hostAt(const Buffer& data, std::size_t offset) {
    T value;
    std::memcpy(&value, &data[offset], sizeof(value));
    return value;
}

static Buffer fileWithBlock(const char* pType, const Buffer& block) {
    Buffer file(0x20);
    std::memcpy(file.data(), "J3D1", 4);
    std::memcpy(&file[4], pType, 4);
    put32(file, 0x0C, 1);
    std::memcpy(&file[0x10], "SVR1", 4);
    put32(file, 0x1C, 0xFFFFFFFF);
    file.insert(file.end(), block.begin(), block.end());
    put32(file, 0x08, static_cast< std::uint32_t >(file.size()));
    return file;
}

// ANK1: 1 joint; scale/translation share one f32 array (as exporters often do).
static Buffer buildBck() {
    Buffer block(0x24 + 0x12 + 2 + 3 * 4 + 4 * 2 + 2);
    std::memcpy(block.data(), "ANK1", 4);
    block[0x08] = 2;           // attribute (loop)
    block[0x09] = 1;           // rotation decimal shift
    put16(block, 0x0A, 90);    // frame max
    put16(block, 0x0C, 1);     // joints
    put16(block, 0x0E, 3);     // scale values
    put16(block, 0x10, 4);     // rotation values
    put16(block, 0x12, 3);     // translation values
    const std::uint32_t table = 0x24, values = table + 0x14, rot = values + 12;
    for (int i = 0; i < 9; i++) {
        put16(block, table + i * 2, static_cast< std::uint16_t >(i % 3 == 0 ? 1 : (i % 3 == 1 ? i / 3 : 0)));
    }
    put32(block, values + 0, floatBits(1.0f));
    put32(block, values + 4, floatBits(-2.5f));
    put32(block, values + 8, floatBits(1000.0f));
    put16(block, rot + 0, 0x4000);
    put16(block, rot + 2, 0xC000);
    put16(block, rot + 4, 0x0001);
    put16(block, rot + 6, 0x8000);
    put32(block, 0x14, table);
    put32(block, 0x18, values);
    put32(block, 0x1C, rot);
    put32(block, 0x20, values);  // translation shares the scale array
    put32(block, 0x04, static_cast< std::uint32_t >(block.size()));
    return fileWithBlock("bck1", block);
}

// TPT1: 1 material, 2 frames of texture indices, material id table and name table.
static Buffer buildBtp() {
    Buffer block(0x20);
    std::memcpy(block.data(), "TPT1", 4);
    put16(block, 0x0A, 2);
    put16(block, 0x0C, 1);
    put16(block, 0x0E, 2);
    auto append = [&](const Buffer& part, std::uint32_t field) {
        while (block.size() % 4 != 0) {
            block.push_back(0);
        }
        put32(block, field, static_cast< std::uint32_t >(block.size()));
        block.insert(block.end(), part.begin(), part.end());
    };
    Buffer table(8);
    put16(table, 0, 2);
    put16(table, 2, 0);
    table[4] = 0;
    put16(table, 6, 0x00FF);
    append(table, 0x10);
    Buffer values(4);
    put16(values, 0, 3);
    put16(values, 2, 0x0102);
    append(values, 0x14);
    Buffer ids(2);
    put16(ids, 0, 7);
    append(ids, 0x18);
    Buffer names(8);
    put16(names, 0, 1);
    put16(names, 2, 0xFFFF);
    put16(names, 4, 0x1234);
    put16(names, 6, 8);
    names.insert(names.end(), {'m', 'a', 't', 0});
    append(names, 0x1C);
    while (block.size() % 0x20 != 0) {
        block.push_back(0);
    }
    put32(block, 0x04, static_cast< std::uint32_t >(block.size()));
    return fileWithBlock("btp1", block);
}

static void testSynthetic() {
    const Buffer bck = buildBck();
    check(J3D::classifyAnimImage(bck.data()) == J3D::AnimImageKind::BigEndian && J3D::animFileSize(bck.data()) == bck.size(), "classify BCK");
    Buffer image(bck.size());
    const char* pError = J3D::makeHostAnimImage(bck.data(), static_cast< std::uint32_t >(bck.size()), image.data());
    check(pError == nullptr, pError != nullptr ? pError : "BCK conversion");
    check(J3D::classifyAnimImage(image.data()) == J3D::AnimImageKind::Host, "classify host image");
    check(hostAt< std::uint32_t >(image, 0x0C) == 1 && hostAt< std::uint32_t >(image, 0x1C) == 0xFFFFFFFF, "file header");
    const std::size_t block = 0x20;
    check(hostAt< std::uint32_t >(image, block + 4) == bck.size() - 0x20 && hostAt< std::int16_t >(image, block + 0x0A) == 90 &&
              hostAt< std::uint16_t >(image, block + 0x10) == 4,
          "ANK1 header");
    const std::size_t values = block + hostAt< std::uint32_t >(image, block + 0x18);
    check(hostAt< float >(image, values + 4) == -2.5f && hostAt< float >(image, values + 8) == 1000.0f,
          "shared scale/translation values converted exactly once");
    const std::size_t rot = block + hostAt< std::uint32_t >(image, block + 0x1C);
    check(hostAt< std::int16_t >(image, rot + 2) == -16384 && hostAt< std::int16_t >(image, rot + 6) == -32768, "s16 rotation keys");
    const std::size_t table = block + hostAt< std::uint32_t >(image, block + 0x14);
    check(hostAt< std::uint16_t >(image, table) == 1 && hostAt< std::uint16_t >(image, table + 8) == 1, "key tables");

    const Buffer btp = buildBtp();
    image.assign(btp.size(), 0);
    pError = J3D::makeHostAnimImage(btp.data(), static_cast< std::uint32_t >(btp.size()), image.data());
    check(pError == nullptr, pError != nullptr ? pError : "BTP conversion");
    const std::size_t tpt = 0x20;
    const std::size_t patternTable = tpt + hostAt< std::uint32_t >(image, tpt + 0x10);
    check(hostAt< std::uint16_t >(image, patternTable) == 2 && image[patternTable + 4] == 0 && hostAt< std::uint16_t >(image, patternTable + 6) == 0xFF,
          "tex pattern table");
    const std::size_t patternValues = tpt + hostAt< std::uint32_t >(image, tpt + 0x14);
    check(hostAt< std::uint16_t >(image, patternValues + 2) == 0x0102, "tex pattern values");
    const std::size_t names = tpt + hostAt< std::uint32_t >(image, tpt + 0x1C);
    check(hostAt< std::uint16_t >(image, names) == 1 && hostAt< std::uint16_t >(image, names + 4) == 0x1234, "name table");

    auto rejects = [](const Buffer& file) {
        Buffer out(file.size());
        return J3D::makeHostAnimImage(file.data(), static_cast< std::uint32_t >(file.size()), out.data()) != nullptr;
    };
    Buffer bad = btp;
    std::memcpy(&bad[0x20], "VCK1", 4);
    check(rejects(bad), "vertex-color block rejected");
    bad = bck;
    put32(bad, 0x20 + 0x14, 0x10000);
    check(rejects(bad), "table offset outside block rejected");
    bad = bck;
    put32(bad, 0x20 + 0x04, 0x100000);
    check(rejects(bad), "oversized block rejected");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

// Checks f32 key/value arrays of ANK1 and TTK1 blocks decode to finite, plausible values.
static bool checkImage(const Buffer& image, std::string* pError) {
    const std::uint32_t blocks = hostAt< std::uint32_t >(image, 0x0C);
    std::size_t block = 0x20;
    for (std::uint32_t i = 0; i < blocks; i++) {
        const std::uint32_t type = hostAt< std::uint32_t >(image, block);
        const std::uint32_t size = std::min< std::uint32_t >(hostAt< std::uint32_t >(image, block + 4), static_cast< std::uint32_t >(image.size() - block));
        std::vector< std::pair< std::uint32_t, std::uint32_t > > floatArrays;  // (offset field, count field)
        if (type == PetariNative::readU32BE("ANK1")) {
            floatArrays = {{0x18, 0x0E}, {0x20, 0x12}};
        } else if (type == PetariNative::readU32BE("TTK1")) {
            floatArrays = {{0x28, 0x0E}, {0x30, 0x12}};
        }
        for (const auto& array : floatArrays) {
            const std::uint32_t offset = hostAt< std::uint32_t >(image, block + array.first);
            const std::uint16_t count = hostAt< std::uint16_t >(image, block + array.second);
            if (offset == 0) {
                continue;
            }
            if (offset + count * 4u > size) {
                *pError = "value count exceeds block";
                return false;
            }
            for (std::uint16_t k = 0; k < count; k++) {
                const float value = hostAt< float >(image, block + offset + k * 4);
                if (!std::isfinite(value) || std::fabs(value) > 1.0e7f) {
                    *pError = "implausible key value";
                    return false;
                }
            }
        }
        block += hostAt< std::uint32_t >(image, block + 4);
    }
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
    check(!archives.empty(), "no archives found under the asset directory");

    std::map< std::string, std::size_t > kinds;
    std::size_t converted = 0, failed = 0;
    for (const fs::path& path : archives) {
        Buffer bytes;
        if (!readFile(path, &bytes)) {
            continue;
        }
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            const auto& entry = archive.entries()[i];
            if (entry.isDirectory()) {
                continue;
            }
            const Buffer file = archive.resourceData(i);
            if (file.size() < 0x20 || std::memcmp(file.data(), "J3D1", 4) != 0) {
                continue;
            }
            const std::string where = path.filename().string() + ":" + entry.name;
            const std::uint32_t size = J3D::animFileSize(file.data());
            Buffer image(size);
            const char* pError = size <= file.size() ? J3D::makeHostAnimImage(file.data(), size, image.data()) : "header size larger than resource";
            std::string detail;
            if (pError == nullptr && !checkImage(image, &detail)) {
                pError = detail.c_str();
            }
            if (pError != nullptr) {
                std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pError);
                failed++;
                continue;
            }
            kinds[std::string(reinterpret_cast< const char* >(&file[4]), 4)]++;
            converted++;
        }
    }

    std::printf("J3D animations: %zu converted and checked, %zu failed (", converted, failed);
    for (const auto& kind : kinds) {
        std::printf(" %s %zu", kind.first.c_str(), kind.second);
    }
    std::printf(" )\n");
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
        std::fprintf(stderr, "%d J3D animation check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("J3D animation conversion tests passed");
    return 0;
}

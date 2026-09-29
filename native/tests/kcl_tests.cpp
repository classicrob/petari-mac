// KCL collision resource conversion tests (native/resource/kcl_collision.cpp).
//
// Default: a synthetic big-endian KCL (two-level octree with shared prism lists), plus
// malformed-input rejection and the "already initialized" marker. With --assets FILES: every
// .kcl in ObjectData and StageData archives is converted, and lookups through the octree
// (the same walk as KCollisionServer::searchBlock) are checked over each file's bounds.
// Links: native/resource/kcl_collision.cpp, petari_resources.
#include "archive.hpp"
#include <petari/endian.hpp>
#include <petari/kcl_collision.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Buffer = std::vector< std::uint8_t >;
namespace KCL = PetariNative::KCL;

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

// Mirrors KCollisionServer::searchBlock on a converted resource. Returns the byte offset of
// the prism list (pointing at the u16 before the first entry).
static std::size_t searchBlock(const Buffer& data, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const std::size_t octree = KCL::octreeOffset(data.data());
    const std::int32_t widthShift = hostAt< std::int32_t >(data, 0x2C);
    const std::int32_t xShift = hostAt< std::int32_t >(data, 0x30);
    const std::int32_t xyShift = hostAt< std::int32_t >(data, 0x34);
    std::int32_t shift = widthShift;
    std::int32_t offset = static_cast< std::int32_t >((((z >> widthShift) << xyShift) | ((y >> widthShift) << xShift) | (x >> widthShift)) * 4);
    if (xyShift == -1 && xShift == -1) {
        offset = 0;
    }

    std::size_t base = octree;
    while ((offset = hostAt< std::int32_t >(data, base + offset)) >= 0) {
        base += offset;
        shift--;
        offset = static_cast< std::int32_t >(((((z >> shift) & 1) << 2) | (((y >> shift) & 1) << 1) | ((x >> shift) & 1)) * 4);
    }
    return base + (static_cast< std::uint32_t >(offset) & 0x7FFFFFFF);
}

// Synthetic KCL: 3 positions, 3 normals, 2 prisms (+ prism 0 overlapping the last 16 bytes
// of the normals, as in disc files), a 2x1x1 top level
// whose first cell is a leaf and whose second cell branches into 8 leaves sharing lists.
static Buffer buildSyntheticKcl() {
    Buffer data(KCL::cHeaderSize);
    const std::uint32_t pos = KCL::cHeaderSize;
    const std::uint32_t nrm = pos + 3 * 12;
    const std::uint32_t prism = nrm + 3 * 12 - 16;
    const std::uint32_t octree = prism + 3 * 16;
    data.resize(octree);

    const float positions[9] = {0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f, -250.5f};
    for (int i = 0; i < 9; i++) {
        put32(data, pos + i * 4, floatBits(positions[i]));
    }
    const float normals[9] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f};
    for (int i = 0; i < 9; i++) {
        put32(data, nrm + i * 4, floatBits(normals[i]));
    }
    for (int i = 1; i <= 2; i++) {
        const std::uint32_t p = prism + i * 16;
        put32(data, p, floatBits(10.0f * i));
        put16(data, p + 4, static_cast< std::uint16_t >(i));
        put16(data, p + 6, 0);
        put16(data, p + 8, 1);
        put16(data, p + 10, 0);
        put16(data, p + 12, 1);
        put16(data, p + 14, static_cast< std::uint16_t >(0x100 + i));
    }

    // Octree: [cell0 leaf][cell1 branch -> 8 child words][lists]
    // Lists (u16): 0, 1, 2, 0 | 2, 0  (the second list shares the tail "2, 0" region layout).
    const std::uint32_t branch = octree + 8;
    const std::uint32_t lists = branch + 8 * 4;
    data.resize(lists + 12);
    put16(data, lists + 0, 0);  // u16 before the first entry
    put16(data, lists + 2, 1);
    put16(data, lists + 4, 2);
    put16(data, lists + 6, 0);
    const std::uint32_t listA = lists - octree;      // entries 1, 2
    const std::uint32_t listB = lists + 2 - octree;  // entries 2 (shares the tail of list A)
    put32(data, octree + 0, 0x80000000 | listA);
    put32(data, octree + 4, branch - octree);
    for (int i = 0; i < 8; i++) {
        // Child words are relative to the branch node.
        const std::uint32_t target = (i & 1) ? listB : listA;
        put32(data, branch + i * 4, 0x80000000 | (target - (branch - octree)));
    }

    put32(data, 0x00, pos);
    put32(data, 0x04, nrm);
    put32(data, 0x08, prism);
    put32(data, 0x0C, octree);
    put32(data, 0x10, floatBits(30.0f));
    put32(data, 0x14, floatBits(-1000.0f));
    put32(data, 0x18, floatBits(-2000.0f));
    put32(data, 0x1C, floatBits(-3000.0f));
    put32(data, 0x20, 0xFFFFFE00);  // x: 2 cells of 256
    put32(data, 0x24, 0xFFFFFF00);  // y: 1 cell
    put32(data, 0x28, 0xFFFFFF00);  // z: 1 cell
    put32(data, 0x2C, 8);           // block width shift
    put32(data, 0x30, 1);           // x shift
    put32(data, 0x34, 1);           // xy shift
    return data;
}

static std::vector< std::uint16_t > readList(const Buffer& data, std::size_t list) {
    std::vector< std::uint16_t > entries;
    for (std::size_t entry = list + 2; hostAt< std::uint16_t >(data, entry) != 0; entry += 2) {
        entries.push_back(hostAt< std::uint16_t >(data, entry));
    }
    return entries;
}

static void testSynthetic() {
    Buffer data = buildSyntheticKcl();
    const Buffer original = data;
    check(!KCL::isHostResource(data.data()), "big-endian resource is not initialized");

    Buffer bad = data;
    put32(bad, 0x04, 0x7FFFFFF0);
    check(KCL::convertInPlace(bad.data(), static_cast< std::uint32_t >(bad.size())) != nullptr, "out-of-order sections rejected");
    bad = data;
    put16(bad, data.size() - 8, 7);  // list entry referencing a missing prism
    check(KCL::convertInPlace(bad.data(), static_cast< std::uint32_t >(bad.size())) != nullptr, "bad prism index rejected");
    bad = data;
    put32(bad, KCL::cHeaderSize + 3 * 12 + 2 * 12 + 3 * 16, 0x80000000 | 0x10000);
    const Buffer badBefore = bad;
    check(KCL::convertInPlace(bad.data(), static_cast< std::uint32_t >(bad.size())) != nullptr, "list outside resource rejected");
    check(bad == badBefore, "rejected resource left unmodified");

    const char* pError = KCL::convertInPlace(data.data(), static_cast< std::uint32_t >(data.size()));
    check(pError == nullptr, pError != nullptr ? pError : "conversion");
    check(KCL::isHostResource(data.data()), "converted resource is marked initialized");
    check(hostAt< std::int32_t >(data, 0) < 0, "Wii isBinaryInitialized test holds");
    check(KCL::convertInPlace(data.data(), static_cast< std::uint32_t >(data.size())) != nullptr, "second conversion refused");

    const std::size_t pos = KCL::positionOffset(data.data());
    check(pos == KCL::cHeaderSize && hostAt< float >(data, pos + 12) == 100.0f && hostAt< float >(data, pos + 32) == -250.5f, "positions");
    const std::size_t prism = KCL::prismOffset(data.data());
    check(hostAt< float >(data, KCL::normalOffset(data.data()) + 32) == -1.0f, "normal overlapping prism 0 converted once");
    check(hostAt< float >(data, prism + 32) == 20.0f && hostAt< std::uint16_t >(data, prism + 32 + 4) == 2 &&
              hostAt< std::uint16_t >(data, prism + 32 + 14) == 0x102,
          "prisms");
    check(hostAt< float >(data, 0x10) == 30.0f && hostAt< float >(data, 0x1C) == -3000.0f && hostAt< std::int32_t >(data, 0x20) == -512 &&
              hostAt< std::int32_t >(data, 0x2C) == 8,
          "header scalars");

    check(readList(data, searchBlock(data, 5, 5, 5)) == std::vector< std::uint16_t >({1, 2}), "leaf cell lookup");
    check(readList(data, searchBlock(data, 256 + 128, 0, 0)) == std::vector< std::uint16_t >({2}), "branch child with shared list (odd x)");
    check(readList(data, searchBlock(data, 256, 128, 128)) == std::vector< std::uint16_t >({1, 2}), "branch child (even x)");
    check(original.size() == data.size(), "size unchanged");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

static bool checkConverted(const Buffer& data, const std::string& where, std::size_t* pLookups) {
    const std::size_t pos = KCL::positionOffset(data.data());
    const std::size_t nrm = KCL::normalOffset(data.data());
    const std::size_t prism = KCL::prismOffset(data.data());
    const std::size_t octree = KCL::octreeOffset(data.data());
    const std::uint32_t prismCount = static_cast< std::uint32_t >((octree - prism) / 16);

    for (std::size_t offset = pos; offset < prism + 16; offset += 4) {
        const float value = hostAt< float >(data, offset);
        if (!std::isfinite(value) || std::fabs(value) > 1.0e7f) {
            std::fprintf(stderr, "FAIL: %s: implausible position/normal value\n", where.c_str());
            return false;
        }
    }
    for (std::size_t offset = nrm; offset < prism + 16; offset += 12) {
        const float x = hostAt< float >(data, offset), y = hostAt< float >(data, offset + 4), z = hostAt< float >(data, offset + 8);
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length > 0.0f && std::fabs(length - 1.0f) > 0.02f) {
            std::fprintf(stderr, "FAIL: %s: normal of length %f\n", where.c_str(), length);
            return false;
        }
    }

    // Walk the octree over the whole grid at block resolution and at a finer offset.
    const std::uint32_t xCells = (static_cast< std::uint32_t >(~hostAt< std::int32_t >(data, 0x20))) + 1;
    const std::uint32_t yCells = (static_cast< std::uint32_t >(~hostAt< std::int32_t >(data, 0x24))) + 1;
    const std::uint32_t zCells = (static_cast< std::uint32_t >(~hostAt< std::int32_t >(data, 0x28))) + 1;
    const std::uint32_t step = std::max< std::uint32_t >(1u << std::max(hostAt< std::int32_t >(data, 0x2C) - 2, 0), 1);
    std::size_t lookups = 0;
    for (std::uint32_t z = 0; z < zCells && lookups < 200000; z += step) {
        for (std::uint32_t y = 0; y < yCells && lookups < 200000; y += step) {
            for (std::uint32_t x = 0; x < xCells && lookups < 200000; x += step) {
                const std::size_t list = searchBlock(data, x, y, z);
                for (std::uint16_t entry : readList(data, list)) {
                    if (entry == 0 || entry >= prismCount) {
                        std::fprintf(stderr, "FAIL: %s: lookup reached prism %u of %u\n", where.c_str(), entry, prismCount);
                        return false;
                    }
                }
                lookups++;
            }
        }
    }
    *pLookups += lookups;
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

    std::size_t converted = 0, failed = 0, lookups = 0;
    for (const fs::path& path : archives) {
        Buffer bytes;
        if (!readFile(path, &bytes)) {
            continue;
        }
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            const auto& entry = archive.entries()[i];
            if (entry.isDirectory() || entry.name.size() < 4 || entry.name.substr(entry.name.size() - 4) != ".kcl") {
                continue;
            }
            Buffer data = archive.resourceData(i);
            const std::string where = path.filename().string() + ":" + entry.name;
            if (const char* pError = KCL::convertInPlace(data.data(), static_cast< std::uint32_t >(data.size()))) {
                std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pError);
                failed++;
                continue;
            }
            if (!checkConverted(data, where, &lookups)) {
                failed++;
                continue;
            }
            converted++;
        }
    }

    std::printf("KCL: %zu converted and checked (%zu octree lookups), %zu failed\n", converted, lookups, failed);
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
        std::fprintf(stderr, "%d KCL check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("KCL tests passed");
    return 0;
}

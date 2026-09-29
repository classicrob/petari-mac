// JPA particle container conversion tests (native/resource/jpa_resource.cpp).
//
// Default: a synthetic big-endian JPAC2-10 container with one resource holding every block
// type (BSP1 with texture-matrix and color key tables, BEM1, FLD1, KFA1, ESP1, SSP1, ETX1,
// TDB1) and one TEX1 texture, plus malformed-input rejection. With --assets FILES: every
// JPAC container in the game data is converted and its blocks are walked in host order.
// Links: native/resource/jpa_resource.cpp, petari_resources, petari_host_runtime.
#include "archive.hpp"
#include <petari/endian.hpp>
#include <petari/jpa_resource.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Buffer = std::vector< std::uint8_t >;
namespace JPA = PetariNative::JPA;

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

static Buffer block(const char* pTag, std::size_t size) {
    Buffer data(size, 0);
    std::memcpy(data.data(), pTag, 4);
    put32(data, 4, static_cast< std::uint32_t >(size));
    return data;
}

struct Synthetic {
    Buffer mFile;
    std::size_t mBlocks[8];  // offsets of BSP1, BEM1, FLD1, KFA1, ESP1, SSP1, ETX1, TDB1
    std::size_t mTexture;
};

static Synthetic buildContainer() {
    Synthetic out;
    std::vector< Buffer > blocks;

    Buffer bsp = block("BSP1", 0x34 + 0x28 + 2 * 6 + 2);
    put32(bsp, 0x08, 0x01000000);  // texture coordinate matrix animation
    put16(bsp, 0x0C, 0x34 + 0x28);
    put16(bsp, 0x0E, 0x34 + 0x28 + 6);
    put32(bsp, 0x10, floatBits(25.0f));
    put32(bsp, 0x14, floatBits(-3.0f));
    put16(bsp, 0x18, 0x0145);
    bsp[0x21] = 0x0A;  // prm and env color animation
    bsp[0x22] = 1;
    bsp[0x23] = 1;
    put16(bsp, 0x24, 30);
    for (int i = 0; i < 10; i++) {
        put32(bsp, 0x34 + i * 4, floatBits(0.5f * i));
    }
    put16(bsp, 0x34 + 0x28, 7);
    bsp[0x34 + 0x28 + 2] = 0x11;
    put16(bsp, 0x34 + 0x28 + 6, 0xFFF0);
    blocks.push_back(bsp);

    Buffer bem = block("BEM1", 0x7C);
    put32(bem, 0x08, 0x12345678);
    put32(bem, 0x10, floatBits(2.0f));
    put32(bem, 0x64, floatBits(0.75f));
    put16(bem, 0x68, 0x8000);
    put16(bem, 0x76, 12);
    blocks.push_back(bem);

    Buffer fld = block("FLD1", 0x44);
    put32(fld, 0x08, 0x103);
    put32(fld, 0x24, floatBits(9.8f));
    put32(fld, 0x3C, floatBits(1.0f));
    fld[0x40] = 3;
    blocks.push_back(fld);

    Buffer kfa = block("KFA1", 0x0C + 2 * 16);
    kfa[0x09] = 2;
    kfa[0x0B] = 1;
    for (int i = 0; i < 8; i++) {
        put32(kfa, 0x0C + i * 4, floatBits(1.0f + i));
    }
    blocks.push_back(kfa);

    Buffer esp = block("ESP1", 0x60);
    put32(esp, 0x0C, floatBits(0.1f));
    put16(esp, 0x28, 0xFFFE);
    put32(esp, 0x5C, floatBits(-1.0f));
    blocks.push_back(esp);

    Buffer ssp = block("SSP1", 0x48);
    put32(ssp, 0x0C, floatBits(4.0f));
    put32(ssp, 0x30, floatBits(0.25f));
    ssp[0x34] = 0xAA;
    put32(ssp, 0x3C, floatBits(0.5f));
    put16(ssp, 0x40, 60);
    put16(ssp, 0x46, 0xF000);
    blocks.push_back(ssp);

    Buffer etx = block("ETX1", 0x28);
    put32(etx, 0x0C, floatBits(0.5f));
    put32(etx, 0x20, floatBits(-0.5f));
    etx[0x24] = 0xFE;
    blocks.push_back(etx);

    Buffer tdb = block("TDB1", 0x0C);
    put16(tdb, 0x08, 0);
    put16(tdb, 0x0A, 0x0100);
    blocks.push_back(tdb);

    Buffer& file = out.mFile;
    file.assign(0x10, 0);
    std::memcpy(file.data(), "JPAC2-10", 8);
    put16(file, 0x08, 1);
    put16(file, 0x0A, 1);
    const std::size_t resource = file.size();
    file.resize(resource + 8);
    put16(file, resource, 42);
    put16(file, resource + 2, static_cast< std::uint16_t >(blocks.size()));
    file[resource + 4] = 1;  // field blocks
    file[resource + 5] = 1;  // key blocks
    file[resource + 6] = 2;  // TDB1 entries
    for (std::size_t i = 0; i < blocks.size(); i++) {
        out.mBlocks[i] = file.size();
        file.insert(file.end(), blocks[i].begin(), blocks[i].end());
    }
    while (file.size() % 0x20 != 0) {
        file.push_back(0);
    }

    out.mTexture = file.size();
    put32(file, 0x0C, static_cast< std::uint32_t >(out.mTexture));
    Buffer tex = block("TEX1", 0x60);
    std::memcpy(&tex[0x0C], "sample", 6);
    tex[0x20] = 6;                // GX_TF_RGBA8 ResTIMG
    put16(tex, 0x22, 4);
    put16(tex, 0x24, 2);
    put32(tex, 0x3C, 0x20);       // image data right after the ResTIMG header
    for (int i = 0; i < 0x20; i++) {
        tex[0x40 + i] = static_cast< std::uint8_t >(i);
    }
    file.insert(file.end(), tex.begin(), tex.end());
    return out;
}

static void testSynthetic() {
    const Synthetic synthetic = buildContainer();
    const Buffer& file = synthetic.mFile;
    check(JPA::classifyImage(file.data(), static_cast< std::uint32_t >(file.size())) == JPA::ImageKind::BigEndian, "classify container");

    Buffer image(file.size());
    const char* pError = JPA::makeHostImage(file.data(), static_cast< std::uint32_t >(file.size()), image.data());
    check(pError == nullptr, pError != nullptr ? pError : "conversion");
    check(JPA::classifyImage(image.data(), static_cast< std::uint32_t >(image.size())) == JPA::ImageKind::Host, "classify host image");
    check(hostAt< std::uint32_t >(image, 4) == 0x322D3130 && hostAt< std::uint16_t >(image, 8) == 1 &&
              hostAt< std::uint32_t >(image, 0x0C) == synthetic.mTexture,
          "container header (version word compared as u32 by the loader)");
    check(hostAt< std::uint16_t >(image, 0x10) == 42 && hostAt< std::uint16_t >(image, 0x12) == 8 && image[0x16] == 2, "resource header");

    const std::size_t* pBlocks = synthetic.mBlocks;
    check(hostAt< std::uint32_t >(image, pBlocks[0]) == 0x42535031 && hostAt< std::uint32_t >(image, pBlocks[0] + 4) == 0x34 + 0x28 + 14,
          "block tag and size in host order");
    check(hostAt< std::uint32_t >(image, pBlocks[0] + 8) == 0x01000000 && hostAt< float >(image, pBlocks[0] + 0x14) == -3.0f &&
              hostAt< std::uint16_t >(image, pBlocks[0] + 0x18) == 0x0145 && hostAt< std::int16_t >(image, pBlocks[0] + 0x24) == 30 &&
              hostAt< float >(image, pBlocks[0] + 0x34 + 36) == 4.5f,
          "BSP1 fields and texture matrix table");
    check(hostAt< std::int16_t >(image, pBlocks[0] + 0x5C) == 7 && image[pBlocks[0] + 0x5E] == 0x11 &&
              hostAt< std::int16_t >(image, pBlocks[0] + 0x62) == -16,
          "BSP1 color key tables (s16 frame, GXColor bytes)");
    check(hostAt< std::uint32_t >(image, pBlocks[1] + 8) == 0x12345678 && hostAt< float >(image, pBlocks[1] + 0x64) == 0.75f &&
              hostAt< std::int16_t >(image, pBlocks[1] + 0x68) == -32768 && hostAt< std::uint16_t >(image, pBlocks[1] + 0x76) == 12,
          "BEM1 dynamics");
    check(hostAt< std::uint32_t >(image, pBlocks[2] + 8) == 0x103 && std::fabs(hostAt< float >(image, pBlocks[2] + 0x24) - 9.8f) < 1e-6f &&
              image[pBlocks[2] + 0x40] == 3,
          "FLD1 field");
    check(image[pBlocks[3] + 9] == 2 && hostAt< float >(image, pBlocks[3] + 0x0C + 28) == 8.0f, "KFA1 keys");
    check(hostAt< std::int16_t >(image, pBlocks[4] + 0x28) == -2 && hostAt< float >(image, pBlocks[4] + 0x5C) == -1.0f, "ESP1 extra shape");
    check(hostAt< float >(image, pBlocks[5] + 0x30) == 0.25f && image[pBlocks[5] + 0x34] == 0xAA && hostAt< std::int16_t >(image, pBlocks[5] + 0x40) == 60 &&
              hostAt< std::int16_t >(image, pBlocks[5] + 0x46) == -4096,
          "SSP1 child shape");
    check(hostAt< float >(image, pBlocks[6] + 0x20) == -0.5f && static_cast< std::int8_t >(image[pBlocks[6] + 0x24]) == -2, "ETX1 indirect matrix");
    check(hostAt< std::uint16_t >(image, pBlocks[7] + 0x0A) == 0x0100, "TDB1 texture indices");

    const std::size_t timg = synthetic.mTexture + 0x20;
    check(hostAt< std::uint32_t >(image, synthetic.mTexture) == 0x54455831 && std::strcmp(reinterpret_cast< const char* >(&image[synthetic.mTexture + 0x0C]), "sample") == 0 &&
              hostAt< std::uint16_t >(image, timg + 2) == 4 && hostAt< std::uint32_t >(image, timg + 0x1C) == 0x20 && image[timg + 0x21] == 1,
          "TEX1 ResTIMG header host order, image bytes unchanged");

    auto rejects = [](const Buffer& bad) {
        Buffer out(bad.size());
        return JPA::makeHostImage(bad.data(), static_cast< std::uint32_t >(bad.size()), out.data()) != nullptr;
    };
    Buffer bad = file;
    std::memcpy(&bad[pBlocks[2]], "XYZ1", 4);
    check(rejects(bad), "unknown block rejected");
    bad = file;
    put32(bad, pBlocks[0] + 4, 0x100000);
    check(rejects(bad), "oversized block rejected");
    bad = file;
    bad[pBlocks[3] + 9] = 100;
    check(rejects(bad), "key count past block rejected");
    bad = file;
    put32(bad, timg + 0x1C, 0x1000);
    check(rejects(bad), "texture data outside block rejected");
    bad = file;
    put16(bad, pBlocks[0] + 0x0C, 0x7000);
    check(rejects(bad), "color table outside block rejected");
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

// Walks a converted container with host-order reads, the way JPAResourceLoader does.
static bool walkImage(const Buffer& image, std::size_t* pBlocks, std::size_t* pTextures) {
    const std::uint16_t resources = hostAt< std::uint16_t >(image, 8);
    const std::uint16_t textures = hostAt< std::uint16_t >(image, 0x0A);
    std::size_t offset = 0x10;
    for (std::uint16_t i = 0; i < resources; i++) {
        const std::uint16_t blocks = hostAt< std::uint16_t >(image, offset + 2);
        offset += 8;
        for (std::uint16_t j = 0; j < blocks; j++) {
            const std::uint32_t size = hostAt< std::uint32_t >(image, offset + 4);
            if (size < 8 || offset + size > image.size()) {
                return false;
            }
            offset += size;
            (*pBlocks)++;
        }
    }
    offset = hostAt< std::uint32_t >(image, 0x0C);
    for (std::uint16_t i = 0; i < textures; i++) {
        const std::size_t timg = offset + 0x20;
        const std::uint16_t width = hostAt< std::uint16_t >(image, timg + 2);
        const std::uint16_t height = hostAt< std::uint16_t >(image, timg + 4);
        if (hostAt< std::uint32_t >(image, offset) != 0x54455831 || width == 0 || width > 1024 || height == 0 || height > 1024) {
            return false;
        }
        offset += hostAt< std::uint32_t >(image, offset + 4);
        (*pTextures)++;
    }
    return true;
}

static void testAssets(const std::filesystem::path& filesRoot) {
    namespace fs = std::filesystem;
    std::size_t containers = 0, blocks = 0, textures = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(filesRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".arc") {
            continue;
        }
        Buffer bytes;
        if (!readFile(entry.path(), &bytes)) {
            continue;
        }
        PetariNative::Resource::Archive archive;
        try {
            archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        } catch (const std::exception&) {
            continue;  // not a RARC (e.g. U8 archives such as HomeButton data)
        }
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            if (archive.entries()[i].isDirectory()) {
                continue;
            }
            const Buffer file = archive.resourceData(i);
            if (file.size() < 8 || std::memcmp(file.data(), "JPAC", 4) != 0) {
                continue;
            }
            const std::string where = entry.path().filename().string() + ":" + archive.entries()[i].name;
            Buffer image(file.size());
            const char* pError = JPA::makeHostImage(file.data(), static_cast< std::uint32_t >(file.size()), image.data());
            if (pError == nullptr && !walkImage(image, &blocks, &textures)) {
                pError = "host image walk failed";
            }
            if (pError != nullptr) {
                std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pError);
                ++sFailures;
                continue;
            }
            containers++;
        }
    }
    std::printf("JPA: %zu containers converted (%zu blocks, %zu textures)\n", containers, blocks, textures);
    check(containers > 0, "no JPAC containers found");
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
        std::fprintf(stderr, "%d JPA check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("JPA conversion tests passed");
    return 0;
}

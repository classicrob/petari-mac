// JPA particle loading test: the disc particle containers are converted to host images and
// loaded through the real JPAResourceManager / JPAResourceLoader, which builds every
// JPAResource (with JPAResource::init) and JPATexture. Checks resource and texture counts,
// user indices, shape blocks and texture headers against the container.
//
// Usage: petari_jpa_load_tests --assets GAME_FILES_DIR
// Links: src/JSystem/JParticle, JUTTexture/JUTPalette, native/resource/jpa_resource.cpp,
// petari_j3d (GX, JKernel, core).
#include "archive.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JParticle/JPAResource.hpp>
#include <JSystem/JParticle/JPAResourceManager.hpp>
#include <JSystem/JParticle/JPATexture.hpp>
#include <petari/boot.hpp>
#include <petari/endian.hpp>
#include <petari/jpa_resource.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Buffer = std::vector< std::uint8_t >;

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

// Loads one container on its own heap. Returns a failure reason or nullptr.
static const char* loadContainer(const Buffer& file, std::size_t* pResources, std::size_t* pTextures) {
    const u16 resourceNum = PetariNative::readU16BE(&file[8]);
    const u16 textureNum = PetariNative::readU16BE(&file[0x0A]);

    // Native operator new allocates from the current JKR heap on game threads: nothing
    // test-owned is created between becomeCurrentHeap and destroy.
    JKRExpHeap* pHeap = JKRExpHeap::create(32 * 1024 * 1024, JKRHeap::sRootHeap, false);
    JKRHeap* pPrevious = pHeap->becomeCurrentHeap();
    const char* pReason = nullptr;

    u8* pImage = new (pHeap, 0x20) u8[file.size()];
    if (const char* pError = PetariNative::JPA::makeHostImage(file.data(), static_cast< u32 >(file.size()), pImage)) {
        pReason = pError;
    } else {
        JPAResourceManager* pManager = new (pHeap, 0) JPAResourceManager(pImage, pHeap);
        if (pManager->mResNum != resourceNum || pManager->mTexNum != textureNum) {
            pReason = "resource or texture count differs from the container";
        }
        for (u16 i = 0; pReason == nullptr && i < pManager->mResNum; i++) {
            JPAResource* pResource = pManager->mpResArr[i];
            if (pResource == nullptr || pResource->getBsp() == nullptr) {
                pReason = "resource without a base shape";
            } else if (pManager->getResource(pResource->getUsrIdx()) == nullptr) {
                pReason = "resource not found by its user index";
            }
        }
        for (u16 i = 0; pReason == nullptr && i < pManager->mTexNum; i++) {
            JPATexture* pTexture = pManager->mpTexArr[i];
            const ResTIMG* pTimg = pTexture != nullptr ? pTexture->getJUTTexture()->getTexInfo() : nullptr;
            if (pTimg == nullptr || pTimg->mWidth == 0 || pTimg->mWidth > 1024 || pTimg->mHeight == 0 || pTimg->mHeight > 1024 ||
                pTexture->getName()[0] == '\0') {
                pReason = "texture header or name not decoded";
            }
        }
        *pResources += pManager->mResNum;
        *pTextures += pManager->mTexNum;
    }

    pPrevious->becomeCurrentHeap();
    JKRHeap::destroy(pHeap);
    return pReason;
}

int main(int argc, char** argv) {
    if (argc != 3 || std::strcmp(argv[1], "--assets") != 0) {
        std::fprintf(stderr, "Usage: %s --assets GAME_FILES_DIR\n", argv[0]);
        return 2;
    }
    OSInit();
    JKRExpHeap::createRoot(1, false);

    namespace fs = std::filesystem;
    std::size_t containers = 0, resources = 0, textures = 0, failed = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(argv[2])) {
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
            continue;
        }
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            if (archive.entries()[i].isDirectory()) {
                continue;
            }
            const Buffer file = archive.resourceData(i);
            if (file.size() < 0x10 || std::memcmp(file.data(), "JPAC2-10", 8) != 0) {
                continue;
            }
            const std::string where = entry.path().filename().string() + ":" + archive.entries()[i].name;
            if (const char* pReason = loadContainer(file, &resources, &textures)) {
                std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pReason);
                failed++;
            }
            containers++;
        }
    }

    std::printf("JPA: %zu containers loaded (%zu resources, %zu textures), %zu failed\n", containers, resources, textures, failed);
    if (failed != 0 || containers == 0) {
        return 1;
    }
    std::puts("JPA load tests passed");
    return 0;
}

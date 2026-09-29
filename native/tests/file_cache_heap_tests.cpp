// File cache host-image heap test (native): the game's HeapMemoryWatcher with the real stage
// archive set of HeavensDoorGalaxy, the stage where the file cache first overflowed.
//
// Archives are placed as the game places scene archives (MR::getAproposHeapForSceneArchive:
// the file cache unless under 3% free, then the scene heap), each as its decompressed bytes
// (a JKRMemArchive keeps them). Their J3D files are then loaded through the J3D loaders with
// the archive's heap current, as ResourceHolder does. Checks:
// - every host-layout copy of a file in a cached archive is in the file cache's companion
//   (HeapMemoryWatcher::mFileCacheHostImageHeap), exactly its file size (no padding, one copy
//   per load), and the file cache itself only receives J3D objects;
// - a file from a scene-heap archive is copied on the current heap, not the companion;
// - after HeapMemoryWatcher::destroyGameHeap both heaps are gone and the resolver returns null.
// It also reports the native J3D object bytes the file cache receives (the growth the
// companion does not cover) and the file cache's remaining headroom.
//
// Usage: petari_file_cache_heap_tests --assets GAME_FILES_DIR
// Links: src/Game/System/HeapMemoryWatcher.cpp, src/Game/Util/MemoryUtil.cpp,
// native/tests/heap_diagnostics.cpp, petari_j3d (J3D loaders with the host-image hook, JKernel,
// OS). WPADGetWorkMemorySize is defined below.
#include "archive.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <petari/boot.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>
#include <petari/host_image_heap.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace MR {
    JKRHeap* getAproposHeapForSceneArchive(f32);
}

// HeapMemoryWatcher::createHeaps sizes the WPAD heap from this; the native WPAD layer (not
// linked here) reports 0 as well.
extern "C" u32 WPADGetWorkMemorySize(void) {
    return 0;
}

using Buffer = std::vector< std::uint8_t >;

static int sFailures = 0;
static bool sLoadedCrashModel = false;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

static s32 usedBytes(JKRHeap* pHeap) {
    return static_cast< s32 >(static_cast< u8* >(pHeap->getEndAddr()) - static_cast< u8* >(pHeap->getStartAddr())) - pHeap->getTotalFreeSize();
}

static bool isModel(const u8* p, bool* pIsBdl) {
    *pIsBdl = std::memcmp(p, "J3D2bdl", 7) == 0;
    return *pIsBdl || std::memcmp(p, "J3D2bmd", 7) == 0;
}

// Loads one J3D file with the current heap = pHeap, as ResourceHolder::createAndRegisterObject.
static bool loadJ3D(const u8* pData) {
    bool isBdl = false;
    if (isModel(pData, &isBdl)) {
        return isBdl ? J3DModelLoaderDataBase::loadBinaryDisplayList(pData, J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials) != nullptr
                     : J3DModelLoaderDataBase::load(pData, J3DMLF_Material_PE_Full | J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials |
                                                               J3DMLF_21) != nullptr;
    }
    return J3DAnmLoaderDataBase::load(pData) != nullptr;
}

int main(int argc, char** argv) {
    if (argc != 3 || std::strcmp(argv[1], "--assets") != 0) {
        std::fprintf(stderr, "Usage: %s --assets GAME_FILES_DIR\n", argv[0]);
        return 2;
    }
    namespace fs = std::filesystem;
    const fs::path files = argv[2];

    // HeavensDoorGalaxy's zones and the ObjectData archives of the objects they place (placed
    // names without an archive of their own, such as planet parts sharing another archive, are
    // not listed). Every listed archive must exist.
    const char* cStageArchives[] = {"StageData/HeavensDoorGalaxy.arc", "StageData/HeavensBlackHoleZone.arc",
                                    "StageData/HeavensDoorInsideZone.arc", "StageData/HeavensDoorLargeZone.arc",
                                    "StageData/HeavensDoorMiddleZone.arc", "StageData/HeavensDoorMysteriousZone.arc",
                                    "StageData/HeavensDoorSmallZone.arc"};
    const char* cObjects[] = {"BlackHole", "Butterfly", "CapsuleCage", "Coin", "CrystalCageM", "CrystalCageS",
                              "EarthenPipe", "FlipPanelReverse", "GrandStar", "HeavensDoorAppearStepA",
                              "HeavensDoorAppearStepAAfter", "HeavensDoorBlackHolePlanet", "HeavensDoorFlowerA", "HeavensDoorHouseDoor",
                              "HeavensDoorInsideCage", "HeavensDoorInsidePlanet", "HeavensDoorInsidePlanetPartsA",
                              "HeavensDoorInsideRotatePartsA", "HeavensDoorInsideRotatePartsB", "HeavensDoorInsideRotatePartsC",
                              "HeavensDoorMiddlePlanet", "HeavensDoorMiddleRotatePartsA", "HeavensDoorMiddleRotatePartsB",
                              "HeavensDoorMysteriousPlanet", "HeavensDoorSmallPlanet", "KoopaJrNormalShipA", "Kuribo", "KuriboChief",
                              "MoonPlanet", "NeedlePlant", "Petari", "PowerStar", "PunchingKinoko", "PurpleCoin", "Rosetta", "ShockWaveGenerator", "ShootingStar", "SmallStone", "SpinDriver", "StarPiece", "SuperSpinDriver",
                              "Tico", "TicoBaby", "WarpPod", "YellowChip"};
    std::vector< fs::path > archives;
    for (const char* pPath : cStageArchives) {
        archives.push_back(files / pPath);
    }
    for (const char* pName : cObjects) {
        archives.push_back(files / "ObjectData" / (std::string(pName) + ".arc"));
    }

    // Boot the game's heaps as petari_game_main and initializeScene do.
    OSInit();
    HeapMemoryWatcher::createRootHeap();
    SingletonHolder< HeapMemoryWatcher >::init();
    HeapMemoryWatcher* pWatcher = SingletonHolder< HeapMemoryWatcher >::get();
    pWatcher->createFileCacheHeapOnGameHeap(0x1040400);
    pWatcher->createSceneHeapOnGameHeap();
    JKRHeap* pCache = pWatcher->mFileCacheHeap;
    JKRHeap* pCompanion = pWatcher->mFileCacheHostImageHeap;
    check(pCache != nullptr && pCompanion != nullptr, "file cache and its host-image companion created");
    check(JKRHeap::findFromRoot(pCompanion) != pCache, "the companion is not inside the file cache");

    std::size_t cachedArchives = 0, sceneArchives = 0, cachedFiles = 0, sceneFiles = 0, archiveBytes = 0, j3dBytes = 0;
    s32 objectBytes = 0;
    for (const fs::path& path : archives) {
        // Test bookkeeping uses the host allocator (the JKR root is fully handed out to the game
        // heaps); the game allocations below (resident archive, J3D loads) are explicit JKR
        // allocations or run with the scope closed.
        Buffer data;
        PetariNative::Resource::Archive archive;
        {
            PetariNative::HostAllocationScope hostAllocations;
            Buffer bytes;
            if (!readFile(path, &bytes)) {
                std::fprintf(stderr, "FAIL: missing archive %s\n", path.c_str());
                ++sFailures;
                continue;
            }
            data = std::memcmp(bytes.data(), "Yaz0", 4) == 0 ? PetariNative::Resource::decompress({bytes.data(), bytes.size()}) : bytes;
            archive = PetariNative::Resource::Archive::parse({data.data(), data.size()});
        }
        JKRHeap* pHeap = MR::getAproposHeapForSceneArchive(0.03f);
        u8* pResident = new (pHeap, 0x20) u8[data.size()];
        std::memcpy(pResident, data.data(), data.size());
        const bool isCached = pHeap == pCache;
        (isCached ? cachedArchives : sceneArchives)++;
        archiveBytes += isCached ? data.size() : 0;

        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            const auto& rEntry = archive.entries()[i];
            if (rEntry.isDirectory()) {
                continue;
            }
            // The file's bytes inside the resident archive: RARC data section (0x20 + header
            // word 0x0C) plus the entry offset. Files stored compressed inside the archive are
            // not J3D files the loaders read in place, so they are skipped.
            const u8* pFound = pResident + 0x20 + PetariNative::readU32BE(pResident + 0x0C) + rEntry.offset;
            const std::size_t fileSize = rEntry.size;
            if (fileSize < 8 || pFound + fileSize > pResident + data.size()) {
                continue;
            }
            bool isBdl = false;
            if (!isModel(pFound, &isBdl) && std::memcmp(pFound, "J3D1", 4) != 0) {
                continue;
            }

            JKRHeap* pExpected = PetariNative::J3D::resolveHostImageHeap(pFound);
            check(isCached ? pExpected == pCompanion : pExpected == nullptr, "resolver routes by the heap holding the source file");

            JKRHeap* pPrevious = pHeap->becomeCurrentHeap();
            const s32 companionBefore = usedBytes(pCompanion), cacheBefore = usedBytes(pCache), sceneBefore = usedBytes(pHeap);
            const bool loaded = loadJ3D(pFound);
            const s32 companionDelta = usedBytes(pCompanion) - companionBefore;
            pPrevious->becomeCurrentHeap();

            if (!loaded) {
                std::fprintf(stderr, "FAIL: %s:%s did not load\n", path.filename().c_str(), rEntry.name.c_str());
                ++sFailures;
                continue;
            }
            if (isCached) {
                // Only the first copy may pay alignment padding (a new solid heap's data is
                // 16-aligned; copies are 32-aligned multiples of 32).
                const s32 padding = cachedFiles == 0 ? 0x1F : 0;
                if (companionDelta < static_cast< s32 >(fileSize) || companionDelta > static_cast< s32 >(fileSize) + padding) {
                    std::fprintf(stderr, "FAIL: %s:%s: companion grew %d, file is %zu\n", path.filename().c_str(), rEntry.name.c_str(),
                                 companionDelta, fileSize);
                    ++sFailures;
                }
                objectBytes += usedBytes(pCache) - cacheBefore;
                // The model whose copy overflowed the file cache in story5 (1,269,376 bytes).
                sLoadedCrashModel = sLoadedCrashModel || rEntry.name == "heavensdoormysteriousplanet.bdl";
                j3dBytes += fileSize;
                cachedFiles++;
            } else {
                check(companionDelta == 0 && usedBytes(pHeap) - sceneBefore >= static_cast< s32 >(fileSize),
                      "a scene-heap file is copied on the current heap, not the companion");
                sceneFiles++;
            }
        }
    }

    const s32 cacheFree = pCache->getTotalFreeSize();
    std::printf("file cache: %zu archives (%zu bytes), %zu J3D files (%zu bytes) -> companion; scene heap: %zu archives, %zu J3D files\n",
                cachedArchives, archiveBytes, cachedFiles, j3dBytes, sceneArchives, sceneFiles);
    std::printf("native J3D objects on the file cache: %d bytes (%.1f%% of the J3D file bytes); file cache free %d of %d, companion free %d\n",
                objectBytes, j3dBytes != 0 ? 100.0 * objectBytes / j3dBytes : 0.0, cacheFree,
                static_cast< s32 >(static_cast< u8* >(pCache->getEndAddr()) - static_cast< u8* >(pCache->getStartAddr())),
                pCompanion->getTotalFreeSize());
    check(cachedArchives + sceneArchives == archives.size(), "every listed archive placed");
    check(cachedArchives >= 52 && cachedFiles >= 304, "at least the fixture's 52 archives and 304 J3D files placed in the file cache");
    check(sLoadedCrashModel, "heavensdoormysteriousplanet.bdl (the story5 overflow) loaded with its copy in the companion");

    // The Wii overflow rule: with the file cache under 3% free, the next archive goes to the
    // scene heap, and its copies stay on the current heap (the scene heap), not the companion.
    {
        const s32 cacheSize = static_cast< s32 >(static_cast< u8* >(pCache->getEndAddr()) - static_cast< u8* >(pCache->getStartAddr()));
        const s32 filler = pCache->getTotalFreeSize() - cacheSize / 50;
        if (filler > 0) {
            new (pCache, 0x20) u8[filler];
        }
        check(MR::getAproposHeapForSceneArchive(0.03f) == pWatcher->mSceneHeapGDDR, "a nearly full file cache hands archives to the scene heap");

        Buffer data;
        {
            PetariNative::HostAllocationScope hostAllocations;
            Buffer bytes;
            readFile(files / "ObjectData" / "CrystalCageS.arc", &bytes);
            data = std::memcmp(bytes.data(), "Yaz0", 4) == 0 ? PetariNative::Resource::decompress({bytes.data(), bytes.size()}) : bytes;
        }
        JKRHeap* pScene = pWatcher->mSceneHeapGDDR;
        u8* pResident = new (pScene, 0x20) u8[data.size()];
        std::memcpy(pResident, data.data(), data.size());
        // crystalcages.bdl is the archive's model; find it by its J3D magic in the data section.
        const u8* pModel = nullptr;
        for (std::size_t offset = 0; offset + 8 <= data.size(); offset += 0x20) {
            if (std::memcmp(pResident + offset, "J3D2bdl", 7) == 0 || std::memcmp(pResident + offset, "J3D2bmd", 7) == 0) {
                pModel = pResident + offset;
                break;
            }
        }
        check(pModel != nullptr && PetariNative::J3D::resolveHostImageHeap(pModel) == nullptr, "a scene-heap source resolves to the current heap");
        if (pModel != nullptr) {
            JKRHeap* pPrevious = pScene->becomeCurrentHeap();
            const s32 companionBefore = usedBytes(pCompanion), sceneBefore = usedBytes(pScene);
            check(loadJ3D(pModel), "scene-heap model loads");
            check(usedBytes(pCompanion) == companionBefore && usedBytes(pScene) - sceneBefore >= static_cast< s32 >(PetariNative::readU32BE(pModel + 8)),
                  "a scene-heap model's copy is on the current heap, not the companion");
            pPrevious->becomeCurrentHeap();
            sceneFiles++;
        }
    }

    // Same-stage scene change (GameSystemSceneController::destroyScene when the next scene is
    // on the same stage): only the scene heaps are rebuilt; the file cache, its companion and
    // the copies in it stay.
    const s32 companionUsed = usedBytes(pCompanion);
    pWatcher->destroySceneHeap();
    pWatcher->createSceneHeapOnGameHeap();
    check(pWatcher->mFileCacheHeap == pCache && pWatcher->mFileCacheHostImageHeap == pCompanion && usedBytes(pCompanion) == companionUsed,
          "a same-stage scene change keeps the file cache, its companion and the copies");
    check(PetariNative::J3D::resolveHostImageHeap(pCache->getStartAddr()) == pCompanion, "resolver still routes after a same-stage scene change");

    // Stage change: both heaps go away together and the resolver stops routing.
    pWatcher->destroyGameHeap();
    check(pWatcher->mFileCacheHeap == nullptr && pWatcher->mFileCacheHostImageHeap == nullptr, "destroyGameHeap destroys both heaps");
    u8 probe[0x40] = {};
    check(PetariNative::J3D::resolveHostImageHeap(probe) == nullptr, "resolver returns null without a file cache");

    if (sFailures != 0) {
        std::fprintf(stderr, "%d file cache check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("File cache heap tests passed");
    return 0;
}

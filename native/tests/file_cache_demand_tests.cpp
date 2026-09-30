// File cache demand (native): what every scene archive on the disc costs the file cache.
//
// The game places scene archives in the file cache while it has at least 3% free
// (MR::getAproposHeapForSceneArchive), then in the scene heap. Each cached archive's
// ResourceHolder later creates its resource objects on the file cache too (the archive's heap is
// current; ResourceHolderManager), after the placement check. So the file cache must hold, besides
// the archive bytes the Wii also holds, the resource objects of every cached archive; native
// objects are larger than the Wii's (64-bit pointers).
//
// For every ObjectData and StageData archive, this places the archive's resident bytes in a fresh
// file cache (the game's HeapMemoryWatcher) and creates its J3D resources as
// ResourceHolder::createAndRegisterObject does, with the file cache current: .bdl
// loadBinaryDisplayList, .bmd load + newSharedDisplayList, .bmt loadMaterialTable, animations
// through J3DAnmLoaderDataBase, plus backupInitMaterialData's Mtx44[materials * 8] for the first
// model. Host-layout copies go to the companion heap and are not counted. It reports the object
// bytes per archive (--csv) and, with --stages, projects each stage's demand (see
// file_cache_projection below).
//
// Usage: petari_file_cache_demand_tests --assets GAME_FILES_DIR [--csv OUT] [--stages LIST]
#include "archive.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <petari/boot.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

extern "C" u32 WPADGetWorkMemorySize(void) {
    return 0;
}

namespace fs = std::filesystem;
using Buffer = std::vector< std::uint8_t >;

namespace {

int sFailures = 0;
// Test bookkeeping (strings, containers) must use the host allocator: outside
// HostAllocationScope plain new goes to the current JKR heap, which the game heaps fill.
void check(bool condition, const char* pText, const char* pDetail = "") {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s%s\n", pText, pDetail);
        ++sFailures;
    }
}

bool readFile(const fs::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

s32 usedBytes(JKRHeap* pHeap) {
    return static_cast< s32 >(static_cast< u8* >(pHeap->getEndAddr()) - static_cast< u8* >(pHeap->getStartAddr())) - pHeap->getTotalFreeSize();
}

bool hasExtension(const std::string& name, const char* pExt) {
    return name.find(pExt) != std::string::npos;  // ResourceHolder matches with strstr
}

struct ArchiveCost {
    std::string name;  // "ObjectData/Kuribo"
    u32 archiveBytes = 0;
    u32 j3dFiles = 0;
    u32 objectBytes = 0;  // native resource objects on the file cache
    bool loaded = true;
};

// Creates one J3D resource as ResourceHolder::createAndRegisterObject does. Returns false if the
// loader returned null; *pMaterials receives a model's material count.
bool createResource(const std::string& name, const u8* pData, u16* pMaterials) {
    if (hasExtension(name, ".bdl")) {
        J3DModelData* pModel = J3DModelLoaderDataBase::loadBinaryDisplayList(pData, J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials);
        *pMaterials = pModel != nullptr ? pModel->getMaterialNum() : 0;
        return pModel != nullptr;
    }
    if (hasExtension(name, ".bmd")) {
        J3DModelData* pModel =
            J3DModelLoaderDataBase::load(pData, J3DMLF_Material_PE_Full | J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials | J3DMLF_21);
        if (pModel == nullptr) {
            return false;
        }
        pModel->newSharedDisplayList(0);
        *pMaterials = pModel->getMaterialNum();
        return true;
    }
    if (hasExtension(name, ".bmt")) {
        return J3DModelLoaderDataBase::loadMaterialTable(pData) != nullptr;
    }
    return J3DAnmLoaderDataBase::load(pData) != nullptr;
}

bool isJ3DResource(const std::string& name) {
    static const char* cExts[] = {".bdl", ".bmd", ".bmt", ".bck", ".bca", ".btk", ".bpk", ".btp", ".blk", ".brk", ".bva"};
    for (const char* pExt : cExts) {
        if (hasExtension(name, pExt)) {
            return true;
        }
    }
    return false;
}

// One archive in a fresh file cache: resident bytes, then its J3D resources.
ArchiveCost measure(HeapMemoryWatcher* pWatcher, const fs::path& path, const std::string& label) {
    ArchiveCost cost;
    {
        PetariNative::HostAllocationScope hostAllocations;
        cost.name = label;
    }
    Buffer data;
    PetariNative::Resource::Archive archive;
    {
        PetariNative::HostAllocationScope hostAllocations;
        Buffer bytes;
        if (!readFile(path, &bytes) || bytes.size() < 8) {
            cost.loaded = false;
            return cost;
        }
        data = std::memcmp(bytes.data(), "Yaz0", 4) == 0 ? PetariNative::Resource::decompress({bytes.data(), bytes.size()}) : bytes;
        archive = PetariNative::Resource::Archive::parse({data.data(), data.size()});
    }
    pWatcher->destroyGameHeap();
    pWatcher->createFileCacheHeapOnGameHeap(0x1040400);
    pWatcher->createSceneHeapOnGameHeap();
    JKRHeap* pCache = pWatcher->mFileCacheHeap;

    // FileRipper allocates the archive's size 0x40-aligned from the heap.
    cost.archiveBytes = static_cast< u32 >(data.size());
    u8* pResident = new (pCache, 0x40) u8[data.size()];
    std::memcpy(pResident, data.data(), data.size());
    const s32 afterArchive = usedBytes(pCache);

    JKRHeap* pPrevious = pCache->becomeCurrentHeap();
    u16 firstModelMaterials = 0;
    bool sawModel = false;
    for (const auto& rEntry : archive.entries()) {
        if (rEntry.isDirectory() || !isJ3DResource(rEntry.name)) {
            continue;
        }
        const u8* pFile = pResident + 0x20 + PetariNative::readU32BE(pResident + 0x0C) + rEntry.offset;
        if (rEntry.size < 0x20 || pFile + rEntry.size > pResident + data.size() || std::memcmp(pFile, "J3D", 3) != 0) {
            continue;  // stored compressed or not a J3D file: the game would not create it in place either
        }
        u16 materials = 0;
        if (!createResource(rEntry.name, pFile, &materials)) {
            std::fprintf(stderr, "FAIL: %s:%s did not load\n", label.c_str(), rEntry.name.c_str());
            ++sFailures;
            continue;
        }
        cost.j3dFiles++;
        if (!sawModel && (hasExtension(rEntry.name, ".bdl") || hasExtension(rEntry.name, ".bmd"))) {
            sawModel = true;
            firstModelMaterials = materials;
        }
    }
    if (sawModel && firstModelMaterials != 0) {
        new Mtx44[firstModelMaterials * 8];  // ResourceHolder::backupInitMaterialData
    }
    pPrevious->becomeCurrentHeap();
    cost.objectBytes = static_cast< u32 >(usedBytes(pCache) - afterArchive);
    return cost;
}

// Per-stage projection (--stages LIST, native/tests/data/file_cache_stage_archives.txt, written by
// native/tools/file_cache_stage_archives.py). Each stage's archives are placed in order by the
// placement rule the game uses (the Wii 3% rule on the Wii-sized part of the file cache,
// HeapMemoryWatcher::getFileCachePlacementFreeRatio), counting archive bytes only, so as many
// archives are cached as possible; then all cached archives' resource objects are assumed to be
// created afterwards (the worst case: in the game some are created between mounts). Resource
// objects: the measured J3D objects plus cBookkeeping per archive (JKRMemArchive, holders,
// resource tables, alignment), calibrated below against a live run. Fails if an admitted archive
// does not fit, or the objects need more than cMargin of the room the archives leave.
constexpr u32 cWiiFileCacheSize = 0x1040400;  // GameSystemSceneController::initializeScene
constexpr u32 cBookkeeping = 6 * 1024;       // live Good Egg 1: (1416288 - 835752) / 116 = 5004
constexpr double cMargin = 0.8;              // objects may use at most 80% of the room left
// Live Good Egg 1 (build/heap-headroom/egg-s1b.log, HeapMemoryWatcher::checkRestMemory): the file
// cache's use beyond its 116 archives' bytes. The projection of that exact archive list must not
// be smaller, or the model underestimates.
constexpr u32 cLiveGoodEgg1OtherUse = 1416288;

struct Projection {
    std::string stage;
    u32 archives = 0, cached = 0, cachedBytes = 0, objectBytes = 0, room = 0;
    bool fits = true;
    double use() const { return room != 0 ? static_cast< double >(objectBytes) / room : 1e9; }
};

Projection project(const std::string& stage, const std::vector< std::string >& archives, const std::map< std::string, ArchiveCost >& costs,
                   u32 allowance) {
    Projection p;
    p.stage = stage;
    const s64 size = static_cast< s64 >(cWiiFileCacheSize) + allowance;
    s64 freeBytes = size;
    for (const std::string& rName : archives) {
        const auto found = costs.find(rName);
        if (found == costs.end()) {
            continue;
        }
        p.archives++;
        const double placementRatio = static_cast< double >(freeBytes - allowance) / cWiiFileCacheSize;
        if (placementRatio < 0.03) {
            continue;  // the scene heap
        }
        const s64 bytes = (static_cast< s64 >(found->second.archiveBytes) + 0x3F) & ~0x3F;
        if (bytes > freeBytes) {
            p.fits = false;
            std::fprintf(stderr, "  %s: %s (%lld bytes) admitted with %lld free\n", stage.c_str(), rName.c_str(), static_cast< long long >(bytes),
                         static_cast< long long >(freeBytes));
        }
        freeBytes -= bytes;
        p.cached++;
        p.cachedBytes += static_cast< u32 >(bytes);
        p.objectBytes += found->second.objectBytes + cBookkeeping;
    }
    p.room = freeBytes > 0 ? static_cast< u32 >(freeBytes) : 0;
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path files, csvPath, stagesPath;
    u32 allowance = HeapMemoryWatcher::cFileCacheNativeAllowance;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string option = argv[i];
        if (option == "--assets") {
            files = argv[i + 1];
        } else if (option == "--csv") {
            csvPath = argv[i + 1];
        } else if (option == "--stages") {
            stagesPath = argv[i + 1];
        } else if (option == "--allowance") {
            allowance = static_cast< u32 >(std::strtoul(argv[i + 1], nullptr, 0));  // to project other sizes
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "Usage: %s --assets GAME_FILES_DIR [--csv OUT] [--stages LIST]\n", argv[0]);
        return 2;
    }

    OSInit();
    HeapMemoryWatcher::createRootHeap();
    SingletonHolder< HeapMemoryWatcher >::init();
    HeapMemoryWatcher* pWatcher = SingletonHolder< HeapMemoryWatcher >::get();

    std::vector< std::pair< fs::path, std::string > > paths;
    {
        PetariNative::HostAllocationScope hostAllocations;
        for (const char* pDir : {"ObjectData", "StageData"}) {
            for (const auto& rItem : fs::directory_iterator(files / pDir)) {
                if (rItem.path().extension() == ".arc") {
                    paths.push_back({rItem.path(), std::string(pDir) + "/" + rItem.path().stem().string()});
                }
            }
        }
        std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    }

    std::map< std::string, ArchiveCost > costs;
    u64 archiveTotal = 0, objectTotal = 0;
    for (const auto& rPath : paths) {
        ArchiveCost cost = measure(pWatcher, rPath.first, rPath.second);
        PetariNative::HostAllocationScope hostAllocations;
        check(cost.loaded, "archive readable: ", rPath.second.c_str());
        costs[cost.name] = std::move(cost);
        archiveTotal += cost.archiveBytes;
        objectTotal += cost.objectBytes;
    }
    std::printf("%zu archives: %llu archive bytes, %llu native resource-object bytes on the file cache (%.2f%%)\n", costs.size(),
                static_cast< unsigned long long >(archiveTotal), static_cast< unsigned long long >(objectTotal),
                archiveTotal != 0 ? 100.0 * objectTotal / archiveTotal : 0.0);

    if (!csvPath.empty()) {
        PetariNative::HostAllocationScope hostAllocations;
        std::ofstream csv(csvPath);
        csv << "archive,archive_bytes,j3d_files,object_bytes\n";
        for (const auto& rItem : costs) {
            csv << rItem.first << ',' << rItem.second.archiveBytes << ',' << rItem.second.j3dFiles << ',' << rItem.second.objectBytes << '\n';
        }
    }

    if (!stagesPath.empty()) {
        PetariNative::HostAllocationScope hostAllocations;
        std::ifstream list(stagesPath);
        check(static_cast< bool >(list), "stage list readable: ", stagesPath.c_str());
        std::vector< Projection > projections;
        std::string line;
        while (std::getline(list, line)) {
            std::istringstream words(line);
            std::string stage, name;
            words >> stage;
            std::vector< std::string > archives;
            while (words >> name) {
                archives.push_back(name);
            }
            if (!stage.empty()) {
                projections.push_back(project(stage, archives, costs, allowance));
            }
        }
        std::sort(projections.begin(), projections.end(), [](const Projection& a, const Projection& b) { return a.use() > b.use(); });
        std::printf("file cache %u + native allowance %u bytes; objects may use %.0f%% of the room archives leave\n", cWiiFileCacheSize, allowance,
                    100 * cMargin);
        for (const Projection& rP : projections) {
            std::printf("  %-28s %3u/%3u archives cached, %8u bytes; objects %7u in room %8u (%5.1f%%)%s\n", rP.stage.c_str(), rP.cached,
                        rP.archives, rP.cachedBytes, rP.objectBytes, rP.room, 100 * rP.use(),
                        !rP.fits ? "  ARCHIVE DOES NOT FIT" : rP.use() > cMargin ? "  OVER MARGIN" : "");
            check(rP.fits, "every cached archive fits: ", rP.stage.c_str());
            check(rP.use() <= cMargin, "resource objects within the margin of the file cache's room: ", rP.stage.c_str());
            if (rP.stage == "EggStarGalaxy-s1-live") {
                check(rP.cached == 114 && rP.objectBytes >= cLiveGoodEgg1OtherUse,
                      "the projection of live Good Egg 1 caches its 114 archives and does not underestimate its measured use", "");
            }
        }
        check(projections.size() >= 49, "all stage lists projected", "");
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d file cache demand check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("File cache demand tests passed");
    return 0;
}

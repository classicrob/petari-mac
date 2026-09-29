// RVLFaceLib booted natively as SMG boots it (MiiFacePartsHolder::init):
// RFLGetWorkSize, RFLInitResAsync with RFL_Res.dat from the disc, then
// RFLGetAsyncStatus polling. The Mii Channel database comes from the native
// NAND (petari_platform_nand) in a temporary root.
//
//        petari_rfl_boot_tests                   with a synthetic RFL_Res.dat
//        petari_rfl_boot_tests --assets FILES    with the disc's

#include <revolution/dvd.h>
#include <revolution/nand.h>
#include <revolution/os.h>

#include <cstdint>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "RVLFaceLibInternal.h"
#include "archive.hpp"
#include "petari/platform/dvd.hpp"
#include "petari/platform/nand.hpp"
#include "rfl_test_resource.hpp"

extern "C" void __OSThreadInit(void);

namespace fs = std::filesystem;
namespace PNAND = PetariNative::Platform::NAND;

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

const char* const kDatabasePath = "/shared2/menu/FaceLib/RFL_DB.dat";

// RFL_Res.dat in 32-byte aligned memory, as JKRMemArchive hands it over.
struct Resource {
    std::vector<std::uint8_t> storage;
    void* data = nullptr;
    u32 size = 0;

    explicit Resource(const Bytes& bytes) : storage(bytes.size() + 32) {
        const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
        data = storage.data() + ((32 - address % 32) % 32);
        std::memcpy(data, bytes.data(), bytes.size());
        size = static_cast<u32>(bytes.size());
    }
};

Bytes discResource(const std::string& files) {
    std::ifstream in(files + "/ObjectData/MiiFaceDatabase.arc", std::ios::binary);
    check(in.good(), "open MiiFaceDatabase.arc");
    const Bytes arc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto archive = PetariNative::Resource::Archive::parse({arc.data(), arc.size()});
    for (std::size_t i = 0; i < archive.entries().size(); ++i) {
        const auto& entry = archive.entries()[i];
        if (!entry.isDirectory() && (entry.name == "rfl_res.dat" || entry.name == "RFL_Res.dat")) {
            return archive.resourceData(i);
        }
    }
    check(false, "RFL_Res.dat in MiiFaceDatabase.arc");
    return {};
}

// As MiiFacePartsHolder: a work buffer of RFLGetWorkSize(false) bytes.
struct Work {
    std::vector<std::uint8_t> storage;
    void* data;

    Work() : storage(RFLGetWorkSize(FALSE) + 32) {
        const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
        data = storage.data() + ((32 - address % 32) % 32);
    }
};

// MiiFacePartsHolder::reinitCharModel polls once per frame.
RFLErrcode waitForBoot() {
    for (int frame = 0; frame < 600; ++frame) {
        const RFLErrcode status = RFLGetAsyncStatus();
        if (status != RFLErrcode_Busy) {
            return status;
        }
        OSSleepTicks(OSMillisecondsToTicks(16));
    }
    return RFLErrcode_Busy;
}

void checkNoOfficialMiis(const std::string& when) {
    int available = 0;
    for (u16 i = 0; i < RFL_DB_CHAR_MAX; ++i) {
        available += RFLIsAvailableOfficialData(i) ? 1 : 0;
    }
    check(available == 0, when + ": no official Miis");
    RFLCreateID id;
    const std::uint8_t bytes[8] = {0x80, 0x00, 0x00, 0x00, 0xEC, 0xFF, 0x82, 0xD2};
    std::memcpy(id.data, bytes, sizeof(bytes));
    u16 index = 0xFFFF;
    check(!RFLSearchOfficialData(&id, &index), when + ": a saved Mii ID is not found");
    RFLAdditionalInfo info;
    check(RFLGetAdditionalInfo(&info, RFLDataSource_Official, nullptr, 0) != RFLErrcode_Success,
          when + ": no official Mii names");
}

void checkDefaultMii(const std::string& when) {
    RFLAdditionalInfo info;
    std::memset(&info, 0, sizeof(info));
    const RFLErrcode err = RFLGetAdditionalInfo(&info, RFLDataSource_Default, nullptr, 0);
    const char16_t expected[] = u"no name";
    check(err == RFLErrcode_Success && std::memcmp(info.name, expected, sizeof(expected)) == 0,
          when + ": built-in default Mii 0 readable (FileSelectItem's face)");
}

void checkHeaps(const std::string& when) {
    RFLiManager* manager = RFLiGetManager();
    check(manager != nullptr && manager->dbMgr.database != nullptr, when + ": database buffer allocated");
    for (int chan = 0; chan < 4; ++chan) {
        check(manager->ctrlMgr.buffer[chan] != nullptr, when + ": controller buffers allocated");
    }
    for (int type = 0; type < RFLiFileType_Max; ++type) {
        check(manager->info[type].safeBuffer != nullptr, when + ": NAND safe buffers allocated");
    }
    const u32 systemFree = MEMGetAllocatableSizeForExpHeapEx(manager->systemHeap, 4);
    const u32 tmpFree = MEMGetAllocatableSizeForExpHeapEx(manager->tmpHeap, 32);
    std::printf("%s: RFLGetWorkSize %u (manager %zu), system heap free %u, temporary heap free %u\n", when.c_str(),
                static_cast<unsigned>(RFLGetWorkSize(FALSE)), sizeof(RFLiManager), static_cast<unsigned>(systemFree),
                static_cast<unsigned>(tmpFree));
    // RFLiInitCharModel and RFLMakeIcon allocate the largest texture or
    // shape file (FaceTex, 16,416 bytes) and a model buffer from here.
    check(tmpFree >= 0x10000, when + ": temporary heap has room for part loading");
}

void testNoDatabase(const Bytes& resourceBytes) {
    const Resource resource(resourceBytes);
    Work work;
    const RFLErrcode start = RFLInitResAsync(work.data, resource.data, resource.size, FALSE);
    check(start == RFLErrcode_Busy || start == RFLErrcode_Success, "RFLInitResAsync starts");
    check(waitForBoot() == RFLErrcode_Success, "no RFL_DB.dat: boot succeeds (the original no-Mii state)");
    check(RFLAvailable() && RFLIsResourceCached(), "RFL available with the disc resource cached");
    // brokenType is a bit set (RFLiSetFileBroken).
    check(RFLiNotFoundError() && RFLiGetManager()->brokenType == (1 << RFLiFileBrokenType_DBNotFound),
          "database recorded as not found, nothing else broken");
    checkNoOfficialMiis("no database");
    checkDefaultMii("no database");
    checkHeaps("no database");
    check(!fs::exists(PNAND::hostPath(kDatabasePath)), "booting creates no database");
    RFLExit();
    check(!RFLAvailable(), "RFLExit");
}

void testForeignDatabase(const Bytes& resourceBytes) {
    const fs::path host = PNAND::hostPath(kDatabasePath);
    fs::create_directories(host.parent_path());
    Bytes stored(0x1F1E0);
    for (std::size_t i = 0; i < stored.size(); ++i) {
        stored[i] = static_cast<std::uint8_t>(i * 7);
    }
    stored[0] = 'R';
    stored[1] = 'N';
    stored[2] = 'O';
    stored[3] = 'D';
    {
        std::ofstream out(host, std::ios::binary);
        out.write(reinterpret_cast<const char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
    }

    const Resource resource(resourceBytes);
    Work work;
    RFLInitResAsync(work.data, resource.data, resource.size, FALSE);
    const RFLErrcode status = waitForBoot();
    check(status != RFLErrcode_Busy, "a console database: boot finishes");
    check(RFLAvailable() && (RFLiGetManager()->brokenType & (1 << RFLiFileBrokenType_DBBroken)) != 0,
          "a console database is refused explicitly (treated as unreadable, status " + std::to_string(status) + ")");
    checkNoOfficialMiis("console database");
    checkDefaultMii("console database");
    RFLExit();

    std::ifstream in(host, std::ios::binary);
    const Bytes after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(after == stored, "the console database is left untouched");
    fs::remove(host);
}

void testDamagedResource(const Bytes& resourceBytes) {
    Bytes damaged = resourceBytes;
    damaged[1] = 17;  // archive count
    const Resource resource(damaged);
    Work work;
    check(RFLInitResAsync(work.data, resource.data, resource.size, FALSE) == RFLErrcode_Fatal && !RFLAvailable(),
          "a damaged RFL_Res.dat is refused before RFL starts");
    check(RFLInitResAsync(work.data, nullptr, 0, FALSE) == RFLErrcode_Fatal, "no resource: fatal, as on the Wii");
}

}  // namespace

int main(int argc, char** argv) {
    __OSThreadInit();
    const fs::path root = fs::temp_directory_path() / ("petari-rfl-boot-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    std::string error;
    check(PNAND::mount(root, &error), "mount NAND root: " + error);

    Bytes resource;
    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        // As the application starts: the disc first, so NANDInit finds the
        // title's home directory from its disc ID.
        check(PetariNative::Platform::DVD::mount({fs::path(argv[2]).parent_path()}, &error), "mount the disc: " + error);
        DVDInit();
        resource = discResource(argv[2]);
    } else if (argc == 1) {
        resource = RflTestResource::syntheticResource().bytes;
    } else {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }

    check(NANDInit() == NAND_RESULT_OK, "NANDInit");
    testNoDatabase(resource);
    testForeignDatabase(resource);
    testDamagedResource(resource);
    testNoDatabase(resource);  // again after RFLExit, as each file-select scene does

    PNAND::shutdown();
    PetariNative::Platform::DVD::shutdown();
    fs::remove_all(root);
    std::printf("native RFL boot tests passed (%d checks)\n", checks);
    return 0;
}

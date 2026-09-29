// Read-only check of the native DVD service against a real extracted disc.
//
// Usage: petari_dvd_smoke <extracted-disc-root> [--full]
//
// Mounts the tree with fst.bin validation, resolves paths the game opens at
// boot, and reads every file's first and last bytes through DVDReadPrio,
// comparing them with the host file. --full compares every byte.

#include <revolution/dvd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "petari/platform/dvd.hpp"

namespace PDVD = PetariNative::Platform::DVD;

namespace {

int failures = 0;

void fail(const std::string& message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures;
}

std::vector<char> hostRead(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
    std::vector<char> out(length, '\0');
    std::ifstream in(path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(out.data(), static_cast<std::streamsize>(length));
    return out;
}

bool compareRange(DVDFileInfo& info, const std::filesystem::path& host, u32 offset, u32 length, std::vector<char>& buffer) {
    // DVDRead needs offsets in 4-byte units; lengths are rounded to 32 like game loaders do.
    const u32 readLength = (length + 31) & ~31u;
    buffer.assign(readLength, '\x5A');
    const s32 got = DVDReadPrio(&info, buffer.data(), static_cast<s32>(readLength), static_cast<s32>(offset), 2);
    if (got != static_cast<s32>(readLength)) {
        return false;
    }
    const std::vector<char> expected = hostRead(host, offset, length);
    return std::memcmp(buffer.data(), expected.data(), length) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <extracted-disc-root> [--full]\n", argv[0]);
        return 2;
    }
    const bool full = argc > 2 && std::strcmp(argv[2], "--full") == 0;

    std::string error;
    if (!PDVD::mount({argv[1]}, &error)) {
        std::fprintf(stderr, "mount failed: %s\n", error.c_str());
        return 1;
    }
    DVDInit();
    const PDVD::MountInfo info = PDVD::mountInfo();
    const DVDDiskID* id = DVDGetCurrentDiskID();
    std::printf("mounted %s: %s FST, %u entries, %u files, disc ID %.4s%.2s version %u\n", info.filesDirectory.c_str(),
                info.source == PDVD::FstSource::DiscFst ? "disc" : "scanned", info.entryCount, info.fileCount, id->gameName,
                id->company, id->gameVersion);

    // Paths used during boot by GameSystem, FileRipper, AudSystem, and ScenarioDataParser.
    for (const char* path : {"/StageData", "/AudioRes", "/SystemData", "/ObjectData", "/LayoutData", "/opening.bnr"}) {
        if (DVDConvertPathToEntrynum(path) < 0) {
            fail(std::string("path not found: ") + path);
        }
    }
    DVDDir dir;
    DVDDirEntry entry;
    int stages = 0;
    if (DVDOpenDir("/StageData", &dir)) {
        while (DVDReadDir(&dir, &entry)) {
            stages += entry.isDir ? 1 : 0;
        }
        DVDCloseDir(&dir);
    }
    std::printf("/StageData has %d stage directories\n", stages);

    std::vector<char> buffer;
    std::uint64_t bytes = 0;
    int compressed = 0, rarc = 0;
    for (u32 i = 1; i < info.entryCount; ++i) {
        const std::filesystem::path host = PDVD::hostPathForEntry(static_cast<s32>(i));
        if (host.empty()) {
            continue;
        }
        DVDFileInfo file;
        if (!DVDFastOpen(static_cast<s32>(i), &file)) {
            fail("DVDFastOpen failed for " + host.string());
            continue;
        }
        const u32 length = file.length;
        const u32 head = std::min<u32>(length, 32);
        if (!compareRange(file, host, 0, head, buffer)) {
            fail("head mismatch in " + host.string());
        } else if (length >= 4) {
            compressed += std::memcmp(buffer.data(), "Yaz0", 4) == 0 || std::memcmp(buffer.data(), "Yay0", 4) == 0;
            rarc += std::memcmp(buffer.data(), "RARC", 4) == 0;
        }
        if (full) {
            for (u32 offset = 0; offset < length; offset += 0x100000) {
                if (!compareRange(file, host, offset, std::min<u32>(0x100000, length - offset), buffer)) {
                    fail("data mismatch in " + host.string() + " at " + std::to_string(offset));
                    break;
                }
            }
        } else if (length > 32) {
            const u32 tail = (length - 1) & ~31u;
            if (!compareRange(file, host, tail, length - tail, buffer)) {
                fail("tail mismatch in " + host.string());
            }
        }
        bytes += length;
        DVDClose(&file);
    }
    std::printf("checked %u files (%.1f MiB listed): %d Yaz0/Yay0, %d uncompressed RARC\n", info.fileCount,
                static_cast<double>(bytes) / (1024.0 * 1024.0), compressed, rarc);
    PDVD::shutdown();
    std::printf(failures == 0 ? "DVD smoke test passed\n" : "DVD smoke test FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}

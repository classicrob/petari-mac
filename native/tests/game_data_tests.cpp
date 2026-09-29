// Game-side serialized data tests.
//
// - FileRipper::decompressSzsSub (the game's own Yaz0 decoder) on every compressed archive in
//   the game data, compared byte-for-byte with the native resource decompressor.
// - GhostPacket (race ghost .gst data): big-endian multi-byte reads, and a walk of every disc
//   ghost file with the packet layout GhostPlayer::receiveGhostPacket uses.
// - FileRipper::loadToMainRAM with decompression requested on uncompressed disc files, read
//   through the native DVD layer: the first 0x20 bytes come from the header probe buffer,
//   which must still be alive when they are copied (a block-scoped buffer escaped natively).
//   Also on Yaz0 disc files, decoded while streaming from DVD (decompressFromDVD's refills)
//   on the OS-bound main thread, where decompressSzsSub offers preemption points.
//
// Usage: petari_game_data_tests [--assets GAME_FILES_DIR]
// Links: src/Game/System/FileRipper.cpp, src/Game/Player/GhostPacket.cpp,
// native/tests/heap_diagnostics.cpp (panic and console output), petari_resources,
// petari_kernel/heaps/platform (DVD, OS, VI). FileRipper.cpp also references the MR file
// helpers below; this file implements them on top of the DVD layer.
#include "archive.hpp"
#include "Game/Player/GhostPacket.hpp"
#include "Game/System/FileRipper.hpp"
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <petari/boot.hpp>
#include <petari/endian.hpp>
#include <petari/platform/dvd.hpp>
#include <revolution/dvd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

[[noreturn]] static void unreachable(const char* pName) {
    std::fprintf(stderr, "game_data_tests: unexpected call to %s\n", pName);
    std::abort();
}

// Disc-path forms of the MR file helpers (the tests pass full paths, no language prefix).
namespace MR {
    void copyMemory(void* pDst, const void* pSrc, u32 size) {
        std::memcpy(pDst, pSrc, size);
    }

    u32 getFileSize(const char* pPath, bool isLocalized) {
        DVDFileInfo fileInfo;
        if (isLocalized || !DVDOpen(pPath, &fileInfo)) {
            unreachable("MR::getFileSize");
        }
        const u32 size = fileInfo.length;
        DVDClose(&fileInfo);
        return size;
    }

    bool isFileExist(const char* pPath, bool isLocalized) {
        if (isLocalized) {
            unreachable("MR::isFileExist");
        }
        return DVDConvertPathToEntrynum(pPath) >= 0;
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

static void testGhostPacketReads() {
    u8 data[] = {0x12, 0x34, 0x56, 0x78, 0xFF, 0xFE, 0x00, 0x10, 0xFF, 0xF0, 0x80, 0x00, 0x7F, 'A', 'b', 0};
    GhostPacket packet(data, sizeof(data));
    u32 word = 0;
    s16 half = 0;
    TVec3s vec;
    s8 byte = 0;
    char* pName = nullptr;
    packet.read(&word);
    packet.read(&half);
    packet.read(&vec);
    packet.read(&byte);
    packet.read(&pName);
    check(word == 0x12345678, "GhostPacket u32 is big-endian");
    check(half == -2, "GhostPacket s16 is big-endian");
    check(vec.x == 16 && vec.y == -16 && vec.z == -32768, "GhostPacket TVec3s is big-endian");
    check(byte == 0x7F && std::strcmp(pName, "Ab") == 0 && packet.mCurOffs == sizeof(data), "GhostPacket s8 and string reads");
}

// Walks a ghost file: each packet is u8 frame, u8 size, s16 flags, then the fields selected
// by the flags in GhostPlayer::receiveGhostPacket's order. The field sizes implied by the
// (big-endian) flags must add up to the packet size exactly, which only holds when the flags
// and the string field are decoded correctly.
static bool walkGhost(const Buffer& file, std::size_t* pPackets, std::string* pError) {
    std::size_t offset = 0;
    while (offset + 4 <= file.size()) {
        GhostPacket packet(const_cast< u8* >(&file[offset]), 0);
        s8 frame, size;
        s16 flags;
        packet.read(&frame);
        packet.read(&size);
        packet.read(&flags);
        const u8 packetSize = static_cast< u8 >(size);
        if (packetSize == 0) {
            break;  // end of recording
        }
        if (packetSize < 4 || offset + packetSize > file.size()) {
            *pError = "packet size outside the file";
            return false;
        }

        const u16 updateFlags = static_cast< u16 >(flags);
        TVec3s pos;
        TVec3Sc byteVec;
        s8 byte;
        s16 half;
        u32 hash;
        char* pName;
        if (updateFlags & 0x0001) {
            packet.read(&pos);
        }
        if (updateFlags & 0x0800) {
            packet.read(&byteVec);
        }
        if (updateFlags & 0x0400) {
            packet.read(&byteVec);
        }
        for (u16 bit : {0x0002, 0x0004, 0x0008}) {
            if (updateFlags & bit) {
                packet.read(&byte);
            }
        }
        if (updateFlags & 0x0010) {
            packet.read(&pName);
        }
        if (updateFlags & 0x2000) {
            packet.read(&hash);
        }
        if (updateFlags & 0x0020) {
            packet.read(&half);
        }
        for (u16 i = 0; i < 4; i++) {
            if (updateFlags & (0x0040 << i)) {
                packet.read(&byte);
            }
        }
        if (updateFlags & 0x1000) {
            packet.read(&byte);
        }
        if (packet.mCurOffs != packetSize) {
            *pError = "fields selected by the flags do not fill the packet";
            return false;
        }
        offset += packetSize;
        (*pPackets)++;
    }
    return true;
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
    std::size_t decompressed = 0, ghosts = 0, packets = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(filesRoot)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".arc") {
            continue;
        }
        Buffer bytes;
        if (!readFile(entry.path(), &bytes) || bytes.size() < 0x10) {
            continue;
        }

        // The Yaz0 comparison is bounded to keep sanitizer runs short; it covers every size class.
        if (std::memcmp(bytes.data(), "Yaz0", 4) == 0 && (decompressed < 300 || entry.path().filename().string().find("Ghost") == 0)) {
            const u32 size = PetariNative::readU32BE(&bytes[4]);
            Buffer game(size + 0x20, 0xCD);
            // decompressSzsSub stops at the header's size; the guard bytes must stay untouched.
            const bool ok = FileRipper::decompressSzsSub(bytes.data(), game.data());
            const Buffer reference = PetariNative::Resource::decompress({bytes.data(), bytes.size()});
            bool guard = true;
            for (std::size_t i = size; i < game.size(); i++) {
                guard = guard && game[i] == 0xCD;
            }
            if (!ok || reference.size() != size || std::memcmp(game.data(), reference.data(), size) != 0 || !guard) {
                std::fprintf(stderr, "FAIL: %s: FileRipper Yaz0 output differs\n", entry.path().filename().c_str());
                ++sFailures;
            }
            decompressed++;
        }

        PetariNative::Resource::Archive archive;
        try {
            archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        } catch (const std::exception&) {
            continue;
        }
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            const auto& file = archive.entries()[i];
            if (file.isDirectory() || file.name.size() < 4 || file.name.substr(file.name.size() - 4) != ".gst") {
                continue;
            }
            std::string error;
            if (!walkGhost(archive.resourceData(i), &packets, &error)) {
                std::fprintf(stderr, "FAIL: %s:%s: %s\n", entry.path().filename().c_str(), file.name.c_str(), error.c_str());
                ++sFailures;
            }
            ghosts++;
        }
    }
    std::printf("FileRipper Yaz0: %zu archives match; ghosts: %zu files, %zu packets\n", decompressed, ghosts, packets);
    check(decompressed > 0 && ghosts > 0, "no compressed archives or ghost files found");
}

// Loads uncompressed disc files with decompress = true, the path that probes the first 0x20
// bytes into a stack buffer and copies them to the destination after allocating it.
static void testLoadToMainRAM(const std::filesystem::path& filesRoot) {
    namespace fs = std::filesystem;
    namespace PDVD = PetariNative::Platform::DVD;
    OSInit();
    std::string error;
    if (!PDVD::mount({filesRoot.parent_path()}, &error)) {
        std::fprintf(stderr, "FAIL: DVD mount: %s\n", error.c_str());
        ++sFailures;
        return;
    }
    DVDInit();
    JKRExpHeap::createRoot(1, false);

    std::size_t loaded = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(filesRoot)) {
        if (loaded >= 64 || !entry.is_regular_file() || entry.file_size() < 0x40 || entry.file_size() > 0x100000) {
            continue;
        }
        Buffer bytes;
        if (!readFile(entry.path(), &bytes) || std::memcmp(bytes.data(), "Yaz0", 4) == 0 || std::memcmp(bytes.data(), "Yay0", 4) == 0) {
            continue;
        }
        const std::string discPath = "/" + fs::relative(entry.path(), filesRoot).generic_string();

        JKRExpHeap* pHeap = JKRExpHeap::create(4 * 1024 * 1024, JKRHeap::sRootHeap, false);
        const u8* pData = static_cast< const u8* >(FileRipper::loadToMainRAM(discPath.c_str(), nullptr, true, pHeap, FileRipper::UNK_0));
        if (pData == nullptr || std::memcmp(pData, bytes.data(), 0x20) != 0) {
            std::fprintf(stderr, "FAIL: %s: first 0x20 bytes differ from the file\n", discPath.c_str());
            ++sFailures;
        } else if (std::memcmp(pData + 0x20, bytes.data() + 0x20, bytes.size() - 0x20) != 0) {
            std::fprintf(stderr, "FAIL: %s: data after 0x20 differs from the file\n", discPath.c_str());
            ++sFailures;
        }
        JKRHeap::destroy(pHeap);
        loaded++;
    }
    std::printf("FileRipper loadToMainRAM: %zu uncompressed files match\n", loaded);
    check(loaded > 0, "no uncompressed files found");

    // Yaz0 files streamed through the 0x20000 read buffer (as GameSystem sets it up), so large
    // archives take several DVD refills; the result must equal the reference decompressor.
    FileRipper::setup(0x20000, JKRHeap::sRootHeap);
    std::size_t streamed = 0, refilled = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(filesRoot)) {
        if (streamed >= 16 || !entry.is_regular_file() || entry.path().extension() != ".arc" || entry.file_size() <= 0x20000 ||
            entry.file_size() > 0x400000) {
            continue;
        }
        Buffer bytes;
        if (!readFile(entry.path(), &bytes) || bytes.size() < 0x10 || std::memcmp(bytes.data(), "Yaz0", 4) != 0) {
            continue;
        }
        const Buffer reference = PetariNative::Resource::decompress({bytes.data(), bytes.size()});
        const std::string discPath = "/" + fs::relative(entry.path(), filesRoot).generic_string();

        JKRExpHeap* pHeap = JKRExpHeap::create(24 * 1024 * 1024, JKRHeap::sRootHeap, false);
        const u8* pData = static_cast< const u8* >(FileRipper::loadToMainRAM(discPath.c_str(), nullptr, true, pHeap, FileRipper::UNK_0));
        if (pData == nullptr || std::memcmp(pData, reference.data(), reference.size()) != 0) {
            std::fprintf(stderr, "FAIL: %s: streamed Yaz0 output differs from the reference\n", discPath.c_str());
            ++sFailures;
        }
        JKRHeap::destroy(pHeap);
        refilled += bytes.size() > 0x20000;
        streamed++;
    }
    std::printf("FileRipper loadToMainRAM: %zu streamed Yaz0 files match (%zu larger than the read buffer)\n", streamed, refilled);
    check(streamed > 0 && refilled == streamed, "no streamed Yaz0 files needing DVD refills");
}

int main(int argc, char** argv) {
    testGhostPacketReads();
    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        testAssets(argv[2]);
        testLoadToMainRAM(argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }
    if (sFailures != 0) {
        std::fprintf(stderr, "%d game data check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("Game data tests passed");
    return 0;
}

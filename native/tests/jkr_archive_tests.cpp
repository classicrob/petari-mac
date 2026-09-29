// Native RARC table handling in JKRArchive/JKRMemArchive, using real JKR heaps and
// the decompression thread. With --assets <files dir>, every RARC archive on the
// disc is also mounted and compared with the independent native/resource parser.
#include "JSystem/JKernel/JKRArchive.hpp"
#include "JSystem/JKernel/JKRDecomp.hpp"
#include "JSystem/JKernel/JKRDvdFile.hpp"
#include "JSystem/JKernel/JKRExpHeap.hpp"
#include "JSystem/JKernel/JKRFileFinder.hpp"
#include "JSystem/JKernel/JKRMemArchive.hpp"
#include "JSystem/JKernel/JKRUnitHeap.hpp"
#include "JSystem/JUtility/JUTTexture.hpp"
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>
#include <petari/platform/dvd.hpp>
#include <revolution/dvd.h>
#include <archive.hpp>
#include <revolution/os.h>

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" void OSInit();

static int sFailures;
static int sChecks;
static int sTextures;

#define CHECK(expr)                                                                                                                                  \
    do {                                                                                                                                             \
        sChecks++;                                                                                                                                   \
        if (!(expr)) {                                                                                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                                                          \
            sFailures++;                                                                                                                             \
        }                                                                                                                                            \
    } while (0)

namespace {
void putU16(std::vector< u8 >& out, size_t at, u32 value) {
    out[at] = static_cast< u8 >(value >> 8);
    out[at + 1] = static_cast< u8 >(value);
}

void putU32(std::vector< u8 >& out, size_t at, u32 value) {
    putU16(out, at, value >> 16);
    putU16(out, at + 2, value);
}

u16 arcHash(const char* name) {
    u16 hash = 0;
    for (; *name; name++) {
        hash = static_cast< u16 >(std::tolower(static_cast< unsigned char >(*name)) + hash * 3);
    }
    return hash;
}

// Yaz0 stream made only of literal groups.
std::vector< u8 > yaz0Literal(const std::vector< u8 >& data) {
    std::vector< u8 > out(16, 0);
    std::memcpy(out.data(), "Yaz0", 4);
    putU32(out, 4, static_cast< u32 >(data.size()));
    for (size_t i = 0; i < data.size(); i += 8) {
        out.push_back(0xFF);
        for (size_t j = i; j < i + 8 && j < data.size(); j++) {
            out.push_back(data[j]);
        }
    }
    return out;
}

struct TestFile {
    const char* name;
    u8 flags;
    u32 dataOrDir;
    std::vector< u8 > data;
    u16 id;
};

// Root "arc" (type ROOT): a.bin, sub/, ., ..   Directory "sub" (type SUB ): b.bin (Yaz0), ., ..
// Stored names are lower case: lookups lower-case the query and compare exactly.
std::vector< u8 > buildArchive(std::vector< u8 >* bPlain) {
    std::vector< u8 > aData = {'h', 'e', 'l', 'l', 'o', '-', 'r', 'a', 'r', 'c'};
    std::vector< u8 > bData;
    for (int i = 0; i < 70; i++) {
        bData.push_back(static_cast< u8 >(i * 7 + 3));
    }
    *bPlain = bData;
    std::vector< u8 > bStored = yaz0Literal(bData);

    std::string strings;
    auto addString = [&](const char* s) {
        u32 offset = static_cast< u32 >(strings.size());
        strings += s;
        strings += '\0';
        return offset;
    };
    u32 sDot = addString(".");
    u32 sDotDot = addString("..");
    u32 sArc = addString("arc");
    u32 sA = addString("a.bin");
    u32 sSub = addString("sub");
    u32 sB = addString("b.bin");
    while (strings.size() % 32) {
        strings += '\0';
    }

    struct Entry {
        u16 id;
        u16 hash;
        u8 flags;
        u32 nameOffset;
        u32 offset;
        u32 size;
    };
    // Data block: a.bin at 0, B.bin at 32.
    std::vector< Entry > files = {
        {0, arcHash("a.bin"), 0x11, sA, 0, static_cast< u32 >(aData.size())},
        {0xFFFF, arcHash("sub"), 0x02, sSub, 1, 0x10},
        {0xFFFF, arcHash("."), 0x02, sDot, 0, 0x10},
        {0xFFFF, arcHash(".."), 0x02, sDotDot, 0xFFFFFFFF, 0x10},
        {4, arcHash("b.bin"), 0x95, sB, 32, static_cast< u32 >(bStored.size())},
        {0xFFFF, arcHash("."), 0x02, sDot, 1, 0x10},
        {0xFFFF, arcHash(".."), 0x02, sDotDot, 0, 0x10},
    };

    const u32 dirOffset = 0x20;
    const u32 fileOffset = dirOffset + 2 * 0x10;
    const u32 stringOffset = ((fileOffset + static_cast< u32 >(files.size()) * 0x14) + 31) & ~31u;
    const u32 infoSize = stringOffset + static_cast< u32 >(strings.size());
    const u32 dataSize = (32 + static_cast< u32 >(bStored.size()) + 31) & ~31u;

    std::vector< u8 > out(0x20 + infoSize + dataSize, 0);
    std::memcpy(out.data(), "RARC", 4);
    putU32(out, 0x04, static_cast< u32 >(out.size()));
    putU32(out, 0x08, 0x20);
    putU32(out, 0x0C, infoSize);
    putU32(out, 0x10, dataSize);
    putU32(out, 0x14, dataSize);

    const size_t info = 0x20;
    putU32(out, info + 0x00, 2);
    putU32(out, info + 0x04, dirOffset);
    putU32(out, info + 0x08, static_cast< u32 >(files.size()));
    putU32(out, info + 0x0C, fileOffset);
    putU32(out, info + 0x10, static_cast< u32 >(strings.size()));
    putU32(out, info + 0x14, stringOffset);
    putU16(out, info + 0x18, static_cast< u32 >(files.size()));
    out[info + 0x1A] = 1;

    std::memcpy(&out[info + dirOffset], "ROOT", 4);
    putU32(out, info + dirOffset + 0x4, sArc);
    putU16(out, info + dirOffset + 0x8, arcHash("arc"));
    putU16(out, info + dirOffset + 0xA, 4);
    putU32(out, info + dirOffset + 0xC, 0);
    std::memcpy(&out[info + dirOffset + 0x10], "SUB ", 4);
    putU32(out, info + dirOffset + 0x14, sSub);
    putU16(out, info + dirOffset + 0x18, arcHash("sub"));
    putU16(out, info + dirOffset + 0x1A, 3);
    putU32(out, info + dirOffset + 0x1C, 4);

    for (size_t i = 0; i < files.size(); i++) {
        size_t at = info + fileOffset + i * 0x14;
        putU16(out, at, files[i].id);
        putU16(out, at + 2, files[i].hash);
        putU32(out, at + 4, static_cast< u32 >(files[i].flags) << 24 | files[i].nameOffset);
        putU32(out, at + 8, files[i].offset);
        putU32(out, at + 12, files[i].size);
    }

    std::memcpy(&out[info + stringOffset], strings.data(), strings.size());
    const size_t data = 0x20 + infoSize;
    std::memcpy(&out[data], aData.data(), aData.size());
    std::memcpy(&out[data + 32], bStored.data(), bStored.size());
    return out;
}

u8* copyToHeap(const std::vector< u8 >& bytes, JKRHeap* heap) {
    u8* buffer = static_cast< u8* >(JKRHeap::alloc(static_cast< u32 >(bytes.size()), 32, heap));
    std::memcpy(buffer, bytes.data(), bytes.size());
    return buffer;
}

void testSyntheticArchive(JKRHeap* heap) {
    std::vector< u8 > bPlain;
    std::vector< u8 > bytes = buildArchive(&bPlain);
    const std::vector< u8 > original = bytes;
    u8* buffer = copyToHeap(bytes, heap);

    JKRMemArchive* archive = new (heap, 0) JKRMemArchive();
    CHECK(archive->mountFixed(buffer, JKR_MEM_BREAK_FLAG_0));
    CHECK(archive->mIsMounted);
    CHECK(std::strcmp(archive->mLoaderName, "arc") == 0);
    CHECK(std::memcmp(buffer, original.data(), original.size()) == 0);

    // Path, case-insensitive, type and index lookups.
    u8* a = static_cast< u8* >(archive->getResource("/a.bin"));
    const u8* aExpected = buffer + 0x20 + PetariNative::readU32BE(buffer + 0x0C);
    CHECK(a == aExpected);
    CHECK(a != nullptr && std::memcmp(a, "hello-rarc", 10) == 0);
    CHECK(archive->getResource("/A.BIN") == a);
    CHECK(archive->getResource("/SUB/B.BIN") != nullptr);
    CHECK(archive->getResource('ROOT', "a.bin") == a);
    CHECK(archive->getResource(0u, "A.bin") == a);
    CHECK(archive->getIdxResource(0) == a);
    CHECK(archive->getResource(static_cast< u16 >(0)) == a);
    CHECK(archive->getResSize(a) == 10);
    CHECK(archive->getResource("/missing.bin") == nullptr);
    CHECK(archive->getResource("/sub") == nullptr);
    CHECK(archive->getIdxResource(7) == nullptr);

    // Subdirectory paths, "..", and the current directory.
    u8* bStored = static_cast< u8* >(archive->getResource("/sub/b.bin"));
    CHECK(bStored != nullptr && std::memcmp(bStored, "Yaz0", 4) == 0);
    CHECK(archive->getResource("/sub/../a.bin") == a);
    CHECK(archive->getResource('SUB ', "b.bin") == bStored);
    CHECK(archive->getResource(static_cast< u16 >(4)) == bStored);
    CHECK(archive->getExpandedResSize(bStored) == bPlain.size());
    CHECK(archive->becomeCurrent("/sub"));
    CHECK(archive->getResource("b.bin") == bStored);
    CHECK(archive->becomeCurrent("/"));
    CHECK(archive->countFile("/") == 4);
    CHECK(archive->countFile("/sub") == 3);
    CHECK(archive->countResource() == 2);
    CHECK(archive->getFileAttribute(4) == 0x95);

    JKRArchive::SDirEntry dirEntry;
    CHECK(archive->getDirEntry(&dirEntry, 1));
    CHECK(dirEntry.mFileFlag == 0x02 && dirEntry.mFileID == 0xFFFF && std::strcmp(dirEntry.mName, "sub") == 0);

    JKRArcFinder* finder = archive->getFirstFile("/sub");
    CHECK(finder != nullptr);
    if (finder != nullptr) {
        CHECK(finder->mHasMoreFiles && std::strcmp(finder->mName, "b.bin") == 0 && !finder->mFileIsFolder && finder->mFileID == 4);
        finder->findNextFile();
        CHECK(finder->mHasMoreFiles && std::strcmp(finder->mName, ".") == 0 && finder->mFileIsFolder);
        finder->findNextFile();
        finder->findNextFile();
        CHECK(!finder->mHasMoreFiles);
        delete finder;
    }

    // A cached resource is copied as stored (still Yaz0), as on Wii.
    std::vector< u8 > readBuffer(128, 0xCC);
    CHECK(archive->readResource(readBuffer.data(), static_cast< u32 >(readBuffer.size()), "/sub/b.bin") == archive->getResSize(bStored));
    CHECK(std::memcmp(readBuffer.data(), "Yaz0", 4) == 0);

    // Uncached, reading decompresses Yaz0 through the JKRDecomp thread.
    CHECK(archive->detachResource(bStored));
    std::fill(readBuffer.begin(), readBuffer.end(), 0xCC);
    u32 readSize = archive->readResource(readBuffer.data(), static_cast< u32 >(readBuffer.size()), "/sub/b.bin");
    CHECK(readSize == bPlain.size());
    CHECK(std::memcmp(readBuffer.data(), bPlain.data(), bPlain.size()) == 0);
    std::vector< u8 > shortBuffer(16, 0);
    CHECK(archive->readResource(shortBuffer.data(), 16, static_cast< u16 >(0)) == 10);
    CHECK(std::memcmp(shortBuffer.data(), "hello-rarc", 10) == 0);

    // Detached resources lose their pointer identity until fetched again.
    CHECK(archive->detachResource(a));
    CHECK(archive->getResSize(a) == -1);
    CHECK(archive->detachResource(a) == false);
    CHECK(archive->getResource("/a.bin") == a);
    CHECK(archive->removeResource(a));
    CHECK(archive->getResSize(a) == -1);

    JKRFileLoader* volume = JKRFileLoader::sVolumeList.getFirst()->getObject();
    CHECK(volume == archive);
    CHECK(JKRFileLoader::getGlbResource("a.bin", nullptr) == a);

    s32 freeBefore = heap->getTotalFreeSize();
    archive->unmount();
    CHECK(JKRFileLoader::sVolumeList.getNumLinks() == 0);
    CHECK(std::memcmp(buffer, original.data(), original.size()) == 0);
    CHECK(heap->getTotalFreeSize() > freeBefore);

    // Malformed tables are rejected without touching the heap.
    std::vector< u8 > corrupt = original;
    size_t fileTable = 0x20 + PetariNative::readU32BE(&corrupt[0x20 + 0x0C]);
    putU32(corrupt, fileTable + 4, 0x11000000u | 0xFFFFFF);
    u8* corruptBuffer = copyToHeap(corrupt, heap);
    s32 freeCorrupt = heap->getTotalFreeSize();
    JKRMemArchive* rejected = new (heap, 0) JKRMemArchive();
    CHECK(rejected->mountFixed(corruptBuffer, JKR_MEM_BREAK_FLAG_0) == false);
    CHECK(!rejected->mIsMounted);
    delete rejected;
    CHECK(heap->getTotalFreeSize() == freeCorrupt);

    std::vector< u8 > badMagic = original;
    badMagic[0] = 'X';
    u8* badBuffer = copyToHeap(badMagic, heap);
    JKRMemArchive* badArchive = new (heap, 0) JKRMemArchive();
    CHECK(badArchive->mountFixed(badBuffer, JKR_MEM_BREAK_FLAG_0) == false);
    delete badArchive;

    JKRHeap::free(buffer, heap);
    JKRHeap::free(corruptBuffer, heap);
    JKRHeap::free(badBuffer, heap);
}

// Mounts every RARC archive under root with JKRMemArchive and compares each file
// entry with the independent native/resource parser.
void testDiscArchives(JKRHeap* heap, const std::filesystem::path& root) {
    int archives = 0;
    int resources = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (!item.is_regular_file() || item.path().extension() != ".arc") {
            continue;
        }

        std::ifstream stream(item.path(), std::ios::binary);
        std::vector< u8 > file((std::istreambuf_iterator< char >(stream)), std::istreambuf_iterator< char >());
        std::vector< u8 > plain;
        {
            JKRHeap::HostAllocationScope host;
            PetariNative::Resource::Bytes input{file.data(), file.size()};
            if (file.size() >= 4 && (std::memcmp(file.data(), "Yaz0", 4) == 0 || std::memcmp(file.data(), "Yay0", 4) == 0)) {
                plain = PetariNative::Resource::decompress(input);
            } else {
                plain = file;
            }
        }
        if (plain.size() < 4 || std::memcmp(plain.data(), "RARC", 4) != 0) {
            continue;
        }

        PetariNative::Resource::Archive reference = [&] {
            JKRHeap::HostAllocationScope host;
            return PetariNative::Resource::Archive::parse({plain.data(), plain.size()});
        }();

        u8* buffer = copyToHeap(plain, heap);
        JKRMemArchive* archive = new (heap, 0) JKRMemArchive();
        bool mounted = archive->mountFixed(buffer, JKR_MEM_BREAK_FLAG_0);
        CHECK(mounted);
        if (!mounted) {
            std::fprintf(stderr, "  mount failed: %s\n", item.path().c_str());
            delete archive;
            JKRHeap::free(buffer, heap);
            continue;
        }

        const auto& entries = reference.entries();
        CHECK(archive->mInfoBlock->mNrFiles == entries.size());
        CHECK(archive->mInfoBlock->mNrDirs == reference.directories().size());
        bool archiveOk = true;
        for (u32 i = 0; i < entries.size(); i++) {
            const auto& entry = entries[i];
            JKRArchive::SDirEntry dirEntry;
            archiveOk &= archive->getDirEntry(&dirEntry, i) && entry.name == dirEntry.mName && dirEntry.mFileFlag == entry.flags &&
                         dirEntry.mFileID == entry.id;
            if (entry.isDirectory()) {
                continue;
            }

            void* resource = archive->getIdxResource(i);
            auto stored = reference.storedData(i);
            archiveOk &= resource != nullptr && archive->getResSize(resource) == static_cast< s32 >(stored.size) &&
                         std::memcmp(resource, stored.data, stored.size) == 0;
            archiveOk &= (entry.id == 0xFFFF || archive->getResource(entry.id) == resource);
            resources++;

            // Loose textures: a normalized copy must describe an image inside the file.
            if (entry.name.size() > 4 && entry.name.compare(entry.name.size() - 4, 4, ".bti") == 0 && !entry.isCompressed()) {
                std::vector< u8 > copy(static_cast< const u8* >(resource), static_cast< const u8* >(resource) + stored.size);
                ResTIMG* timg = reinterpret_cast< ResTIMG* >(copy.data());
                bool normalized = JUTNativeNormalizeResTIMG(timg, static_cast< u32 >(copy.size()));
                bool textureOk = normalized && timg->mWidth > 0 && timg->mWidth <= 1024 && timg->mHeight > 0 && timg->mHeight <= 1024 &&
                                 timg->mImageDataOffset >= sizeof(ResTIMG) && timg->mImageNum > 0;
                if (!textureOk) {
                    std::fprintf(stderr, "  bad texture %s in %s\n", entry.name.c_str(), item.path().c_str());
                }
                archiveOk &= textureOk;
                sTextures++;
            }
        }
        CHECK(archiveOk);
        if (!archiveOk) {
            std::fprintf(stderr, "  mismatch: %s\n", item.path().c_str());
        }

        archive->unmount();
        JKRHeap::free(buffer, heap);
        archives++;
    }

    std::printf("disc archives: %d mounted, %d resources compared, %d textures normalized\n", archives, resources, sTextures);
    CHECK(archives > 0);
}

// Unit heaps on 64-bit addresses: the unit area must stay inside the heap's block
// (a u32 alignment mask once truncated it, giving ~2^32 / unitSize units).
void testUnitHeap(JKRHeap* heap) {
    const u32 size = 24856;
    JKRUnitHeap* units = JKRUnitHeap::create(96, size, 8, heap, false);
    CHECK(units != nullptr);
    if (units == nullptr) {
        return;
    }
    u8* start = reinterpret_cast< u8* >(units);
    CHECK(units->mUnits > start && units->mUnits + units->mUnitCount * units->mUnitSize <= start + size);
    CHECK(units->mUnitCount > 0 && units->mUnitCount <= size / 96);

    const s32 freeBefore = units->getTotalFreeSize();
    std::vector< void* > blocks;
    bool ok = true;
    for (u32 i = 0; i <= units->mUnitCount; i++) {
        void* block = units->alloc(96, 8);
        if (block == nullptr) {
            break;
        }
        ok &= static_cast< u8* >(block) >= units->mUnits && static_cast< u8* >(block) + 96 <= start + size &&
              reinterpret_cast< uintptr_t >(block) % 8 == 0;
        blocks.push_back(block);
    }
    CHECK(ok && blocks.size() == units->mUnitCount);
    for (void* block : blocks) {
        units->free(block);
    }
    CHECK(units->getTotalFreeSize() == freeBefore);
    JKRHeap::destroy(units);
}

// JKRDvdFile::readData completes through DVDReadAsyncPrio -> doneProcess, which
// finds the JKRDvdFile from its DVDFileInfo. Reads a synthetic disc on the
// platform DVD drive thread.
void testDvdFileRead() {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "petari_jkr_dvd_test";
    std::vector< u8 > contents(96 * 1024);
    {
        JKRHeap::HostAllocationScope host;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "files");
        for (size_t i = 0; i < contents.size(); i++) {
            contents[i] = static_cast< u8 >(i * 31 + (i >> 8));
        }
        std::ofstream(root / "files" / "data.bin", std::ios::binary).write(reinterpret_cast< const char* >(contents.data()), contents.size());
        PetariNative::Platform::DVD::MountOptions options;
        options.root = root;
        options.ignoreDiscFst = true;
        std::string error;
        bool mounted = PetariNative::Platform::DVD::mount(options, &error);
        CHECK(mounted);
        if (!mounted) {
            std::fprintf(stderr, "  mount failed: %s\n", error.c_str());
            return;
        }
    }
    DVDInit();

    JKRDvdFile file;
    CHECK(file.open("/data.bin"));
    CHECK(file.mDvdFile == &file && file.getFileSize() == contents.size());

    alignas(32) static u8 buffer[32 * 1024];
    const s32 offsets[] = {0, 32768, 65536};
    for (s32 offset : offsets) {
        std::memset(buffer, 0, sizeof(buffer));
        s32 read = file.readData(buffer, sizeof(buffer), offset);
        CHECK(read == static_cast< s32 >(sizeof(buffer)));
        CHECK(std::memcmp(buffer, contents.data() + offset, sizeof(buffer)) == 0);
    }
    file.close();

    JKRHeap::HostAllocationScope host;
    std::filesystem::remove_all(root);
}
}  // namespace

int main(int argc, char** argv) {
    OSInit();
    PetariNative::setGameAllocationThread(true);
    JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
    CHECK(root != nullptr);
    if (root == nullptr) {
        return 1;
    }
    root->becomeCurrentHeap();
    root->becomeSystemHeap();
    JKRExpHeap* heap = JKRExpHeap::create(64 * 1024 * 1024, root, false);
    CHECK(heap != nullptr);
    heap->becomeCurrentHeap();
    JKRDecomp::create(8);

    testSyntheticArchive(heap);
    testUnitHeap(heap);
    testDvdFileRead();

    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--assets") == 0) {
            testDiscArchives(heap, argv[i + 1]);
        }
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d archive check(s) failed\n", sFailures, sChecks);
        return 1;
    }

    std::printf("jkr archive tests passed (%d checks)\n", sChecks);
    return 0;
}

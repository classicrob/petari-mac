#pragma once
// In-memory Wii file system table backed by host files.
//
// Mirrors the disc FST layout used by the SDK's dvdfs.c: entry 0 is the root
// directory, directories store their parent and the index one past their last
// descendant, files store their disc position (in 4-byte words, Wii layout
// format 0) and byte length. Each file entry is also bound to a host path.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PetariNative::Platform::DVD {

// Big-endian field readers for disc structures.
inline std::uint32_t readU24BE(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
}
inline std::uint32_t readU32BE(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

struct FstEntry {
    bool isDir = false;
    std::uint32_t nameOffset = 0;
    // Directory: parent entry. File: disc position in words.
    std::uint32_t parentOrPosition = 0;
    // Directory: one past the last descendant. File: length in bytes.
    std::uint32_t nextOrLength = 0;
};

class Fst {
public:
    // Parses a Wii fst.bin image (big-endian) and binds every file to
    // filesDirectory/<path>. Fails if a listed file is missing, is not a
    // regular file, or its size differs from the FST.
    static bool fromDiscImage(const std::vector<std::uint8_t>& image, const std::filesystem::path& filesDirectory, Fst& out,
                              std::string* error);

    // Builds an FST by scanning filesDirectory. Siblings are ordered by ASCII
    // upper-cased name, then by exact name, which is the ordering of Nintendo's
    // disc mastering tools. Files get synthetic, 32 KiB aligned disc positions.
    // macOS metadata files (.DS_Store, AppleDouble "._*") are not part of discs
    // and are skipped.
    static bool fromDirectory(const std::filesystem::path& filesDirectory, Fst& out, std::string* error);

    std::uint32_t entryCount() const { return static_cast<std::uint32_t>(mEntries.size()); }
    std::uint32_t fileCount() const { return mFileCount; }
    const FstEntry& entry(std::uint32_t index) const { return mEntries[index]; }
    // Stable for the lifetime of this object; DVDDirEntry::name points here.
    char* name(std::uint32_t index) { return mStrings.data() + mEntries[index].nameOffset; }
    const char* name(std::uint32_t index) const { return mStrings.data() + mEntries[index].nameOffset; }
    const std::filesystem::path& hostPath(std::uint32_t index) const { return mHostPaths[index]; }

    // Same resolution rules as the SDK's DVDConvertPathToEntrynum: ASCII
    // case-insensitive names, "/" restarts at the root, "." and ".." are
    // honoured, a trailing "/" requires a directory. Returns -1 if not found.
    std::int32_t convertPathToEntrynum(const char* path, std::uint32_t currentDirectory) const;

    // Writes the absolute path of an entry the way the SDK's
    // DVDConvertEntrynumToPath does (directories end with "/"). Returns false
    // if it had to be truncated to maxlen.
    bool convertEntrynumToPath(std::uint32_t entryNum, char* path, std::uint32_t maxlen) const;

    struct Extent {
        std::uint64_t start = 0;   // disc byte offset
        std::uint64_t length = 0;  // bytes
        std::uint32_t entry = 0;
    };
    // File extents sorted by start; non-overlapping.
    const std::vector<Extent>& extents() const { return mExtents; }
    // One past the last byte readable from this disc (end of the last file plus
    // the 32-byte read tolerance DVDRead allows, rounded to 32 KiB).
    std::uint64_t discEnd() const { return mDiscEnd; }

private:
    bool finish(std::string* error);
    std::uint32_t entryToPath(std::uint32_t entry, char* path, std::uint32_t maxlen) const;

    std::vector<FstEntry> mEntries;
    std::vector<char> mStrings;
    std::vector<std::filesystem::path> mHostPaths;
    std::vector<Extent> mExtents;
    std::uint32_t mFileCount = 0;
    std::uint64_t mDiscEnd = 0;
};

}  // namespace PetariNative::Platform::DVD

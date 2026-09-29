#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PetariNative::Resource {
struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

// Malformed input and configured size limits are reported with std::runtime_error.
std::vector<std::uint8_t> decompress(Bytes input, std::size_t limit = 256 * 1024 * 1024);

struct ArchiveEntry {
    std::uint16_t id;
    std::uint16_t hash;
    std::uint8_t flags;
    std::string name;
    std::uint32_t offset;
    std::uint32_t size;
    bool isDirectory() const { return (flags & 2) != 0; }
    bool isCompressed() const { return (flags & 4) != 0; }
};

struct ArchiveDirectory {
    std::uint32_t type;
    std::string name;
    std::uint16_t hash;
    std::uint16_t entryCount;
    std::uint32_t firstEntry;
};

// Owns host metadata separately from serialized big-endian records and payloads.
class Archive {
public:
    static Archive parse(Bytes input, std::size_t limit = 256 * 1024 * 1024);
    const std::vector<ArchiveEntry>& entries() const { return entries_; }
    const std::vector<ArchiveDirectory>& directories() const { return directories_; }
    Bytes storedData(std::size_t entry) const;
    std::vector<std::uint8_t> resourceData(std::size_t entry, std::size_t limit = 256 * 1024 * 1024) const;
private:
    std::vector<std::uint8_t> storage_;
    std::vector<ArchiveEntry> entries_;
    std::vector<ArchiveDirectory> directories_;
    std::size_t dataStart_ = 0;
};
}

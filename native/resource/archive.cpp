#include "archive.hpp"
#include <petari/endian.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace PetariNative::Resource {
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool contains(std::size_t size, std::size_t offset, std::size_t length) {
    return offset <= size && length <= size - offset;
}
bool magic(Bytes bytes, const char* text) {
    return bytes.size >= 4 && std::memcmp(bytes.data, text, 4) == 0;
}
std::string stringAt(Bytes table, std::size_t offset) {
    require(offset < table.size, "RARC string offset outside table");
    const auto* start = reinterpret_cast<const char*>(table.data + offset);
    const auto* end = static_cast<const char*>(std::memchr(start, 0, table.size - offset));
    require(end != nullptr, "RARC string is not terminated");
    return std::string(start, end);
}
}

std::vector<std::uint8_t> decompress(Bytes input, std::size_t limit) {
    require(input.data != nullptr || input.size == 0, "Null compressed input");
    const bool yaz = magic(input, "Yaz0");
    const bool yay = magic(input, "Yay0");
    require(yaz || yay, "Unsupported compression magic");
    require(input.size >= 16, "Truncated compression header");
    const std::size_t size = readU32BE(input.data + 4);
    require(size <= limit, "Decompressed data exceeds size limit");
    std::vector<std::uint8_t> result;
    result.reserve(size);
    std::size_t masks = 16;
    std::size_t links = yay ? readU32BE(input.data + 8) : 16;
    std::size_t literals = yay ? readU32BE(input.data + 12) : 16;
    if (yay) require(links >= 16 && literals >= links && literals <= input.size, "Invalid Yay0 stream offsets");
    const std::size_t maskEnd = yay ? links : input.size;
    const std::size_t linkEnd = yay ? literals : input.size;
    unsigned bits = 0;
    std::uint32_t mask = 0;
    auto byte = [&](std::size_t& cursor, std::size_t end) {
        require(cursor < end, "Truncated compressed stream");
        return input.data[cursor++];
    };
    while (result.size() < size) {
        if (!bits) {
            if (yay) {
                require(contains(maskEnd, masks, 4), "Truncated Yay0 mask stream");
                mask = readU32BE(input.data + masks);
                masks += 4;
                bits = 32;
            } else {
                mask = std::uint32_t(byte(literals, input.size)) << 24;
                bits = 8;
            }
        }
        if (mask & 0x80000000u) {
            result.push_back(byte(literals, input.size));
        } else {
            auto& cursor = yay ? links : literals;
            const auto a = byte(cursor, linkEnd);
            const auto b = byte(cursor, linkEnd);
            const std::size_t distance = ((a & 15u) << 8 | b) + 1;
            std::size_t length = a >> 4;
            length = length ? length + 2 : std::size_t(byte(literals, input.size)) + 18;
            require(distance <= result.size(), "Compressed back-reference precedes output");
            require(length <= size - result.size(), "Compressed run exceeds declared output");
            while (length--) result.push_back(result[result.size() - distance]);
        }
        mask <<= 1;
        --bits;
    }
    return result;
}

Archive Archive::parse(Bytes input, std::size_t limit) {
    require(input.data != nullptr || input.size == 0, "Null archive input");
    Archive result;
    if (magic(input, "Yaz0") || magic(input, "Yay0")) {
        result.storage_ = decompress(input, limit);
    } else {
        require(input.size <= limit, "Archive exceeds size limit");
        require(input.size >= 32, "Truncated RARC header");
        result.storage_.assign(input.data, input.data + input.size);
    }
    const Bytes bytes{result.storage_.data(), result.storage_.size()};
    require(bytes.size >= 32 && magic(bytes, "RARC"), "Invalid RARC header");
    const auto* p = bytes.data;
    const std::size_t fileSize = readU32BE(p + 4);
    const std::size_t headerSize = readU32BE(p + 8);
    require(fileSize >= 64 && fileSize <= bytes.size, "RARC file size outside input");
    require(headerSize >= 32 && contains(fileSize, headerSize, 32), "RARC info block outside input");
    const std::size_t dataOffset = readU32BE(p + 12);
    const std::size_t dataSize = readU32BE(p + 16);
    require(contains(fileSize - headerSize, dataOffset, dataSize), "RARC data block outside input");
    result.dataStart_ = headerSize + dataOffset;
    const auto* info = p + headerSize;
    const std::size_t dirCount = readU32BE(info);
    const std::size_t dirOffset = readU32BE(info + 4);
    const std::size_t entryCount = readU32BE(info + 8);
    const std::size_t entryOffset = readU32BE(info + 12);
    const std::size_t stringSize = readU32BE(info + 16);
    const std::size_t stringOffset = readU32BE(info + 20);
    const auto metadataSize = fileSize - headerSize;
    require(dirCount > 0 && dirCount <= metadataSize / 16 && contains(metadataSize, dirOffset, dirCount * 16), "RARC directory table outside input");
    require(entryCount <= metadataSize / 20 && contains(metadataSize, entryOffset, entryCount * 20), "RARC entry table outside input");
    require(contains(metadataSize, stringOffset, stringSize), "RARC string table outside input");
    const Bytes strings{info + stringOffset, stringSize};
    result.directories_.reserve(dirCount);
    result.entries_.reserve(entryCount);
    for (std::size_t i = 0; i < dirCount; ++i) {
        const auto* d = info + dirOffset + i * 16;
        ArchiveDirectory dir{readU32BE(d), stringAt(strings, readU32BE(d + 4)), readU16BE(d + 8), readU16BE(d + 10), readU32BE(d + 12)};
        require(contains(entryCount, dir.firstEntry, dir.entryCount), "RARC directory entry range outside table");
        result.directories_.push_back(std::move(dir));
    }
    for (std::size_t i = 0; i < entryCount; ++i) {
        const auto* e = info + entryOffset + i * 20;
        ArchiveEntry entry{readU16BE(e), readU16BE(e + 2), e[4], stringAt(strings, readU24BE(e + 5)), readU32BE(e + 8), readU32BE(e + 12)};
        if (entry.isDirectory()) {
            require(entry.offset < dirCount || (entry.name == ".." && entry.offset == 0xffffffffu), "RARC directory target outside table");
        } else {
            require((entry.flags & 1) != 0, "RARC entry has no file or directory flag");
            require(contains(dataSize, entry.offset, entry.size), "RARC file payload outside data block");
        }
        result.entries_.push_back(std::move(entry));
    }
    return result;
}

Bytes Archive::storedData(std::size_t index) const {
    if (index >= entries_.size()) throw std::out_of_range("RARC entry index");
    const auto& entry = entries_[index];
    require(!entry.isDirectory(), "RARC directory has no file payload");
    return {storage_.data() + dataStart_ + entry.offset, entry.size};
}

std::vector<std::uint8_t> Archive::resourceData(std::size_t index, std::size_t limit) const {
    const auto data = storedData(index);
    if (entries_[index].isCompressed()) return decompress(data, limit);
    require(data.size <= limit, "RARC resource exceeds size limit");
    return {data.data, data.data + data.size};
}
}

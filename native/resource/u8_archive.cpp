#include "u8_archive.hpp"
#include <petari/endian.hpp>
#include <cstring>
#include <stdexcept>

namespace PetariNative::Resource {
std::vector<U8Entry> parseU8(Bytes input) {
    auto require = [](bool value, const char* reason) {
        if (!value) throw std::runtime_error(reason);
    };
    require(input.data && input.size >= 32 && readU32BE(input.data) == 0x55aa382d, "Invalid U8 archive header");
    const std::size_t tableStart = readU32BE(input.data + 4);
    const std::size_t tableSize = readU32BE(input.data + 8);
    const std::size_t fileStart = readU32BE(input.data + 12);
    require(tableStart >= 32 && tableStart <= input.size && tableSize <= input.size - tableStart && tableSize >= 12,
            "U8 file table outside archive");
    require(fileStart >= tableStart + tableSize && fileStart <= input.size, "U8 data offset outside archive");
    const auto* table = input.data + tableStart;
    const std::size_t count = readU32BE(table + 8);
    require(table[0] == 1 && readU32BE(table + 4) == 0 && count > 0 && count <= tableSize / 12, "Invalid U8 root node");
    const auto* strings = reinterpret_cast<const char*>(table + count * 12);
    const std::size_t stringSize = tableSize - count * 12;
    std::vector<U8Entry> entries;
    entries.reserve(count);
    std::vector<std::size_t> parents;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* node = table + i * 12;
        const std::size_t nameOffset = readU24BE(node + 1);
        require(node[0] <= 1 && nameOffset < stringSize, "Invalid U8 node type or name offset");
        const auto* name = strings + nameOffset;
        const auto* end = static_cast<const char*>(std::memchr(name, 0, stringSize - nameOffset));
        require(end != nullptr, "Unterminated U8 name");
        U8Entry entry{node[0] == 1, std::string(name, end), readU32BE(node + 4), readU32BE(node + 8)};
        while (!parents.empty() && i == entries[parents.back()].endOrSize) parents.pop_back();
        require(i == 0 || !parents.empty(), "U8 node outside root hierarchy");
        if (entry.directory) {
            require(entry.endOrSize > i && entry.endOrSize <= count, "Invalid U8 directory extent");
            require(i == 0 || (entry.parentOrOffset == parents.back() && entry.endOrSize <= entries[parents.back()].endOrSize),
                    "Invalid U8 directory parent");
            parents.push_back(i);
        } else {
            require(entry.parentOrOffset >= fileStart && entry.parentOrOffset <= input.size &&
                    entry.endOrSize <= input.size - entry.parentOrOffset, "U8 file payload outside archive");
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}
}

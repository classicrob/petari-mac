#pragma once
#include "archive.hpp"

namespace PetariNative::Resource {
struct U8Entry {
    bool directory;
    std::string name;
    std::uint32_t parentOrOffset;
    std::uint32_t endOrSize;
};

// Validates all tables, hierarchy, strings and file extents before SDK ARC use.
std::vector<U8Entry> parseU8(Bytes input);
}

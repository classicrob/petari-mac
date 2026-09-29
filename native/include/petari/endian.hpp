#pragma once
#include <cstdint>

namespace PetariNative {
inline std::uint16_t readU16BE(const void* data) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    return (std::uint16_t(p[0]) << 8) | p[1];
}
inline std::uint32_t readU24BE(const void* data) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
}
inline std::uint32_t readU32BE(const void* data) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
}

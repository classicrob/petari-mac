#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>

namespace PetariNative::GX {
// Aurora's pinned FIFO extension carries pointers without the Wii CP truncation.
constexpr std::uint8_t extensionOpcode = 0x50;
constexpr std::uint16_t arrayBaseCommand = 0x10;

inline std::array<std::uint8_t, 22> arrayCommand(std::uint8_t index, const void* data,
                                                std::uint32_t size, std::uint8_t stride, bool littleEndian) {
    if (index >= 16) std::abort();
    std::array<std::uint8_t, 22> result{};
    result[0] = extensionOpcode;
    result[2] = static_cast<std::uint8_t>(arrayBaseCommand | index);
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    for (unsigned i = 0; i < 8; ++i) result[3 + i] = static_cast<std::uint8_t>(address >> ((7 - i) * 8));
    for (unsigned i = 0; i < 4; ++i) result[11 + i] = static_cast<std::uint8_t>(size >> ((3 - i) * 8));
    result[15] = littleEndian ? 1 : 0;
    result[16] = 0x08;
    result[17] = 0xB0 | index;
    result[21] = stride;
    return result;
}
}

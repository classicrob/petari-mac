#pragma once

// Native-only views of big-endian resource data (BCSV, BMG, ...).

#ifdef PETARI_NATIVE
#include <cstring>
#include <petari/endian.hpp>
#include <revolution/types.h>
#include <type_traits>

/// @brief Big-endian integer or f32 stored in a Wii resource. It has the serialized size
/// and byte alignment and converts to the host value on read, so resource structs keep
/// their Wii field names and layout while the bytes stay in file order.
template < typename T >
struct BigEndianValue {
    static_assert(sizeof(T) == 2 || sizeof(T) == 4, "BigEndianValue supports 16- and 32-bit fields");

    operator T() const {
        if constexpr (sizeof(T) == 2) {
            return static_cast< T >(PetariNative::readU16BE(mBytes));
        } else if constexpr (std::is_floating_point< T >::value) {
            const u32 bits = PetariNative::readU32BE(mBytes);
            T value;
            memcpy(&value, &bits, sizeof(value));
            return value;
        } else {
            return static_cast< T >(PetariNative::readU32BE(mBytes));
        }
    }

    u8 mBytes[sizeof(T)];
};
#endif

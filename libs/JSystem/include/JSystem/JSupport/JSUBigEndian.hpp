#pragma once

#include <revolution/types.h>

// Field type for serialized big-endian data that is read in place.
// JSU_BE(T) is T on the Wii. On native builds it has T's size and alignment and
// converts to and from T in big-endian byte order, so structures that overlay
// disc data keep their layout and field reads return host values.
#ifdef PETARI_NATIVE
#include <cstring>

template < typename T, unsigned Align = alignof(T) >
struct alignas(Align) JSUBigEndian {
    operator T() const {
        return get();
    }

    T get() const {
        u8 bytes[sizeof(T)];
        for (unsigned i = 0; i < sizeof(T); i++) {
            bytes[i] = mBytes[sizeof(T) - 1 - i];
        }
        T value;
        memcpy(&value, bytes, sizeof(T));
        return value;
    }

    JSUBigEndian& operator=(T value) {
        u8 bytes[sizeof(T)];
        memcpy(bytes, &value, sizeof(T));
        for (unsigned i = 0; i < sizeof(T); i++) {
            mBytes[i] = bytes[sizeof(T) - 1 - i];
        }
        return *this;
    }

    u8 mBytes[sizeof(T)];
};

#define JSU_BE(T) JSUBigEndian< T >
// For data that may sit at unaligned addresses (the Wii CPU loads those in
// hardware). Only for structures whose fields are already packed, so the
// layout matches JSU_BE.
#define JSU_BE_UNALIGNED(T) JSUBigEndian< T, 1 >
#else
#define JSU_BE(T) T
#define JSU_BE_UNALIGNED(T) T
#endif

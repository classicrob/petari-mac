#pragma once

#include <revolution/types.h>
#ifdef PETARI_NATIVE
#include <cstdint>
#endif

class HashSortTable {
public:
#ifdef PETARI_NATIVE
    /// @brief Payload stored next to each hash. Some tables store object pointers, so the
    /// native payload is pointer-sized. Hash codes remain `u32` to match serialized data.
    typedef uintptr_t Value;
#else
    typedef u32 Value;
#endif

    HashSortTable(u32);

    bool add(const char*, Value, bool);
    bool add(u32, Value);
    bool addOrSkip(u32, Value);
    void sort();
    bool search(u32, u32*);
    bool search(const char*, u32*);
    bool search(const char*, const char*, u32*);
    void swap(const char*, const char*);

#ifdef PETARI_NATIVE
    /// @brief Looks up a payload without narrowing it to `u32`.
    bool searchValue(u32, Value*);
    bool searchValue(const char*, Value*);

    /// @brief Adds an object pointer payload. Native replacement for `add(pName, (u32)pObj, skip)`.
    template < typename T >
    bool addPtr(const char* pName, T* pObj, bool isValidSkip) {
        return add(pName, reinterpret_cast< Value >(pObj), isValidSkip);
    }

    /// @brief Looks up an object pointer payload stored with `addPtr`. `*ppObj` is null if not found.
    template < typename T >
    bool searchPtr(const char* pName, T** ppObj) {
        Value value;
        bool isFound = searchValue(pName, &value);
        *ppObj = reinterpret_cast< T* >(value);
        return isFound;
    }
#endif

    /* 0x00 */ bool mHasBeenSorted;
    /* 0x04 */ u32* mHashCodes;
    /* 0x08 */ Value* _8;
    /* 0x0C */ u16* _C;
    /* 0x10 */ u16* _10;
    /* 0x14 */ u32 mCurrentLength;
    /* 0x18 */ u32 mMaxLength;
};

namespace MR {
    u32 getHashCode(const char*);
    u32 getHashCodeLower(const char*);
};  // namespace MR

#include "Game/Speaker/SpkTable.hpp"
#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#endif

SpkTable::SpkTable() {
    mInitialized = false;
    mResourceCount = 0;
    mParameters = nullptr;
    mNames = nullptr;
}

#ifdef PETARI_NATIVE
// Resource layout is SpkFile (big-endian); the names table holds s32 offsets from the
// resource start. Names are resolved into a host pointer table instead of in place.
void SpkTable::setResource(void* pRes) {
    mInitialized = false;

    u8* res = static_cast< u8* >(pRes);
    mResourceCount = PetariNative::readU32BE(res + 0x0);
    mParameters = reinterpret_cast< SpkParameters* >(res + PetariNative::readU32BE(res + 0x4));
    u32 namesOff = PetariNative::readU32BE(res + 0x8);

    const char** names = new const char*[mResourceCount];
    for (s32 i = 0; i < mResourceCount; i++) {
        names[i] = reinterpret_cast< const char* >(res + PetariNative::readU32BE(res + namesOff + i * 4));
    }

    delete[] mNames;
    mNames = names;
    mInitialized = true;
}
#else
void SpkTable::setResource(void* pRes) {
    mInitialized = false;

    s32* cursor = (s32*)pRes;

    s32 resourceCount = *cursor++;
    s32 entryOff = *cursor++;
    s32 dataOffsetsStartOff = *cursor++;
    s32* pIsDataOffsetsInitialized = cursor;
    BOOL isDataOffsetsInitialized = *cursor++;

    mResourceCount = resourceCount;

    SpkParameters* entryOffset = (SpkParameters*)((s32)pRes + entryOff);
    mParameters = entryOffset;
    const char** names = (const char**)((s32)pRes + dataOffsetsStartOff);
    if (!isDataOffsetsInitialized) {
        for (s32 i = 0; i < mResourceCount; i++) {
            names[i] += (s32)pRes;
        }
    }

    mNames = names;
    *pIsDataOffsetsInitialized = TRUE;
    mInitialized = true;
}
#endif

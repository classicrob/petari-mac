// Native byte-order normalization for JUtility resource headers. See
// JUTNativeNormalizeResTIMG and JUTNativeNormalizeResNTAB.
#ifdef PETARI_NATIVE
#include "JSystem/JUtility/JUTNameTab.hpp"
#include "JSystem/JUtility/JUTTexture.hpp"
#include <petari/endian.hpp>
#include <cstring>

bool JUTNativeNormalizeResTIMG(ResTIMG* pHeader, u32 resourceSize) {
    if (pHeader == nullptr || resourceSize < sizeof(ResTIMG)) {
        return false;
    }

    const u8* raw = reinterpret_cast< const u8* >(pHeader);
    u16 width = PetariNative::readU16BE(raw + 0x02);
    u16 height = PetariNative::readU16BE(raw + 0x04);
    u16 paletteNum = PetariNative::readU16BE(raw + 0x0A);
    u32 paletteOffset = PetariNative::readU32BE(raw + 0x0C);
    s16 lodBias = static_cast< s16 >(PetariNative::readU16BE(raw + 0x1A));
    u32 imageOffset = PetariNative::readU32BE(raw + 0x1C);

    // Palette entries are 16 bits. An offset of zero means the field is unused.
    if (imageOffset != 0 && imageOffset >= resourceSize) {
        return false;
    }
    if (paletteNum != 0 && (paletteOffset > resourceSize || paletteNum * 2u > resourceSize - paletteOffset)) {
        return false;
    }

    pHeader->mWidth = width;
    pHeader->mHeight = height;
    pHeader->mPaletteNum = paletteNum;
    pHeader->mPaletteDataOffset = paletteOffset;
    pHeader->mLodBias = lodBias;
    pHeader->mImageDataOffset = imageOffset;
    return true;
}

bool JUTNativeNormalizeResNTAB(ResNTAB* pTable, u32 resourceSize) {
    const u32 headerSize = 4;
    const u32 entrySize = sizeof(ResNTAB::Entry);
    if (pTable == nullptr || resourceSize < headerSize) {
        return false;
    }

    u8* raw = reinterpret_cast< u8* >(pTable);
    u16 count = PetariNative::readU16BE(raw);
    if (count > (resourceSize - headerSize) / entrySize) {
        return false;
    }

    // Names must start inside the resource and end with a NUL inside it.
    for (u32 i = 0; i < count; i++) {
        u16 offset = PetariNative::readU16BE(raw + headerSize + i * entrySize + 2);
        if (offset >= resourceSize || std::memchr(raw + offset, 0, resourceSize - offset) == nullptr) {
            return false;
        }
    }

    pTable->mEntryNum = count;
    pTable->_2 = PetariNative::readU16BE(raw + 2);
    for (u32 i = 0; i < count; i++) {
        const u8* entry = raw + headerSize + i * entrySize;
        u16 keyCode = PetariNative::readU16BE(entry);
        u16 offset = PetariNative::readU16BE(entry + 2);
        pTable->mEntries[i].mKeyCode = keyCode;
        pTable->mEntries[i].mOffs = offset;
    }
    return true;
}
#endif

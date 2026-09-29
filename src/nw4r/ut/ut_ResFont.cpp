#include "nw4r/ut/ResFont.h"
#include "nw4r/ut/binaryFileFormat.h"
#include <stdint.h>

#ifdef PETARI_NATIVE
#include <petari/endian.hpp>
#include <cstring>
#include <new>
#endif

namespace nw4r {
    namespace ut {
        namespace {
            template < typename T >
            inline void ResolveOffset(T*& rpPtr, void* pBase) {
                rpPtr = reinterpret_cast< T* >(static_cast< char* >(pBase) + reinterpret_cast< intptr_t >(rpPtr));
            }

        }  // namespace

#ifdef PETARI_NATIVE
        namespace {
            const u32 kMaxListBlocks = 64;

            // FontCodeMap::mappingMethod values, as read by ResFontBase::FindGlyphIndex.
            enum { FONT_MAPMETHOD_DIRECT = 0, FONT_MAPMETHOD_TABLE = 1, FONT_MAPMETHOD_SCAN = 2 };

            struct NativeFontBuilder {
                const u8* file;
                u32 fileSize;
                u8* cursor;
                u32 widthOffsets[kMaxListBlocks];
                FontWidth* widths[kMaxListBlocks];
                u32 widthNum;
                u32 mapOffsets[kMaxListBlocks];
                FontCodeMap* maps[kMaxListBlocks];
                u32 mapNum;
            };

            u32 AlignNative(u32 size) {
                return (size + 7) & ~7u;
            }

            // Number of u16 mapInfo entries for a serialized code map, or 0 if invalid.
            u32 CodeMapInfoCount(const u8* map, u32 available) {
                u16 ccodeBegin = PetariNative::readU16BE(map + 0);
                u16 ccodeEnd = PetariNative::readU16BE(map + 2);
                u16 method = PetariNative::readU16BE(map + 4);
                const u32 headerSize = 12;
                if (available < headerSize + 2 || ccodeEnd < ccodeBegin) {
                    return 0;
                }

                u32 count;
                switch (method) {
                case FONT_MAPMETHOD_DIRECT:
                    count = 1;
                    break;
                case FONT_MAPMETHOD_TABLE:
                    count = ccodeEnd - ccodeBegin + 1u;
                    break;
                case FONT_MAPMETHOD_SCAN:
                    count = 1 + 2u * PetariNative::readU16BE(map + headerSize);
                    break;
                default:
                    return 0;
                }
                return count * 2 <= available - headerSize ? count : 0;
            }

            FontWidth* FindWidth(const NativeFontBuilder& builder, u32 offset) {
                for (u32 i = 0; i < builder.widthNum; i++) {
                    if (builder.widthOffsets[i] == offset) {
                        return builder.widths[i];
                    }
                }
                return nullptr;
            }

            FontCodeMap* FindMap(const NativeFontBuilder& builder, u32 offset) {
                for (u32 i = 0; i < builder.mapNum; i++) {
                    if (builder.mapOffsets[i] == offset) {
                        return builder.maps[i];
                    }
                }
                return nullptr;
            }

            // Builds host-layout FINF/TGLP/CWDH/CMAP structures for a big-endian
            // RFNT file. Offsets in the file are relative to its start. Glyph sheets
            // stay in the caller's buffer. Returns nullptr for a malformed file.
            FontInformation* BuildNativeFont(void* brfnt, void** pNativeData) {
                const u8* file = static_cast< const u8* >(brfnt);
                if (PetariNative::readU32BE(file) != 'RFNT' || PetariNative::readU16BE(file + 4) != 0xFEFF) {
                    return nullptr;
                }

                u16 version = PetariNative::readU16BE(file + 6);
                u32 fileSize = PetariNative::readU32BE(file + 8);
                u16 headerSize = PetariNative::readU16BE(file + 12);
                u16 dataBlocks = PetariNative::readU16BE(file + 14);
                if ((version != 0x104 && version != 0x102) || dataBlocks < 2 || fileSize < sizeof(BinaryFileHeader) + sizeof(BinaryBlockHeader) * 2 ||
                    headerSize < sizeof(BinaryFileHeader) || headerSize > fileSize) {
                    return nullptr;
                }

                // First pass: validate blocks and size the native copy.
                u32 nativeSize = AlignNative(sizeof(FontInformation)) + AlignNative(sizeof(FontTextureGlyph));
                u32 offset = headerSize;
                bool hasInfo = false;
                bool hasGlyph = false;
                for (u32 n = 0; n < dataBlocks; n++) {
                    if (offset > fileSize || fileSize - offset < sizeof(BinaryBlockHeader)) {
                        return nullptr;
                    }
                    u32 kind = PetariNative::readU32BE(file + offset);
                    u32 size = PetariNative::readU32BE(file + offset + 4);
                    if (size < sizeof(BinaryBlockHeader) || size > fileSize - offset) {
                        return nullptr;
                    }
                    const u8* data = file + offset + sizeof(BinaryBlockHeader);
                    u32 dataSize = size - sizeof(BinaryBlockHeader);

                    switch (kind) {
                    case 'FINF':
                        hasInfo = dataSize >= 0x18;
                        if (!hasInfo) {
                            return nullptr;
                        }
                        break;
                    case 'TGLP':
                        hasGlyph = dataSize >= 0x18;
                        if (!hasGlyph) {
                            return nullptr;
                        }
                        break;
                    case 'CWDH': {
                        if (dataSize < 8) {
                            return nullptr;
                        }
                        u16 indexBegin = PetariNative::readU16BE(data);
                        u16 indexEnd = PetariNative::readU16BE(data + 2);
                        u32 count = indexEnd >= indexBegin ? indexEnd - indexBegin + 1u : 0;
                        if (count == 0 || count * sizeof(CharWidths) > dataSize - 8) {
                            return nullptr;
                        }
                        nativeSize += AlignNative(sizeof(FontWidth) + count * sizeof(CharWidths));
                        break;
                    }
                    case 'CMAP': {
                        u32 count = CodeMapInfoCount(data, dataSize);
                        if (count == 0) {
                            return nullptr;
                        }
                        nativeSize += AlignNative(sizeof(FontCodeMap) + count * sizeof(u16));
                        break;
                    }
                    case 'GLGR':
                        break;
                    default:
                        return nullptr;
                    }

                    offset += size;
                }

                if (!hasInfo || !hasGlyph) {
                    return nullptr;
                }

                u8* nativeData = static_cast< u8* >(::operator new(nativeSize));
                NativeFontBuilder builder;
                builder.file = file;
                builder.fileSize = fileSize;
                builder.cursor = nativeData;
                builder.widthNum = 0;
                builder.mapNum = 0;

                FontInformation* info = reinterpret_cast< FontInformation* >(builder.cursor);
                builder.cursor += AlignNative(sizeof(FontInformation));
                FontTextureGlyph* glyph = reinterpret_cast< FontTextureGlyph* >(builder.cursor);
                builder.cursor += AlignNative(sizeof(FontTextureGlyph));

                // Second pass: convert. Linked-list offsets are resolved afterwards.
                u32 infoOffset = 0;
                u32 glyphOffset = 0;
                u32 widthNext[kMaxListBlocks];
                u32 mapNext[kMaxListBlocks];
                bool valid = true;
                offset = headerSize;
                for (u32 n = 0; n < dataBlocks && valid; n++) {
                    u32 kind = PetariNative::readU32BE(file + offset);
                    u32 size = PetariNative::readU32BE(file + offset + 4);
                    u32 dataOffset = offset + sizeof(BinaryBlockHeader);
                    const u8* data = file + dataOffset;

                    switch (kind) {
                    case 'FINF':
                        infoOffset = dataOffset;
                        break;
                    case 'TGLP': {
                        glyphOffset = dataOffset;
                        glyph->cellWidth = data[0];
                        glyph->cellHeight = data[1];
                        glyph->baselinePos = static_cast< s8 >(data[2]);
                        glyph->maxCharWidth = data[3];
                        glyph->sheetSize = PetariNative::readU32BE(data + 4);
                        glyph->sheetNum = PetariNative::readU16BE(data + 8);
                        glyph->sheetFormat = PetariNative::readU16BE(data + 10);
                        glyph->sheetRow = PetariNative::readU16BE(data + 12);
                        glyph->sheetLine = PetariNative::readU16BE(data + 14);
                        glyph->sheetWidth = PetariNative::readU16BE(data + 16);
                        glyph->sheetHeight = PetariNative::readU16BE(data + 18);
                        u32 sheetOffset = PetariNative::readU32BE(data + 20);
                        u64 sheetBytes = static_cast< u64 >(glyph->sheetSize) * glyph->sheetNum;
                        valid = sheetOffset <= fileSize && sheetBytes <= fileSize - sheetOffset;
                        glyph->sheetImage = static_cast< u8* >(brfnt) + sheetOffset;
                        break;
                    }
                    case 'CWDH': {
                        if (builder.widthNum == kMaxListBlocks) {
                            valid = false;
                            break;
                        }
                        FontWidth* width = reinterpret_cast< FontWidth* >(builder.cursor);
                        width->indexBegin = PetariNative::readU16BE(data);
                        width->indexEnd = PetariNative::readU16BE(data + 2);
                        width->pNext = nullptr;
                        u32 count = width->indexEnd - width->indexBegin + 1u;
                        memcpy(width->widthTable, data + 8, count * sizeof(CharWidths));
                        builder.cursor += AlignNative(sizeof(FontWidth) + count * sizeof(CharWidths));
                        widthNext[builder.widthNum] = PetariNative::readU32BE(data + 4);
                        builder.widthOffsets[builder.widthNum] = dataOffset;
                        builder.widths[builder.widthNum++] = width;
                        break;
                    }
                    case 'CMAP': {
                        if (builder.mapNum == kMaxListBlocks) {
                            valid = false;
                            break;
                        }
                        FontCodeMap* map = reinterpret_cast< FontCodeMap* >(builder.cursor);
                        u32 count = CodeMapInfoCount(data, size - sizeof(BinaryBlockHeader));
                        map->ccodeBegin = PetariNative::readU16BE(data);
                        map->ccodeEnd = PetariNative::readU16BE(data + 2);
                        map->mappingMethod = PetariNative::readU16BE(data + 4);
                        map->reserved = PetariNative::readU16BE(data + 6);
                        map->pNext = nullptr;
                        for (u32 i = 0; i < count; i++) {
                            map->mapInfo[i] = PetariNative::readU16BE(data + 12 + i * 2);
                        }
                        builder.cursor += AlignNative(sizeof(FontCodeMap) + count * sizeof(u16));
                        mapNext[builder.mapNum] = PetariNative::readU32BE(data + 8);
                        builder.mapOffsets[builder.mapNum] = dataOffset;
                        builder.maps[builder.mapNum++] = map;
                        break;
                    }
                    }

                    offset += size;
                }

                for (u32 i = 0; i < builder.widthNum && valid; i++) {
                    if (widthNext[i] != 0) {
                        builder.widths[i]->pNext = FindWidth(builder, widthNext[i]);
                        valid = builder.widths[i]->pNext != nullptr;
                    }
                }
                for (u32 i = 0; i < builder.mapNum && valid; i++) {
                    if (mapNext[i] != 0) {
                        builder.maps[i]->pNext = FindMap(builder, mapNext[i]);
                        valid = builder.maps[i]->pNext != nullptr;
                    }
                }

                if (valid) {
                    const u8* src = file + infoOffset;
                    info->fontType = src[0];
                    info->linefeed = static_cast< s8 >(src[1]);
                    info->alterCharIndex = PetariNative::readU16BE(src + 2);
                    info->defaultWidth.left = static_cast< s8 >(src[4]);
                    info->defaultWidth.glyphWidth = src[5];
                    info->defaultWidth.charWidth = static_cast< s8 >(src[6]);
                    info->encoding = src[7];
                    u32 glyphRef = PetariNative::readU32BE(src + 8);
                    u32 widthRef = PetariNative::readU32BE(src + 12);
                    u32 mapRef = PetariNative::readU32BE(src + 16);
                    info->height = src[20];
                    info->width = src[21];
                    info->ascent = src[22];
                    info->padding_[0] = src[23];
                    info->pGlyph = glyphRef == glyphOffset ? glyph : nullptr;
                    info->pWidth = widthRef != 0 ? FindWidth(builder, widthRef) : nullptr;
                    info->pMap = mapRef != 0 ? FindMap(builder, mapRef) : nullptr;
                    valid = info->pGlyph != nullptr && (widthRef == 0 || info->pWidth != nullptr) && (mapRef == 0 || info->pMap != nullptr);
                }

                if (!valid) {
                    ::operator delete(nativeData);
                    return nullptr;
                }

                *pNativeData = nativeData;
                return info;
            }
        }  // namespace
#endif

        ResFont::ResFont() {
#ifdef PETARI_NATIVE
            mNativeFontData = nullptr;
#endif
        }

        ResFont::~ResFont() {
#ifdef PETARI_NATIVE
            ::operator delete(mNativeFontData);
#endif
        }

        bool ResFont::SetResource(void* brfnt) {
#ifdef PETARI_NATIVE
            if (!IsManaging(NULL)) {
                return false;
            }

            void* nativeData = nullptr;
            FontInformation* nativeInfo = BuildNativeFont(brfnt, &nativeData);
            if (nativeInfo == NULL) {
                return false;
            }

            mNativeFontData = nativeData;
            SetResourceBuffer(brfnt, nativeInfo);
            InitReaderFunc(GetEncoding());
            return true;
#endif

            FontInformation* pFontInfo = NULL;
            BinaryFileHeader* fileHeader = reinterpret_cast< BinaryFileHeader* >(brfnt);

            if (!IsManaging(NULL)) {
                return false;
            }
            if (fileHeader->signature == 'RFNU') {
                BinaryBlockHeader* blockHeader;
                int nBlocks = 0;

                blockHeader = reinterpret_cast< BinaryBlockHeader* >(reinterpret_cast< u8* >(fileHeader) + fileHeader->headerSize);

                while (nBlocks < fileHeader->dataBlocks) {
                    if (blockHeader->kind == 'FINF') {
                        pFontInfo = reinterpret_cast< FontInformation* >(reinterpret_cast< u8* >(blockHeader) + sizeof(*blockHeader));
                        break;
                    }

                    blockHeader = reinterpret_cast< BinaryBlockHeader* >(reinterpret_cast< u8* >(blockHeader) + blockHeader->size);
                    nBlocks++;
                }
            } else {
                if (fileHeader->version == 0x104) {
                    if (!IsValidBinaryFile(fileHeader, 'RFNT', 0x104, 2)) {
                        return false;
                    }
                } else {
                    if (!IsValidBinaryFile(fileHeader, 'RFNT', 0x102, 2)) {
                        return false;
                    }
                }
                pFontInfo = Rebuild(fileHeader);
            }

            if (pFontInfo == NULL) {
                return false;
            }

            SetResourceBuffer(brfnt, pFontInfo);
            InitReaderFunc(GetEncoding());

            return true;
        }

        void ResFont::RemoveResource() {
            RemoveResourceBuffer();
#ifdef PETARI_NATIVE
            ::operator delete(mNativeFontData);
            mNativeFontData = nullptr;
#endif
        }

        FontInformation* ResFont::Rebuild(BinaryFileHeader* fileHeader) {
            BinaryBlockHeader* blockHeader;
            FontInformation* info = nullptr;
            int nBlocks = 0;

            blockHeader = reinterpret_cast< BinaryBlockHeader* >(reinterpret_cast< u8* >(fileHeader) + fileHeader->headerSize);

            while (nBlocks < fileHeader->dataBlocks) {
                switch (blockHeader->kind) {
                case 'FINF': {
                    info = reinterpret_cast< FontInformation* >(reinterpret_cast< u8* >(blockHeader) + sizeof(*blockHeader));
                    ResolveOffset(info->pGlyph, fileHeader);

                    if (info->pWidth != nullptr) {
                        ResolveOffset(info->pWidth, fileHeader);
                    }
                    if (info->pMap != nullptr) {
                        ResolveOffset(info->pMap, fileHeader);
                    }
                } break;

                case 'TGLP': {
                    FontTextureGlyph* glyph = reinterpret_cast< FontTextureGlyph* >(reinterpret_cast< u8* >(blockHeader) + sizeof(*blockHeader));
                    ResolveOffset(glyph->sheetImage, fileHeader);
                } break;

                case 'CWDH': {
                    FontWidth* width = reinterpret_cast< FontWidth* >(reinterpret_cast< u8* >(blockHeader) + sizeof(*blockHeader));

                    if (width->pNext != nullptr) {
                        ResolveOffset(width->pNext, fileHeader);
                    }
                } break;

                case 'CMAP': {
                    FontCodeMap* map = reinterpret_cast< FontCodeMap* >(reinterpret_cast< u8* >(blockHeader) + sizeof(*blockHeader));

                    if (map->pNext != nullptr) {
                        ResolveOffset(map->pNext, fileHeader);
                    }
                } break;

                case 'GLGR': {
                } break;
                default:
                    return nullptr;
                }

                blockHeader = reinterpret_cast< BinaryBlockHeader* >(reinterpret_cast< u8* >(blockHeader) + blockHeader->size);
                nBlocks++;
            }

            fileHeader->signature = 'RFNU';

            return info;
        };  // namespace ut
    };  // namespace ut
};  // namespace nw4r

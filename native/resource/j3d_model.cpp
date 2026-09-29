// Conversion of big-endian J3D2 model files into host-layout images. See
// native/include/petari/j3d_model.hpp for the contract.
//
// Layouts follow the J3DGraphLoader structs (J3DModelLoader.hpp, J3DMaterialFactory.hpp,
// J3DShapeFactory.hpp, J3DJointFactory.hpp, J3DStruct.hpp, JUTTexture.hpp, JUTNameTab.hpp).
#include <petari/j3d_model.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

namespace PetariNative {
namespace J3D {
    namespace {
        const std::uint32_t cMagicJ3D2 = 0x4A334432;  // 'J3D2'
        const std::uint32_t cHeaderSize = 0x20;

        std::uint32_t tag(const char* name) {
            return static_cast< std::uint32_t >(static_cast< std::uint8_t >(name[0])) << 24 |
                   static_cast< std::uint32_t >(static_cast< std::uint8_t >(name[1])) << 16 |
                   static_cast< std::uint32_t >(static_cast< std::uint8_t >(name[2])) << 8 | static_cast< std::uint8_t >(name[3]);
        }

        std::uint32_t hostU32(const std::uint8_t* p) {
            std::uint32_t value;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        std::uint16_t hostU16(const std::uint8_t* p) {
            std::uint16_t value;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void swap16(std::uint8_t* p) {
            std::uint16_t value = readU16BE(p);
            std::memcpy(p, &value, sizeof(value));
        }

        void swap32(std::uint8_t* p) {
            std::uint32_t value = readU32BE(p);
            std::memcpy(p, &value, sizeof(value));
        }

        // A run of `count` scalars of `size` bytes at `offset` inside an element.
        struct Scalars {
            std::uint32_t mOffset;
            std::uint32_t mSize;
            std::uint32_t mCount;
        };

        struct Layout {
            std::uint32_t mElementSize;
            std::vector< Scalars > mScalars;  // bytes not listed stay as they are
        };

        const Layout cU8 = {1, {}};
        const Layout cU16 = {2, {{0, 2, 1}}};
        const Layout cU32 = {4, {{0, 4, 1}}};
        const Layout cColor = {4, {}};                                   // GXColor
        const Layout cColorS10 = {8, {{0, 2, 4}}};                       // GXColorS10
        const Layout cBytes4 = {4, {}};                                  // tev order, swap modes, blend, z mode, tex gens
        const Layout cBytes8 = {8, {}};                                  // color channels, alpha compare
        const Layout cTevStage = {20, {}};                               // J3DTevStageInfo
        const Layout cLight = {0x34, {{0x00, 4, 6}, {0x1C, 4, 6}}};      // J3DLightInfo
        const Layout cNbtScale = {0x10, {{0x04, 4, 3}}};                 // J3DNBTScaleInfo
        const Layout cFog = {0x2C, {{0x02, 2, 1}, {0x04, 4, 4}, {0x18, 2, 10}}};  // J3DFogInfo
        const Layout cTexMtx = {0x64, {{0x04, 4, 3}, {0x10, 4, 2}, {0x18, 2, 1}, {0x1C, 4, 2}, {0x24, 4, 16}}};  // J3DTexMtxInfo
        const Layout cMaterialInit = {0x14C, {{0x08, 2, (0x9C - 0x08) / 2}, {0xBC, 2, (0x14C - 0xBC) / 2}}};   // J3DMaterialInitData
        const Layout cIndInit = {0x138, {{0x14, 4, 6}, {0x30, 4, 6}, {0x4C, 4, 6}}};                          // J3DIndInitData
        const Layout cVtxAttrFmt = {0x10, {{0x00, 4, 3}}};                  // GXVtxAttrFmtList
        const Layout cVtxDesc = {0x08, {{0x00, 4, 2}}};                     // GXVtxDescList
        const Layout cShapeInit = {0x28, {{0x02, 2, 4}, {0x0C, 4, 7}}};     // J3DShapeInitData
        const Layout cShapeMtxInit = {0x08, {{0x00, 2, 2}, {0x04, 4, 1}}};  // J3DShapeMtxInitData
        const Layout cShapeDrawInit = {0x08, {{0x00, 4, 2}}};               // J3DShapeDrawInitData
        const Layout cJointInit = {0x40, {{0x00, 2, 1}, {0x04, 4, 3}, {0x10, 2, 4}, {0x18, 4, 3}, {0x24, 4, 7}}};  // J3DJointInitData
        const Layout cHierarchy = {0x04, {{0x00, 2, 2}}};                   // J3DModelHierarchy
        const Layout cInvMtx = {0x30, {{0x00, 4, 12}}};                     // Mtx
        const Layout cResTIMG = {0x20, {{0x02, 2, 2}, {0x0A, 2, 1}, {0x0C, 4, 1}, {0x1A, 2, 1}, {0x1C, 4, 1}}};
        const Layout cDisplayListInit = {0x08, {{0x00, 4, 2}}};             // J3DDisplayListInit
        const Layout cPatchingInfo = {0x10, {{0x00, 2, 6}}};                // J3DPatchingInfo
        const Layout cCurrentMtxInfo = {0x08, {{0x00, 4, 2}}};              // J3DCurrentMtxInfo

        enum class Extent {
            ToNextTable,  // elements fill the space up to the next table (tables of shared entries)
            Count,        // exactly `count` elements
            NameTable,    // ResNTAB: count from its own header
            Skip,         // big-endian byte stream (display lists): left untouched
        };

        struct Table {
            std::uint32_t mField;  // offset of the u32 offset field in the block header
            const Layout* mLayout;
            Extent mExtent;
        };

        struct BlockSpec {
            const char* mName;
            bool mHasCount;  // u16 count at 0x08
            std::vector< std::uint32_t > mScalarFields;  // extra u32 header scalars (not offsets)
            std::vector< Table > mTables;
        };

        class Converter {
        public:
            Converter(std::uint8_t* image, std::uint32_t size) : mImage(image), mSize(size) {
            }

            const char* run();

        private:
            const char* convertBlock(std::uint32_t blockStart, std::uint32_t blockSize);
            const char* convertVertexBlock(std::uint32_t block, std::uint32_t blockSize);
            const char* convertTables(std::uint32_t block, std::uint32_t blockSize, const BlockSpec& spec, std::uint32_t count);
            const char* convertElements(std::uint32_t start, std::uint32_t end, const Layout& layout, std::uint32_t count);
            const char* convertNameTable(std::uint32_t start, std::uint32_t end);

            std::uint8_t* mImage;
            std::uint32_t mSize;
        };

        const BlockSpec* findSpec(std::uint32_t type) {
            static const std::vector< BlockSpec > cSpecs = {
                {"INF1", true, {0x0C, 0x10}, {{0x14, &cHierarchy, Extent::ToNextTable}}},
                {"EVP1", true, {}, {{0x0C, &cU8, Extent::ToNextTable}, {0x10, &cU16, Extent::ToNextTable}, {0x14, &cU32, Extent::ToNextTable},
                                    {0x18, &cInvMtx, Extent::ToNextTable}}},
                {"DRW1", true, {}, {{0x0C, &cU8, Extent::ToNextTable}, {0x10, &cU16, Extent::ToNextTable}}},
                {"JNT1", true, {}, {{0x0C, &cJointInit, Extent::ToNextTable}, {0x10, &cU16, Extent::ToNextTable}, {0x14, nullptr, Extent::NameTable}}},
                {"MAT3", true, {}, {{0x0C, &cMaterialInit, Extent::ToNextTable}, {0x10, &cU16, Extent::ToNextTable},
                                    {0x14, nullptr, Extent::NameTable}, {0x18, &cIndInit, Extent::ToNextTable},
                                    {0x1C, &cU32, Extent::ToNextTable}, {0x20, &cColor, Extent::ToNextTable},
                                    {0x24, &cU8, Extent::ToNextTable}, {0x28, &cBytes8, Extent::ToNextTable},
                                    {0x2C, &cColor, Extent::ToNextTable}, {0x30, &cLight, Extent::ToNextTable},
                                    {0x34, &cU8, Extent::ToNextTable}, {0x38, &cBytes4, Extent::ToNextTable},
                                    {0x3C, &cBytes4, Extent::ToNextTable}, {0x40, &cTexMtx, Extent::ToNextTable},
                                    {0x44, &cTexMtx, Extent::ToNextTable}, {0x48, &cU16, Extent::ToNextTable},
                                    {0x4C, &cBytes4, Extent::ToNextTable}, {0x50, &cColorS10, Extent::ToNextTable},
                                    {0x54, &cColor, Extent::ToNextTable}, {0x58, &cU8, Extent::ToNextTable},
                                    {0x5C, &cTevStage, Extent::ToNextTable}, {0x60, &cBytes4, Extent::ToNextTable},
                                    {0x64, &cBytes4, Extent::ToNextTable}, {0x68, &cFog, Extent::ToNextTable},
                                    {0x6C, &cBytes8, Extent::ToNextTable}, {0x70, &cBytes4, Extent::ToNextTable},
                                    {0x74, &cBytes4, Extent::ToNextTable}, {0x78, &cU8, Extent::ToNextTable},
                                    {0x7C, &cU8, Extent::ToNextTable}, {0x80, &cNbtScale, Extent::ToNextTable}}},
                {"SHP1", true, {}, {{0x0C, &cShapeInit, Extent::ToNextTable}, {0x10, &cU16, Extent::ToNextTable},
                                    {0x14, nullptr, Extent::NameTable}, {0x18, &cVtxDesc, Extent::ToNextTable},
                                    {0x1C, &cU16, Extent::ToNextTable}, {0x20, nullptr, Extent::Skip},
                                    {0x24, &cShapeMtxInit, Extent::ToNextTable}, {0x28, &cShapeDrawInit, Extent::ToNextTable}}},
                {"TEX1", true, {}, {{0x0C, &cResTIMG, Extent::Count}, {0x10, nullptr, Extent::NameTable}}},
                {"MDL3", true, {}, {{0x0C, &cDisplayListInit, Extent::Count}, {0x10, &cPatchingInfo, Extent::Count},
                                    {0x14, &cCurrentMtxInfo, Extent::Count}, {0x18, &cU8, Extent::Count},
                                    {0x1C, &cU16, Extent::Count}, {0x20, nullptr, Extent::NameTable}}},
            };

            for (const BlockSpec& spec : cSpecs) {
                if (tag(spec.mName) == type) {
                    return &spec;
                }
            }
            return nullptr;
        }

        const char* Converter::run() {
            if (mSize < cHeaderSize || readU32BE(mImage) != cMagicJ3D2 || readU32BE(mImage + 8) != mSize) {
                return "not a big-endian J3D2 file of the given size";
            }

            const std::uint32_t blockNum = readU32BE(mImage + 12);
            for (std::uint32_t field = 0; field < 16; field += 4) {
                swap32(mImage + field);
            }

            std::uint32_t block = cHeaderSize;
            for (std::uint32_t i = 0; i < blockNum; i++) {
                if (block > mSize - 8) {
                    return "block header outside the file";
                }
                const std::uint32_t blockSize = readU32BE(mImage + block + 4);
                if (blockSize < 8 || blockSize > mSize - block) {
                    return "block size outside the file";
                }
                if (const char* error = convertBlock(block, blockSize)) {
                    return error;
                }
                swap32(mImage + block);      // block type
                swap32(mImage + block + 4);  // block size
                block += blockSize;
            }
            return nullptr;
        }

        const char* Converter::convertBlock(std::uint32_t block, std::uint32_t blockSize) {
            const std::uint32_t type = readU32BE(mImage + block);
            if (type == tag("VTX1")) {
                return convertVertexBlock(block, blockSize);
            }

            const BlockSpec* spec = findSpec(type);
            if (spec == nullptr) {
                return "unsupported J3D block type";
            }

            const std::uint32_t headerEnd = std::max< std::uint32_t >(spec->mTables.empty() ? 0x0C : spec->mTables.back().mField + 4, 0x0C);
            if (blockSize < headerEnd) {
                return "block smaller than its header";
            }

            std::uint32_t count = 0;
            if (spec->mHasCount) {
                count = readU16BE(mImage + block + 8);
                swap16(mImage + block + 8);
            }
            for (std::uint32_t field : spec->mScalarFields) {
                swap32(mImage + block + field);
            }
            return convertTables(block, blockSize, *spec, count);
        }

        const char* Converter::convertTables(std::uint32_t block, std::uint32_t blockSize, const BlockSpec& spec, std::uint32_t count) {
            // Offsets are relative to the block; 0 means the table is absent.
            std::vector< std::uint32_t > offsets;
            for (const Table& table : spec.mTables) {
                const std::uint32_t offset = readU32BE(mImage + block + table.mField);
                if (offset != 0) {
                    if (offset >= blockSize) {
                        return "table offset outside its block";
                    }
                    offsets.push_back(offset);
                }
            }
            std::sort(offsets.begin(), offsets.end());

            for (const Table& table : spec.mTables) {
                const std::uint32_t offset = readU32BE(mImage + block + table.mField);
                swap32(mImage + block + table.mField);
                if (offset == 0) {
                    continue;
                }

                const auto next = std::upper_bound(offsets.begin(), offsets.end(), offset);
                const std::uint32_t end = block + (next != offsets.end() ? *next : blockSize);
                const std::uint32_t start = block + offset;
                const char* error = nullptr;
                switch (table.mExtent) {
                case Extent::ToNextTable:
                    error = convertElements(start, end, *table.mLayout, (end - start) / table.mLayout->mElementSize);
                    break;
                case Extent::Count:
                    error = convertElements(start, block + blockSize, *table.mLayout, count);
                    break;
                case Extent::NameTable:
                    error = convertNameTable(start, end);
                    break;
                case Extent::Skip:
                    break;
                }
                if (error != nullptr) {
                    return error;
                }
            }
            return nullptr;
        }

        const char* Converter::convertElements(std::uint32_t start, std::uint32_t end, const Layout& layout, std::uint32_t count) {
            if (static_cast< std::uint64_t >(count) * layout.mElementSize > end - start) {
                return "table extends past its block";
            }
            for (std::uint32_t i = 0; i < count; i++) {
                std::uint8_t* element = mImage + start + i * layout.mElementSize;
                for (const Scalars& run : layout.mScalars) {
                    for (std::uint32_t k = 0; k < run.mCount; k++) {
                        if (run.mSize == 2) {
                            swap16(element + run.mOffset + k * 2);
                        } else {
                            swap32(element + run.mOffset + k * 4);
                        }
                    }
                }
            }
            return nullptr;
        }

        const char* Converter::convertNameTable(std::uint32_t start, std::uint32_t end) {
            if (end - start < 4) {
                return "name table header outside its block";
            }
            const std::uint32_t count = readU16BE(mImage + start);
            if (4 + count * 4 > end - start) {
                return "name table entries outside its block";
            }
            for (std::uint32_t i = 0; i < count; i++) {
                const std::uint32_t name = start + readU16BE(mImage + start + 4 + i * 4 + 2);
                if (name >= end || std::memchr(mImage + name, 0, end - name) == nullptr) {
                    return "name table string outside its block";
                }
            }
            for (std::uint32_t i = 0; i < 2 + count * 2; i++) {
                swap16(mImage + start + i * 2);
            }
            return nullptr;
        }

        // GXCompType values used by position/normal/texcoord arrays, and GXCompType values
        // used by color arrays (GX_RGB565 .. GX_RGBA8).
        std::uint32_t componentSize(std::uint32_t attr, std::uint32_t type) {
            const bool isColor = attr == 11 || attr == 12;  // GX_VA_CLR0, GX_VA_CLR1
            if (isColor) {
                switch (type) {
                case 0:  // GX_RGB565
                case 3:  // GX_RGBA4
                    return 2;
                case 1:  // GX_RGB8
                case 2:  // GX_RGBX8
                case 5:  // GX_RGBA8
                    return 1;
                default:  // GX_RGBA6 is packed 24-bit; not used by any known J3D file
                    return 0;
                }
            }
            switch (type) {
            case 0:  // GX_U8
            case 1:  // GX_S8
                return 1;
            case 2:  // GX_U16
            case 3:  // GX_S16
                return 2;
            case 4:  // GX_F32
                return 4;
            default:
                return 0;
            }
        }

        // VTX1 header: 0x08 format list offset, then 13 array offsets at 0x0C in this order.
        const std::uint32_t cVtxFmtListField = 0x08;
        const std::uint32_t cVtxArrayField = 0x0C;
        const int cVtxArrayNum = 13;
        const std::uint32_t cVtxSlotAttr[cVtxArrayNum] = {
            9,   // GX_VA_POS
            10,  // GX_VA_NRM
            25,  // GX_VA_NBT
            11,  // GX_VA_CLR0
            12,  // GX_VA_CLR1
            13, 14, 15, 16, 17, 18, 19, 20,  // GX_VA_TEX0 .. GX_VA_TEX7
        };

        const char* Converter::convertVertexBlock(std::uint32_t block, std::uint32_t blockSize) {
            if (blockSize < cVtxArrayField + cVtxArrayNum * 4) {
                return "VTX1 smaller than its header";
            }

            std::vector< std::uint32_t > offsets;
            for (std::uint32_t field = cVtxFmtListField; field < cVtxArrayField + cVtxArrayNum * 4; field += 4) {
                const std::uint32_t offset = readU32BE(mImage + block + field);
                if (offset != 0) {
                    if (offset >= blockSize) {
                        return "VTX1 offset outside its block";
                    }
                    offsets.push_back(offset);
                }
            }
            std::sort(offsets.begin(), offsets.end());
            auto tableEnd = [&](std::uint32_t offset) {
                const auto next = std::upper_bound(offsets.begin(), offsets.end(), offset);
                return block + (next != offsets.end() ? *next : blockSize);
            };

            const std::uint32_t fmtOffset = readU32BE(mImage + block + cVtxFmtListField);
            if (fmtOffset == 0) {
                return "VTX1 without an attribute format list";
            }

            // Component size per array slot, from the (still big-endian) format list.
            std::uint32_t slotComponent[cVtxArrayNum] = {};
            const std::uint32_t fmtEnd = tableEnd(fmtOffset);
            std::uint32_t fmt = block + fmtOffset;
            for (;; fmt += 0x10) {
                if (fmt + 0x10 > fmtEnd) {
                    return "VTX1 format list is not terminated";
                }
                const std::uint32_t attr = readU32BE(mImage + fmt);
                if (attr == 0xFF) {  // GX_VA_NULL
                    break;
                }
                for (int slot = 0; slot < cVtxArrayNum; slot++) {
                    if (cVtxSlotAttr[slot] == attr) {
                        slotComponent[slot] = componentSize(attr, readU32BE(mImage + fmt + 8));
                    }
                }
            }

            for (int slot = 0; slot < cVtxArrayNum; slot++) {
                const std::uint32_t field = cVtxArrayField + slot * 4;
                const std::uint32_t offset = readU32BE(mImage + block + field);
                if (offset == 0) {
                    continue;
                }
                const std::uint32_t size = slotComponent[slot];
                if (size == 0) {
                    return "VTX1 array without a supported component format";
                }
                const std::uint32_t start = block + offset;
                const std::uint32_t count = (tableEnd(offset) - start) / size;
                for (std::uint32_t i = 0; i < count; i++) {
                    if (size == 2) {
                        swap16(mImage + start + i * 2);
                    } else if (size == 4) {
                        swap32(mImage + start + i * 4);
                    }
                }
            }

            for (std::uint32_t entry = block + fmtOffset; entry <= fmt; entry += 0x10) {
                swap32(mImage + entry);
                swap32(mImage + entry + 4);
                swap32(mImage + entry + 8);
            }
            for (std::uint32_t field = cVtxFmtListField; field < cVtxArrayField + cVtxArrayNum * 4; field += 4) {
                swap32(mImage + block + field);
            }
            return nullptr;
        }
    }  // namespace

    ModelImageKind classifyModelImage(const void* data) {
        const std::uint8_t* bytes = static_cast< const std::uint8_t* >(data);
        if (hostU32(bytes) == cMagicJ3D2) {
            return ModelImageKind::Host;
        }
        if (readU32BE(bytes) == cMagicJ3D2) {
            return ModelImageKind::BigEndian;
        }
        return ModelImageKind::NotJ3D;
    }

    std::uint32_t modelFileSize(const void* data) {
        if (classifyModelImage(data) != ModelImageKind::BigEndian) {
            return 0;
        }
        return readU32BE(static_cast< const std::uint8_t* >(data) + 8);
    }

    const char* makeHostModelImage(const void* src, std::uint32_t fileSize, void* dst) {
        // The converter's tables and temporaries are host data: keep them off the game's
        // current JKR heap, which callers may destroy while this code's statics live on.
        HostAllocationScope hostAllocations;
        static_assert(sizeof(std::uint32_t) == 4, "");
        if (hostU32(reinterpret_cast< const std::uint8_t* >("\x01\x02\x03\x04")) == 0x01020304) {
            return "host images are only defined for little-endian hosts";
        }
        std::memcpy(dst, src, fileSize);
        Converter converter(static_cast< std::uint8_t* >(dst), fileSize);
        return converter.run();
    }

    std::uint32_t hostVertexArrayBytes(const void* hostVtx1Block, int slot) {
        const std::uint8_t* block = static_cast< const std::uint8_t* >(hostVtx1Block);
        if (slot < 0 || slot >= cVtxArrayNum) {
            return 0;
        }
        const std::uint32_t offset = hostU32(block + cVtxArrayField + slot * 4);
        if (offset == 0) {
            return 0;
        }
        std::uint32_t end = hostU32(block + 4);
        for (std::uint32_t field = cVtxFmtListField; field < cVtxArrayField + cVtxArrayNum * 4; field += 4) {
            const std::uint32_t other = hostU32(block + field);
            if (other > offset && other < end) {
                end = other;
            }
        }
        return end - offset;
    }
}  // namespace J3D
}  // namespace PetariNative

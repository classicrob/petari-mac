// Conversion of big-endian J3D1 animation files into host-layout images. See
// native/include/petari/j3d_animation.hpp. Block layouts follow the J3DAnm*Data structs in
// JSystem/J3DGraphAnimator/J3DAnimation.hpp and the readers in J3DAnmLoader.cpp.
#include <petari/j3d_animation.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

namespace PetariNative {
namespace J3D {
    namespace {
        const std::uint32_t cMagicJ3D1 = 0x4A334431;  // 'J3D1'
        const std::uint32_t cHeaderSize = 0x20;

        std::uint32_t tag(const char* name) {
            return readU32BE(name);
        }

        std::uint32_t hostU32(const std::uint8_t* p) {
            std::uint32_t value;
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

        struct Scalars {
            std::uint32_t mOffset;
            std::uint32_t mSize;
            std::uint32_t mCount;
        };

        struct Layout {
            std::uint32_t mElementSize;
            std::vector< Scalars > mScalars;
        };

        const Layout cU8 = {1, {}};
        const Layout cU16 = {2, {{0, 2, 1}}};
        const Layout cU32 = {4, {{0, 4, 1}}};
        const Layout cKeyTable = {0x06, {{0, 2, 3}}};             // J3DAnmKeyTableBase
        const Layout cColorKeyTable = {0x18, {{0, 2, 12}}};       // J3DAnmColorKeyTable
        const Layout cRegKeyTable = {0x1C, {{0, 2, 12}}};         // J3DAnmCRegKeyTable / KRegKeyTable
        const Layout cTransformKeyTable = {0x12, {{0, 2, 9}}};    // J3DAnmTransformKeyTable
        const Layout cTransformFullTable = {0x0C, {{0, 2, 6}}};   // J3DAnmTransformFullTable
        const Layout cColorFullTable = {0x10, {{0, 2, 8}}};       // J3DAnmColorFullTable
        const Layout cPairTable = {0x04, {{0, 2, 2}}};            // visibility / cluster full tables
        const Layout cTexPatternTable = {0x08, {{0, 2, 2}, {6, 2, 1}}};  // J3DAnmTexPatternFullTable

        enum class Extent {
            ToNextTable,
            NameTable,
        };

        struct Table {
            std::uint32_t mField;
            const Layout* mLayout;
            Extent mExtent;
        };

        struct BlockSpec {
            const char* mName;
            std::uint32_t mHeaderSize;
            std::vector< Scalars > mHeaderScalars;  // counts and sizes (not offsets)
            std::vector< Table > mTables;
        };

        const BlockSpec* findSpec(std::uint32_t type) {
            static const std::vector< BlockSpec > cSpecs = {
                {"ANK1", 0x24, {{0x0A, 2, 5}},
                 {{0x14, &cTransformKeyTable, Extent::ToNextTable}, {0x18, &cU32, Extent::ToNextTable},
                  {0x1C, &cU16, Extent::ToNextTable}, {0x20, &cU32, Extent::ToNextTable}}},
                {"ANF1", 0x24, {{0x0A, 2, 5}},
                 {{0x14, &cTransformFullTable, Extent::ToNextTable}, {0x18, &cU32, Extent::ToNextTable},
                  {0x1C, &cU16, Extent::ToNextTable}, {0x20, &cU32, Extent::ToNextTable}}},
                {"PAK1", 0x34, {{0x0C, 2, 6}},
                 {{0x18, &cColorKeyTable, Extent::ToNextTable}, {0x1C, &cU16, Extent::ToNextTable},
                  {0x20, nullptr, Extent::NameTable}, {0x24, &cU16, Extent::ToNextTable},
                  {0x28, &cU16, Extent::ToNextTable}, {0x2C, &cU16, Extent::ToNextTable},
                  {0x30, &cU16, Extent::ToNextTable}}},
                {"PAF1", 0x34, {{0x0C, 2, 6}},
                 {{0x18, &cColorFullTable, Extent::ToNextTable}, {0x1C, &cU16, Extent::ToNextTable},
                  {0x20, nullptr, Extent::NameTable}, {0x24, &cU8, Extent::ToNextTable},
                  {0x28, &cU8, Extent::ToNextTable}, {0x2C, &cU8, Extent::ToNextTable},
                  {0x30, &cU8, Extent::ToNextTable}}},
                {"CLK1", 0x18, {{0x0A, 2, 1}, {0x0C, 4, 1}},
                 {{0x10, &cKeyTable, Extent::ToNextTable}, {0x14, &cU32, Extent::ToNextTable}}},
                {"CLF1", 0x18, {{0x0A, 2, 1}, {0x0C, 4, 1}},
                 {{0x10, &cPairTable, Extent::ToNextTable}, {0x14, &cU32, Extent::ToNextTable}}},
                {"TPT1", 0x20, {{0x0A, 2, 3}},
                 {{0x10, &cTexPatternTable, Extent::ToNextTable}, {0x14, &cU16, Extent::ToNextTable},
                  {0x18, &cU16, Extent::ToNextTable}, {0x1C, nullptr, Extent::NameTable}}},
                {"TRK1", 0x58, {{0x0A, 2, 11}},
                 {{0x20, &cRegKeyTable, Extent::ToNextTable}, {0x24, &cRegKeyTable, Extent::ToNextTable},
                  {0x28, &cU16, Extent::ToNextTable}, {0x2C, &cU16, Extent::ToNextTable},
                  {0x30, nullptr, Extent::NameTable}, {0x34, nullptr, Extent::NameTable},
                  {0x38, &cU16, Extent::ToNextTable}, {0x3C, &cU16, Extent::ToNextTable},
                  {0x40, &cU16, Extent::ToNextTable}, {0x44, &cU16, Extent::ToNextTable},
                  {0x48, &cU16, Extent::ToNextTable}, {0x4C, &cU16, Extent::ToNextTable},
                  {0x50, &cU16, Extent::ToNextTable}, {0x54, &cU16, Extent::ToNextTable}}},
                {"TTK1", 0x60, {{0x0A, 2, 5}, {0x34, 2, 4}, {0x5C, 4, 1}},
                 {{0x14, &cTransformKeyTable, Extent::ToNextTable}, {0x18, &cU16, Extent::ToNextTable},
                  {0x1C, nullptr, Extent::NameTable}, {0x20, &cU8, Extent::ToNextTable},
                  {0x24, &cU32, Extent::ToNextTable}, {0x28, &cU32, Extent::ToNextTable},
                  {0x2C, &cU16, Extent::ToNextTable}, {0x30, &cU32, Extent::ToNextTable},
                  {0x3C, &cTransformKeyTable, Extent::ToNextTable}, {0x40, &cU16, Extent::ToNextTable},
                  {0x44, nullptr, Extent::NameTable}, {0x48, &cU8, Extent::ToNextTable},
                  {0x4C, &cU32, Extent::ToNextTable}, {0x50, &cU32, Extent::ToNextTable},
                  {0x54, &cU16, Extent::ToNextTable}, {0x58, &cU32, Extent::ToNextTable}}},
                {"VAF1", 0x18, {{0x0A, 2, 3}},
                 {{0x10, &cPairTable, Extent::ToNextTable}, {0x14, &cU8, Extent::ToNextTable}}},
            };

            for (const BlockSpec& spec : cSpecs) {
                if (tag(spec.mName) == type) {
                    return &spec;
                }
            }
            return nullptr;
        }

        class Converter {
        public:
            Converter(std::uint8_t* image, std::uint32_t size) : mImage(image), mSize(size) {
            }

            const char* run() {
                if (mSize < cHeaderSize + 8 || readU32BE(mImage) != cMagicJ3D1 || readU32BE(mImage + 8) != mSize) {
                    return "not a big-endian J3D1 file of the given size";
                }
                const std::uint32_t blockNum = readU32BE(mImage + 0x0C);
                swap32(mImage + 0x00);
                swap32(mImage + 0x04);
                swap32(mImage + 0x08);
                swap32(mImage + 0x0C);
                swap32(mImage + 0x1C);  // sound animation offset (bck only)

                std::uint32_t block = cHeaderSize;
                for (std::uint32_t i = 0; i < blockNum; i++) {
                    if (block > mSize - 8) {
                        return "block header outside the file";
                    }
                    std::uint32_t blockSize = readU32BE(mImage + block + 4);
                    if (blockSize < 8) {
                        return "block size too small";
                    }
                    if (blockSize > mSize - block) {
                        // Some disc files' final block size runs a few bytes past the file
                        // (the Wii loader never checks); its tables must still fit the file.
                        if (i + 1 != blockNum || blockSize - (mSize - block) > 0x20) {
                            return "block size outside the file";
                        }
                        blockSize = mSize - block;
                    }
                    if (const char* error = convertBlock(block, blockSize)) {
                        return error;
                    }
                    swap32(mImage + block);
                    swap32(mImage + block + 4);
                    block += blockSize;
                }
                return nullptr;
            }

        private:
            const char* convertBlock(std::uint32_t block, std::uint32_t blockSize) {
                const BlockSpec* spec = findSpec(readU32BE(mImage + block));
                if (spec == nullptr) {
                    return "unsupported animation block type";
                }
                if (blockSize < spec->mHeaderSize) {
                    return "block smaller than its header";
                }
                for (const Scalars& run : spec->mHeaderScalars) {
                    for (std::uint32_t k = 0; k < run.mCount; k++) {
                        if (run.mSize == 2) {
                            swap16(mImage + block + run.mOffset + k * 2);
                        } else {
                            swap32(mImage + block + run.mOffset + k * 4);
                        }
                    }
                }

                std::vector< std::uint32_t > offsets;
                for (const Table& table : spec->mTables) {
                    const std::uint32_t offset = readU32BE(mImage + block + table.mField);
                    if (offset != 0) {
                        if (offset >= blockSize || offset < spec->mHeaderSize) {
                            return "table offset outside its block";
                        }
                        offsets.push_back(offset);
                    }
                }
                std::sort(offsets.begin(), offsets.end());

                for (const Table& table : spec->mTables) {
                    const std::uint32_t offset = readU32BE(mImage + block + table.mField);
                    swap32(mImage + block + table.mField);
                    if (offset == 0) {
                        continue;
                    }
                    // Tables may be shared by several offset fields (e.g. identical value
                    // arrays); convert each distinct table once.
                    const auto first = std::lower_bound(offsets.begin(), offsets.end(), offset);
                    if (std::count(offsets.begin(), offsets.end(), offset) > 1 && isConverted(block + offset)) {
                        continue;
                    }
                    const auto next = std::upper_bound(first, offsets.end(), offset);
                    const std::uint32_t start = block + offset;
                    const std::uint32_t end = block + (next != offsets.end() ? *next : blockSize);
                    const char* error = table.mExtent == Extent::NameTable ? convertNameTable(start, end)
                                                                            : convertElements(start, end, *table.mLayout);
                    if (error != nullptr) {
                        return error;
                    }
                    mConverted.push_back(start);
                }
                return nullptr;
            }

            bool isConverted(std::uint32_t start) const {
                return std::find(mConverted.begin(), mConverted.end(), start) != mConverted.end();
            }

            const char* convertElements(std::uint32_t start, std::uint32_t end, const Layout& layout) {
                const std::uint32_t count = (end - start) / layout.mElementSize;
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

            const char* convertNameTable(std::uint32_t start, std::uint32_t end) {
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

            std::uint8_t* mImage;
            std::uint32_t mSize;
            std::vector< std::uint32_t > mConverted;
        };
    }  // namespace

    AnimImageKind classifyAnimImage(const void* data) {
        const std::uint8_t* bytes = static_cast< const std::uint8_t* >(data);
        if (hostU32(bytes) == cMagicJ3D1) {
            return AnimImageKind::Host;
        }
        if (readU32BE(bytes) == cMagicJ3D1) {
            return AnimImageKind::BigEndian;
        }
        return AnimImageKind::NotJ3D;
    }

    std::uint32_t animFileSize(const void* data) {
        if (classifyAnimImage(data) != AnimImageKind::BigEndian) {
            return 0;
        }
        return readU32BE(static_cast< const std::uint8_t* >(data) + 8);
    }

    const char* makeHostAnimImage(const void* src, std::uint32_t fileSize, void* dst) {
        // The converter's tables and temporaries are host data: keep them off the game's
        // current JKR heap, which callers may destroy while this code's statics live on.
        HostAllocationScope hostAllocations;
        std::memcpy(dst, src, fileSize);
        Converter converter(static_cast< std::uint8_t* >(dst), fileSize);
        return converter.run();
    }
}  // namespace J3D
}  // namespace PetariNative

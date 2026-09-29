// In-place host conversion of KCL collision resources. See petari/kcl_collision.hpp.
#include <petari/kcl_collision.hpp>
#include <petari/endian.hpp>
#include <petari/host_allocation.hpp>
#include <cstring>
#include <vector>

namespace PetariNative {
namespace KCL {
    namespace {
        const std::uint32_t cPrismSize = 0x10;

        std::uint32_t hostU32(const std::uint8_t* p) {
            std::uint32_t value;
            std::memcpy(&value, p, sizeof(value));
            return value;
        }

        void storeU32(std::uint8_t* p, std::uint32_t value) {
            std::memcpy(p, &value, sizeof(value));
        }

        void storeU16(std::uint8_t* p, std::uint16_t value) {
            std::memcpy(p, &value, sizeof(value));
        }

        // Collects every octree word and prism list entry first, so the resource is only
        // modified once the whole structure has been validated.
        class OctreeScan {
        public:
            OctreeScan(const std::uint8_t* data, std::uint32_t size, std::uint32_t octree, std::uint32_t prismCount)
                : mData(data), mSize(size), mOctree(octree), mPrismCount(prismCount), mMarks(size, 0) {
            }

            const char* node(std::uint32_t base, std::uint32_t wordOffset, int depth) {
                const std::uint32_t word = base + wordOffset;
                if (word < mOctree || word > mSize - 4) {
                    return "octree word outside the resource";
                }
                if (mMarks[word] == 1) {
                    return nullptr;  // already visited
                }
                if (mMarks[word] != 0 || depth > 32) {
                    return "octree words overlap other data";
                }
                mMarks[word] = 1;
                mWords.push_back(word);

                const std::uint32_t value = readU32BE(mData + word);
                if ((value & cInitializedFlag) != 0) {
                    return list(base + (value & ~cInitializedFlag));
                }
                const std::uint32_t child = base + value;
                for (std::uint32_t i = 0; i < 8; i++) {
                    if (const char* error = node(child, i * 4, depth + 1)) {
                        return error;
                    }
                }
                return nullptr;
            }

            // The list pointer addresses the u16 before the first entry; entries follow until 0.
            const char* list(std::uint32_t start) {
                for (std::uint32_t entry = start + 2;; entry += 2) {
                    if (entry < mOctree || entry > mSize - 2) {
                        return "prism list outside the resource";
                    }
                    if (mMarks[entry] == 2) {
                        return nullptr;  // shared suffix already collected
                    }
                    if (mMarks[entry] != 0) {
                        return "prism list overlaps octree words";
                    }
                    mMarks[entry] = 2;
                    const std::uint16_t prism = readU16BE(mData + entry);
                    if (prism == 0) {
                        mEntries.push_back(entry);
                        return nullptr;
                    }
                    if (prism >= mPrismCount) {
                        return "prism list references a missing prism";
                    }
                    mEntries.push_back(entry);
                }
            }

            std::vector< std::uint32_t > mWords;
            std::vector< std::uint32_t > mEntries;

        private:
            const std::uint8_t* mData;
            std::uint32_t mSize;
            std::uint32_t mOctree;
            std::uint32_t mPrismCount;
            std::vector< std::uint8_t > mMarks;
        };
    }  // namespace

    bool isHostResource(const void* data) {
        return (hostU32(static_cast< const std::uint8_t* >(data)) & cInitializedFlag) != 0;
    }

    const char* convertInPlace(void* resource, std::uint32_t size) {
        // The conversion's tables and temporaries are host data: keep them off the game's
        // current JKR heap, which callers may destroy while this code's statics live on.
        HostAllocationScope hostAllocations;
        std::uint8_t* data = static_cast< std::uint8_t* >(resource);
        if (size < cHeaderSize) {
            return "resource smaller than the KCL header";
        }

        const std::uint32_t pos = readU32BE(data + 0x00);
        const std::uint32_t nrm = readU32BE(data + 0x04);
        const std::uint32_t prism = readU32BE(data + 0x08);
        const std::uint32_t octree = readU32BE(data + 0x0C);
        if ((pos & cInitializedFlag) != 0) {
            return "resource is already initialized";
        }
        // Prisms are indexed from 1: the prism offset addresses an unused prism 0 that overlaps
        // the last 16 bytes of the normal section.
        if (!(cHeaderSize <= pos && pos <= nrm && nrm <= prism + cPrismSize && prism + cPrismSize <= octree && octree < size)) {
            return "section offsets out of order";
        }
        if ((nrm - pos) % 12 != 0 || (prism + cPrismSize - nrm) % 12 != 0 || (octree - prism) % cPrismSize != 0) {
            return "section sizes are not whole elements";
        }

        const std::uint32_t posCount = (nrm - pos) / 12;
        const std::uint32_t nrmCount = (prism + cPrismSize - nrm) / 12;
        const std::uint32_t prismCount = (octree - prism) / cPrismSize;
        for (std::uint32_t i = 1; i < prismCount; i++) {
            const std::uint8_t* p = data + prism + i * cPrismSize;
            if (readU16BE(p + 4) >= posCount) {
                return "prism references a missing position";
            }
            for (int k = 0; k < 4; k++) {
                if (readU16BE(p + 6 + k * 2) >= nrmCount) {
                    return "prism references a missing normal";
                }
            }
        }

        // Top-level cells, as indexed by KCollisionServer::searchBlock.
        const std::int32_t xMask = static_cast< std::int32_t >(readU32BE(data + 0x20));
        const std::int32_t yMask = static_cast< std::int32_t >(readU32BE(data + 0x24));
        const std::int32_t zMask = static_cast< std::int32_t >(readU32BE(data + 0x28));
        const std::int32_t widthShift = static_cast< std::int32_t >(readU32BE(data + 0x2C));
        const std::int32_t xShift = static_cast< std::int32_t >(readU32BE(data + 0x30));
        const std::int32_t xyShift = static_cast< std::int32_t >(readU32BE(data + 0x34));
        if (widthShift < 0 || widthShift > 31 || xShift < -1 || xShift > 31 || xyShift < -1 || xyShift > 31) {
            return "octree shifts out of range";
        }

        std::uint64_t cells = 1;
        if (!(xShift == -1 && xyShift == -1)) {
            const std::uint64_t xCells = (static_cast< std::uint32_t >(~xMask) >> widthShift) + 1;
            const std::uint64_t yCells = (static_cast< std::uint32_t >(~yMask) >> widthShift) + 1;
            const std::uint64_t zCells = (static_cast< std::uint32_t >(~zMask) >> widthShift) + 1;
            cells = xCells * yCells * zCells;
            if (cells * 4 > size - octree) {
                return "octree top level outside the resource";
            }
        }

        OctreeScan scan(data, size, octree, prismCount);
        for (std::uint32_t cell = 0; cell < cells; cell++) {
            if (const char* error = scan.node(octree, cell * 4, 0)) {
                return error;
            }
        }

        // Validated: convert. The header's first word keeps its offset with the marker bit.
        for (std::uint32_t field = 0; field < cHeaderSize; field += 4) {
            storeU32(data + field, readU32BE(data + field));
        }
        storeU32(data, pos | cInitializedFlag);
        for (std::uint32_t offset = pos; offset < prism + cPrismSize; offset += 4) {
            storeU32(data + offset, readU32BE(data + offset));
        }
        for (std::uint32_t i = 1; i < prismCount; i++) {
            std::uint8_t* p = data + prism + i * cPrismSize;
            storeU32(p, readU32BE(p));
            for (int k = 0; k < 6; k++) {
                storeU16(p + 4 + k * 2, readU16BE(p + 4 + k * 2));
            }
        }
        for (std::uint32_t word : scan.mWords) {
            storeU32(data + word, readU32BE(data + word));
        }
        for (std::uint32_t entry : scan.mEntries) {
            storeU16(data + entry, readU16BE(data + entry));
        }
        return nullptr;
    }

    std::uint32_t positionOffset(const void* hostData) {
        return hostU32(static_cast< const std::uint8_t* >(hostData)) & ~cInitializedFlag;
    }

    std::uint32_t normalOffset(const void* hostData) {
        return hostU32(static_cast< const std::uint8_t* >(hostData) + 0x04);
    }

    std::uint32_t prismOffset(const void* hostData) {
        return hostU32(static_cast< const std::uint8_t* >(hostData) + 0x08);
    }

    std::uint32_t octreeOffset(const void* hostData) {
        return hostU32(static_cast< const std::uint8_t* >(hostData) + 0x0C);
    }
}  // namespace KCL
}  // namespace PetariNative

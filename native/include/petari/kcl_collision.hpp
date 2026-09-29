#pragma once
// In-place host conversion of KCL collision resources (.kcl).
//
// The Wii game relocates a KCL resource in place the first time a collision server uses
// it: the four section offsets in the header become pointers, and a negative first word
// marks the resource as initialized for later users of the same (shared) resource.
// Natively the resource is instead converted in place to host byte order, and the first
// header word gets its high bit set as the same "initialized" marker. Section pointers
// live in a separate host descriptor (KCLFile in Game/Map/KCollision.hpp).
//
// Header (0x38 bytes): u32 position, normal, prism, octree offsets; f32 thickness;
// f32 min[3]; s32 x/y/z masks; s32 block width shift, x shift, xy shift.
// Sections: f32 positions and normals, 16-byte prisms (f32 height, u16 position, normal,
// edge[3], attribute; indexed from 1, and the unused prism 0 overlaps the last 16 bytes of
// the normals), and an octree of s32 node words whose leaves point to 0-terminated u16
// prism lists (lists may share suffixes).

#include <cstdint>

namespace PetariNative {
namespace KCL {
    const std::uint32_t cHeaderSize = 0x38;
    const std::uint32_t cInitializedFlag = 0x80000000;

    // True when the resource was already converted (the Wii "initialized" test).
    bool isHostResource(const void* data);

    // Converts a big-endian KCL resource of `size` bytes in place. All offsets, octree node
    // words and prism list entries are checked against the resource; on failure the
    // resource is left unmodified and a static description is returned.
    const char* convertInPlace(void* data, std::uint32_t size);

    // Section offsets of a converted resource.
    std::uint32_t positionOffset(const void* hostData);
    std::uint32_t normalOffset(const void* hostData);
    std::uint32_t prismOffset(const void* hostData);
    std::uint32_t octreeOffset(const void* hostData);
}  // namespace KCL
}  // namespace PetariNative

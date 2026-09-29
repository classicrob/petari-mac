#pragma once
// Host-layout images of J3D animation resources ("J3D1" files: bck, bca, bpk, bpa, btp, brk,
// btk, bva, blk, bla).
//
// Same contract as petari/j3d_model.hpp: the loader converts a big-endian file once into a
// same-size copy in which every header, table and key/value scalar is in host byte order.
// A host image is recognised by its first word reading as 'J3D1' in host order.
// Vertex-color animations (VCK1/VCF1) embed 32-bit relocated pointers and are rejected.

#include <cstdint>

namespace PetariNative {
namespace J3D {
    enum class AnimImageKind {
        NotJ3D,
        BigEndian,
        Host,
    };

    AnimImageKind classifyAnimImage(const void* data);

    // Total file size from the big-endian header (0 if data is not a big-endian J3D1 file).
    std::uint32_t animFileSize(const void* data);

    // Converts a big-endian J3D1 file of fileSize bytes into dst (fileSize bytes, may not
    // alias src). Returns nullptr on success, otherwise a static description of the problem.
    const char* makeHostAnimImage(const void* src, std::uint32_t fileSize, void* dst);
}  // namespace J3D
}  // namespace PetariNative

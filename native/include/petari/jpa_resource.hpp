#pragma once
// Host-layout images of JPA particle resource containers ("JPAC2-10", .jpc).
//
// Same contract as petari/j3d_model.hpp: JPAResourceLoader converts a big-endian container
// once into a same-size copy in which every header word, block field and table scalar is in
// host byte order. Block tags and the version word become host-order u32 values, as the
// loader compares them numerically. Texture image and palette bytes stay big-endian; the
// ResTIMG headers become host order (the ResTIMG contract shared with JUTTexture).
// A host image is recognised by its version word reading as '2-10' in host order.

#include <cstdint>

namespace PetariNative {
namespace JPA {
    enum class ImageKind {
        NotJPA,
        BigEndian,
        Host,
    };

    ImageKind classifyImage(const void* data, std::uint32_t size);

    // Converts a big-endian JPAC2-10 container of `size` bytes (the archive resource size)
    // into dst (size bytes, may not alias src). Returns nullptr on success, otherwise a
    // static description of the first problem.
    const char* makeHostImage(const void* src, std::uint32_t size, void* dst);
}  // namespace JPA
}  // namespace PetariNative

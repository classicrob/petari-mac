#pragma once
// Host-layout images of J3D model resources (BMD/BDL/BMT, "J3D2" files).
//
// Wii model files are big-endian and the J3D loader and runtime read them through
// struct pointers. Natively the loader converts a file once into a host-layout image:
// a same-size copy where every header, table and vertex-attribute scalar is in host
// byte order. GX display lists (SHP1 and MDL3) and texture image/palette bytes stay
// big-endian because GX consumes them as byte streams. The original file is not changed.
//
// A host image is recognised by its first word reading as 'J3D2' in host order, which a
// big-endian file never does on a little-endian host, so converting twice is detected.

#include <cstddef>
#include <cstdint>

namespace PetariNative {
namespace J3D {
    enum class ModelImageKind {
        NotJ3D,     // not a J3D2 file
        BigEndian,  // serialized file as stored on disc
        Host,       // already converted by makeHostModelImage
    };

    ModelImageKind classifyModelImage(const void* data);

    // Total file size from the big-endian header (0 if data is not a big-endian J3D2 file).
    std::uint32_t modelFileSize(const void* data);

    // Converts a big-endian J3D2 file of fileSize bytes into dst (fileSize bytes, may not
    // alias src). Every table offset and count is checked against its block and the file.
    // Returns nullptr on success, otherwise a static description of the first problem.
    const char* makeHostModelImage(const void* src, std::uint32_t fileSize, void* dst);

    // Byte span of a VTX1 vertex attribute array in a host image block, bounded by the next
    // table in the block (or the block end). slot: 0 pos, 1 nrm, 2 nbt, 3-4 color, 5-12 tex.
    // Returns 0 when the array is absent.
    std::uint32_t hostVertexArrayBytes(const void* hostVtx1Block, int slot);
}  // namespace J3D
}  // namespace PetariNative

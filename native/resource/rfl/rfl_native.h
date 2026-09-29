#ifndef PETARI_RFL_NATIVE_H
#define PETARI_RFL_NATIVE_H
/*
 * Native support for RVLFaceLib (Miis): big-endian resource access, the
 * packed Mii record codec, and resource validation.
 *
 * On the Wii, RFL reads RFL_Res.dat and Mii records in place. Natively:
 * - Multi-byte resource fields are big-endian; RFL reads them through the
 *   loaders below.
 * - Mii records (RFLiCharData, RFLiHiddenCharData) are big-endian 16-bit
 *   words of bitfields that CodeWarrior allocates from the most significant
 *   bit. Clang on arm64 allocates from the least significant bit, so the
 *   in-memory struct is a host layout and the codec converts at every
 *   boundary with stored data (the default database, the NAND database).
 * - Texture pixels and GX display lists keep their Wii byte order: the GX
 *   backend consumes them as the hardware did.
 */

#include <revolution/types.h>

#include "RFLi_MiddleDatabase.h"
#include "RFLi_Model.h"
#include "RFLi_Texture.h"
#include "RFLi_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RFLi_NATIVE_CHAR_DATA_SIZE 0x4A
#define RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE 0x40
#define RFLi_NATIVE_RESOURCE_ARCHIVES 18

static inline u16 RFLiNativeLoadU16(const void* p) {
    const u8* b = (const u8*)p;
    return (u16)((b[0] << 8) | b[1]);
}

static inline s16 RFLiNativeLoadS16(const void* p) {
    return (s16)RFLiNativeLoadU16(p);
}

static inline u32 RFLiNativeLoadU32(const void* p) {
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | (u32)b[3];
}

static inline f32 RFLiNativeLoadF32(const void* p) {
    union {
        u32 u;
        f32 f;
    } v;
    v.u = RFLiNativeLoadU32(p);
    return v.f;
}

/* Stored (big-endian, CodeWarrior bitfield order) <-> host RFLiCharData.
 * wire is RFLi_NATIVE_CHAR_DATA_SIZE bytes. Names are UTF-16BE on disc and
 * host-order PetariChar16 in the struct. */
void RFLiNativeDecodeCharData(const void* wire, RFLiCharData* out);
void RFLiNativeEncodeCharData(const RFLiCharData* in, void* wire);

/* The same for hidden-database records (RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE). */
void RFLiNativeDecodeHiddenCharData(const void* wire, RFLiHiddenCharData* out);
void RFLiNativeEncodeHiddenCharData(const RFLiHiddenCharData* in, void* wire);

/* Checks an RFL_Res.dat image completely: the archive table, every file
 * table, every texture header and image size, and every shape's vertex
 * arrays and primitive indices. On failure writes a description to error
 * (if errorSize > 0) and returns FALSE. */
BOOL RFLiNativeValidateResource(const void* res, u32 size, char* error, u32 errorSize);

/* Converts a shape copied out of the resource to host byte order in place:
 * the Faceline transforms, the vertex counts, and the s16 positions,
 * normals, and texture coordinates. Primitive indices are bytes. The shape
 * kind comes from its tag. Returns FALSE (and changes nothing) for a shape
 * that RFLiNativeValidateResource would reject. */
BOOL RFLiNativeShapeToHost(void* shape, u32 size);

/* Converts the header of a texture copied out of the resource (RFLiTexture)
 * to host byte order in place. The image stays in GX order. */
void RFLiNativeTextureHeaderToHost(RFLiTexture* texture);

#ifdef __cplusplus
}
#endif

#endif

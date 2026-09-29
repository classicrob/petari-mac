#pragma once
// Native home of the game's J3D overrides that need GX (see native/gx/model_overrides.cpp).
//
// On the Wii, Overwrite.cpp replaces J3DShapeMtx::loadMtxIndx_PNGP so that shape packets
// carrying ShapePacketUserData also load their texture matrices. Natively the override lives
// with the J3D target, and the game registers the ShapePacketUserData step as a hook, so J3D
// links without the game. When no hook is registered no packet can carry user data, which
// matches the override's behavior for packets without it.

#include <revolution/types.h>

class J3DShapePacket;

namespace PetariNative {
namespace J3D {
    typedef void (*ShapePacketTexMtxLoader)(const J3DShapePacket* pPacket, int slot, u16 index);

    void setShapePacketTexMtxLoader(ShapePacketTexMtxLoader loader);
}  // namespace J3D
}  // namespace PetariNative

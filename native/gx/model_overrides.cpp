// Native definitions of the J3D functions that the game overrides in Overwrite.cpp.
// The Wii definitions stay in src/Game/System/Overwrite.cpp under !PETARI_NATIVE.
#include <petari/j3d_overrides.hpp>
#include <JSystem/J3DGraphBase/J3DFifo.hpp>
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShapeMtx.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>

namespace {
    PetariNative::J3D::ShapePacketTexMtxLoader sShapePacketTexMtxLoader = nullptr;
}  // namespace

void PetariNative::J3D::setShapePacketTexMtxLoader(ShapePacketTexMtxLoader loader) {
    sShapePacketTexMtxLoader = loader;
}

void J3DShapeMtx::loadMtxIndx_PNGP(int slot, u16 index) const {
    J3DFifoLoadIndx(0x20, index, 0xB000 | static_cast< u16 >(slot * 12));
    J3DFifoLoadNrmMtxIndx3x3(index, slot * 3);
    if (sShapePacketTexMtxLoader != nullptr) {
        sShapePacketTexMtxLoader(j3dSys.getShapePacket(), slot, index);
    }
}

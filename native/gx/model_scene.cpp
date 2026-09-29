#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <petari/host_allocation.hpp>
#include "archive.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
J3DModel* model;
J3DDrawBuffer* opaque;
J3DDrawBuffer* translucent;
float radius = 100;
Vec center{};
}

extern "C" bool petari_probe_load_model(const char* path) {
    std::vector<unsigned char> file;
    {
        PetariNative::HostAllocationScope host;
        std::ifstream stream(path, std::ios::binary);
        if (!stream) { std::fprintf(stderr, "Cannot read model archive: %s\n", path); return false; }
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(stream), {}};
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size(); ++i) {
            const auto& entry = archive.entries()[i];
            if (!entry.isDirectory() && (entry.name.ends_with(".bdl") || entry.name.ends_with(".bmd"))) {
                file = archive.resourceData(i);
                std::fprintf(stderr, "Probe model: %s (%zu bytes)\n", entry.name.c_str(), file.size());
                break;
            }
        }
    }
    if (file.empty()) return false;
    JKRHeap* previous = JKRHeap::sRootHeap->becomeCurrentHeap();
    const bool bdl = file[4] == 'b' && file[5] == 'd' && file[6] == 'l';
    auto* data = bdl ? J3DModelLoaderDataBase::loadBinaryDisplayList(file.data(), 0x51100000 | J3DMLF_UseUniqueMaterials)
                     : J3DModelLoaderDataBase::load(file.data(), 0x51100000 | J3DMLF_UseUniqueMaterials);
    if (!data) { previous->becomeCurrentHeap(); return false; }
    model = new J3DModel();
    if (model->entryModelData(data, J3DMdlFlag_UseSharedDL | J3DMdlFlag_UseSingleDL, 1) != 0) {
        previous->becomeCurrentHeap();
        return false;
    }
    opaque = new J3DDrawBuffer(32);
    translucent = new J3DDrawBuffer(32);
    j3dSys.mDrawBuffer[0] = opaque;
    j3dSys.mDrawBuffer[1] = translucent;
    const auto& vertex = data->getVertexData();
    if (vertex.mVtxPosType == GX_F32 && vertex.mVtxNum && vertex.mNativeArrayBytes[0] >= vertex.mVtxNum * 12) {
        const auto* positions = static_cast<const float*>(vertex.mVtxPosArray);
        Vec low{positions[0], positions[1], positions[2]}, high = low;
        for (u32 i = 0; i < vertex.mVtxNum; ++i) {
            low.x = std::min(low.x, positions[i * 3]); high.x = std::max(high.x, positions[i * 3]);
            low.y = std::min(low.y, positions[i * 3 + 1]); high.y = std::max(high.y, positions[i * 3 + 1]);
            low.z = std::min(low.z, positions[i * 3 + 2]); high.z = std::max(high.z, positions[i * 3 + 2]);
        }
        center = {(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
        radius = std::max({high.x - low.x, high.y - low.y, high.z - low.z, 1.0f});
    }
    std::fprintf(stderr, "Probe model: %u joints, %u shapes, %u materials\n", data->getJointNum(), data->getShapeNum(), data->getMaterialNum());
    previous->becomeCurrentHeap();
    return true;
}

extern "C" void petari_probe_draw_model(unsigned frame) {
    j3dSys.drawInit();
    GXSetCopyClear({12, 18, 32, 255}, 0xFFFFFF);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXSetScissor(0, 0, 640, 480);
    Mtx44 projection;
    C_MTXPerspective(projection, 50, 4.0f / 3.0f, radius * 0.01f, radius * 20);
    GXSetProjection(projection, GX_PERSPECTIVE);
    Vec camera{center.x, center.y + radius * 0.25f, center.z + radius * 2.5f};
    Vec up{0, 1, 0};
    C_MTXLookAt(j3dSys.mViewMtx, &camera, &up, &center);
    Mtx rotation;
    PSMTXRotRad(rotation, 'y', frame * 0.01f);
    model->setBaseTRMtx(rotation);
    GXLightObj light{};
    GXInitLightPos(&light, radius * 3, radius * 4, radius * 5);
    GXInitLightColor(&light, {255, 255, 255, 255});
    GXInitLightAttn(&light, 1, 0, 0, 1, 0, 0);
    GXLoadLightObjImm(&light, GX_LIGHT0);
    opaque->frameInit();
    translucent->frameInit();
    model->update();
    model->viewCalc();
    j3dSys.mDrawMode = 1;
    opaque->draw();
    j3dSys.mDrawMode = 2;
    translucent->draw();
}

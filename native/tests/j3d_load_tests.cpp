// J3D model loading tests: real J3DModelData construction through J3DModelLoaderDataBase.
//
// Default: the synthetic conversion checks (shared with j3d_model_tests.cpp).
// With --assets FILES [--load-limit N]: models from ObjectData/StageData archives are loaded
// into real J3DModelData objects (joints, hierarchy, materials, shapes, vertex data,
// textures), each on its own JKR heap.
// Links: J3DGraphLoader (except J3DAnmLoader), J3DGraphBase, J3DGraphAnimator, JUTNameTab,
// JUTAssert, JUTNativeResource, native/resource/j3d_model.cpp, the RVL GD library, a GX
// implementation, petari_heaps/petari_boot/petari_core/petari_resources.
#include "archive.hpp"
#include <petari/boot.hpp>
#include <petari/endian.hpp>
#include <petari/j3d_model.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Buffer = std::vector< std::uint8_t >;
namespace J3D = PetariNative::J3D;

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

static void put32(Buffer& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast< std::uint8_t >(value >> (24 - i * 8));
    }
}

static void put16(Buffer& data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast< std::uint8_t >(value >> 8);
    data[offset + 1] = static_cast< std::uint8_t >(value);
}

static std::uint32_t floatBits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

template < typename T >
static T hostAt(const Buffer& data, std::size_t offset) {
    T value;
    std::memcpy(&value, &data[offset], sizeof(value));
    return value;
}

namespace {
    // Builds big-endian blocks with a table directory, so offsets are filled in once each
    // table has been placed.
    struct BlockWriter {
        Buffer mData;

        explicit BlockWriter(const char* pType, std::size_t headerSize) : mData(headerSize, 0) {
            std::memcpy(mData.data(), pType, 4);
        }

        std::size_t place(const Buffer& table, std::size_t field, std::size_t alignment = 4) {
            while (mData.size() % alignment != 0) {
                mData.push_back(0);
            }
            const std::size_t offset = mData.size();
            mData.insert(mData.end(), table.begin(), table.end());
            put32(mData, field, static_cast< std::uint32_t >(offset));
            return offset;
        }

        Buffer finish() {
            while (mData.size() % 0x20 != 0) {
                mData.push_back(0);
            }
            put32(mData, 4, static_cast< std::uint32_t >(mData.size()));
            return mData;
        }
    };

    Buffer nameTable(const std::vector< std::string >& names) {
        Buffer data(4 + names.size() * 4);
        put16(data, 0, static_cast< std::uint16_t >(names.size()));
        put16(data, 2, 0xFFFF);
        for (std::size_t i = 0; i < names.size(); i++) {
            std::uint16_t hash = 0;
            for (char c : names[i]) {
                hash = static_cast< std::uint16_t >(hash * 3 + static_cast< std::uint8_t >(c));
            }
            put16(data, 4 + i * 4, hash);
            put16(data, 6 + i * 4, static_cast< std::uint16_t >(data.size()));
            data.insert(data.end(), names[i].begin(), names[i].end());
            data.push_back(0);
        }
        return data;
    }

    Buffer u16Table(const std::vector< std::uint16_t >& values) {
        Buffer data(values.size() * 2);
        for (std::size_t i = 0; i < values.size(); i++) {
            put16(data, i * 2, values[i]);
        }
        return data;
    }
}  // namespace

// A minimal but complete BDL: one joint, one material (MAT3 + MDL3), one shape, one texture.
static Buffer buildSyntheticModel() {
    std::vector< Buffer > blocks;

    {
        BlockWriter inf("INF1", 0x18);
        put16(inf.mData, 0x08, 0x0002);  // flags
        put32(inf.mData, 0x0C, 1);       // packet count
        put32(inf.mData, 0x10, 3);       // vertex count
        // Hierarchy: joint 0, down, material 0, shape 0, up, end.
        inf.place(u16Table({0x10, 0, 0x01, 0, 0x11, 0, 0x12, 0, 0x02, 0, 0x00, 0}), 0x14);
        blocks.push_back(inf.finish());
    }
    {
        BlockWriter vtx("VTX1", 0x40);
        Buffer fmt(0x40);
        const std::uint32_t formats[][4] = {{9, 1, 4, 0}, {13, 1, 3, 8}, {0xFF, 1, 0, 0}};  // POS F32 XYZ, TEX0 S16 ST
        for (int i = 0; i < 3; i++) {
            put32(fmt, i * 16, formats[i][0]);
            put32(fmt, i * 16 + 4, formats[i][1]);
            put32(fmt, i * 16 + 8, formats[i][2]);
            fmt[i * 16 + 12] = static_cast< std::uint8_t >(formats[i][3]);
        }
        fmt.resize(0x30);
        vtx.place(fmt, 0x08);
        Buffer positions(3 * 12);
        const float coords[9] = {0.0f, 1.0f, 2.0f, -3.5f, 4.25f, 5.0f, 100.0f, -200.0f, 0.125f};
        for (int i = 0; i < 9; i++) {
            put32(positions, i * 4, floatBits(coords[i]));
        }
        vtx.place(positions, 0x0C, 0x20);
        vtx.place(u16Table({0x0100, 0xFF00, 0x7FFF, 0x8000, 0x0001, 0x1234}), 0x20, 0x20);
        blocks.push_back(vtx.finish());
    }
    {
        BlockWriter evp("EVP1", 0x1C);  // no weighted matrices
        blocks.push_back(evp.finish());
    }
    {
        BlockWriter drw("DRW1", 0x14);
        put16(drw.mData, 0x08, 1);
        drw.place(Buffer{0}, 0x0C);
        drw.place(u16Table({0}), 0x10);
        blocks.push_back(drw.finish());
    }
    {
        BlockWriter jnt("JNT1", 0x18);
        put16(jnt.mData, 0x08, 1);
        Buffer init(0x40);
        put16(init, 0x00, 0);
        init[0x02] = 1;
        const float scale[3] = {1.0f, 2.0f, 3.0f};
        for (int i = 0; i < 3; i++) {
            put32(init, 0x04 + i * 4, floatBits(scale[i]));
        }
        put16(init, 0x10, 0x4000);
        put16(init, 0x12, 0xC000);
        put16(init, 0x14, 0x0010);
        const float translate[3] = {10.0f, -20.0f, 30.5f};
        for (int i = 0; i < 3; i++) {
            put32(init, 0x18 + i * 4, floatBits(translate[i]));
        }
        put32(init, 0x24, floatBits(50.0f));
        jnt.place(init, 0x0C);
        jnt.place(u16Table({0}), 0x10);
        jnt.place(nameTable({"center"}), 0x14);
        blocks.push_back(jnt.finish());
    }
    {
        BlockWriter shp("SHP1", 0x2C);
        put16(shp.mData, 0x08, 1);
        Buffer init(0x28);
        init[0x00] = 0;
        put16(init, 0x02, 1);  // matrix groups
        put16(init, 0x04, 0);  // vtx desc list index
        put16(init, 0x06, 0);  // mtx init index
        put16(init, 0x08, 0);  // draw init index
        put32(init, 0x0C, floatBits(75.0f));
        shp.place(init, 0x0C);
        shp.place(u16Table({0}), 0x10);
        shp.place(nameTable({"shape"}), 0x14);
        Buffer desc(3 * 8);
        const std::uint32_t descs[][2] = {{9, 3}, {13, 3}, {0xFF, 0}};  // POS/TEX0 index16
        for (int i = 0; i < 3; i++) {
            put32(desc, i * 8, descs[i][0]);
            put32(desc, i * 8 + 4, descs[i][1]);
        }
        shp.place(desc, 0x18);
        shp.place(u16Table({0}), 0x1C);
        // One triangle: GX_DRAW_TRIANGLES (0x90), vtxfmt 0, 3 vertices, (pos, tex) u16 indices.
        Buffer dl = {0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x02};
        dl.resize(0x20, 0);
        shp.place(dl, 0x20, 0x20);
        Buffer mtxInit(8);
        put16(mtxInit, 0, 0);
        put16(mtxInit, 2, 1);
        put32(mtxInit, 4, 0);
        shp.place(mtxInit, 0x24);
        Buffer drawInit(8);
        put32(drawInit, 0, 0x20);
        put32(drawInit, 4, 0);
        shp.place(drawInit, 0x28);
        blocks.push_back(shp.finish());
    }
    {
        BlockWriter mat("MAT3", 0x84);
        put16(mat.mData, 0x08, 1);
        Buffer init(0x14C, 0xFF);
        init[0x00] = 1;  // material mode
        init[0x01] = 0;  // cull index
        init[0x02] = 0;  // color channel num index
        init[0x03] = 0;  // tex gen num index
        init[0x04] = 0;  // tev stage num index
        for (int i = 0; i < 2; i++) {
            put16(init, 0x08 + i * 2, 0);  // mat color
        }
        put16(init, 0x28, 0);  // tex coord 0
        put16(init, 0x84, 0);  // tex no 0
        put16(init, 0xBC, 0);  // tev order 0
        put16(init, 0xE4, 0);  // tev stage 0
        put16(init, 0x144, 0xFFFF);
        put16(init, 0x146, 0);  // alpha comp
        put16(init, 0x148, 0);  // blend
        put16(init, 0x14A, 0xFFFF);
        mat.place(init, 0x0C);
        mat.place(u16Table({0}), 0x10);
        mat.place(nameTable({"mat"}), 0x14);
        Buffer cull(4);
        put32(cull, 0, 2);  // GX_CULL_BACK
        mat.place(cull, 0x1C);
        mat.place(Buffer{0xFF, 0x80, 0x40, 0xFF}, 0x20);
        mat.place(Buffer{1}, 0x24);
        mat.place(Buffer{1}, 0x34);
        mat.place(Buffer{1, 4, 60, 0}, 0x38);  // GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY
        mat.place(u16Table({0}), 0x48);
        mat.place(Buffer{0, 0, 4, 0xFF}, 0x4C);
        mat.place(Buffer{1}, 0x58);
        Buffer stage(20, 0);
        mat.place(stage, 0x5C);
        mat.place(Buffer{7, 0, 0, 7, 0, 0, 0, 0}, 0x6C);
        mat.place(Buffer{1, 4, 5, 15}, 0x70);
        blocks.push_back(mat.finish());
    }
    {
        BlockWriter mdl("MDL3", 0x24);
        put16(mdl.mData, 0x08, 1);
        Buffer dlInit(8);
        put32(dlInit, 0, 0x40);  // relative to this entry
        put32(dlInit, 4, 0x20);
        const std::size_t dlInitOffset = mdl.place(dlInit, 0x0C);
        Buffer patching(0x10);
        for (int i = 0; i < 6; i++) {
            put16(patching, i * 2, 0);
        }
        mdl.place(patching, 0x10);
        Buffer currentMtx(8);
        put32(currentMtx, 0, 0x3CF3CF00);
        put32(currentMtx, 4, 0x00F3CF3C);
        mdl.place(currentMtx, 0x14);
        mdl.place(Buffer{1}, 0x18);
        mdl.place(u16Table({0}), 0x1C);
        mdl.place(nameTable({"mat"}), 0x20);
        while (mdl.mData.size() < dlInitOffset + 0x40) {
            mdl.mData.push_back(0);
        }
        Buffer dl(0x20, 0);  // GX_NOP padding: a valid empty material display list
        mdl.mData.insert(mdl.mData.end(), dl.begin(), dl.end());
        blocks.push_back(mdl.finish());
    }
    {
        BlockWriter tex("TEX1", 0x14);
        put16(tex.mData, 0x08, 1);
        Buffer timg(0x20 + 0x20);
        timg[0x00] = 6;  // GX_TF_RGBA8
        put16(timg, 0x02, 4);
        put16(timg, 0x04, 4);
        put16(timg, 0x1A, 0xFFF0);
        put32(timg, 0x1C, 0x20);  // image data right after the header
        for (int i = 0; i < 0x20; i++) {
            timg[0x20 + i] = static_cast< std::uint8_t >(i);
        }
        tex.place(timg, 0x0C, 0x20);
        tex.place(nameTable({"image"}), 0x10);
        blocks.push_back(tex.finish());
    }

    Buffer file(0x20);
    std::memcpy(file.data(), "J3D2bdl4", 8);
    put32(file, 0x0C, static_cast< std::uint32_t >(blocks.size()));
    std::memcpy(&file[0x10], "SVR3", 4);
    for (const Buffer& block : blocks) {
        file.insert(file.end(), block.begin(), block.end());
    }
    put32(file, 0x08, static_cast< std::uint32_t >(file.size()));
    return file;
}

static std::size_t findBlock(const Buffer& image, const char* pType) {
    const std::uint32_t count = hostAt< std::uint32_t >(image, 0x0C);
    std::size_t block = 0x20;
    for (std::uint32_t i = 0; i < count; i++) {
        if (PetariNative::readU32BE(pType) == hostAt< std::uint32_t >(image, block)) {
            return block;
        }
        block += hostAt< std::uint32_t >(image, block + 4);
    }
    return 0;
}

static void testConversion() {
    const Buffer file = buildSyntheticModel();
    const Buffer original = file;
    check(J3D::classifyModelImage(file.data()) == J3D::ModelImageKind::BigEndian, "classify big-endian file");
    check(J3D::modelFileSize(file.data()) == file.size(), "file size from header");

    Buffer image(file.size());
    const char* pError = J3D::makeHostModelImage(file.data(), static_cast< std::uint32_t >(file.size()), image.data());
    check(pError == nullptr, pError != nullptr ? pError : "conversion");
    check(file == original, "source file left unmodified");
    check(J3D::classifyModelImage(image.data()) == J3D::ModelImageKind::Host, "classify host image");
    check(J3D::modelFileSize(image.data()) == 0, "host image is not reported as a big-endian file");

    const std::size_t inf = findBlock(image, "INF1");
    check(inf != 0 && hostAt< std::uint16_t >(image, inf + 8) == 2 && hostAt< std::uint32_t >(image, inf + 0x10) == 3, "INF1 scalars");
    check(hostAt< std::uint16_t >(image, inf + hostAt< std::uint32_t >(image, inf + 0x14)) == 0x10, "hierarchy entries");

    const std::size_t vtx = findBlock(image, "VTX1");
    const std::size_t pos = vtx + hostAt< std::uint32_t >(image, vtx + 0x0C);
    check(hostAt< float >(image, pos + 4) == 1.0f && hostAt< float >(image, pos + 12) == -3.5f && hostAt< float >(image, pos + 32) == 0.125f,
          "F32 positions in host order");
    const std::size_t tex = vtx + hostAt< std::uint32_t >(image, vtx + 0x20);
    check(hostAt< std::uint16_t >(image, tex) == 0x0100 && hostAt< std::int16_t >(image, tex + 6) == -32768, "S16 texcoords in host order");
    check(hostAt< std::uint32_t >(image, vtx + hostAt< std::uint32_t >(image, vtx + 8) + 8) == 4, "format list in host order");
    check(J3D::hostVertexArrayBytes(&image[vtx], 0) == 0x40 && J3D::hostVertexArrayBytes(&image[vtx], 5) == 0x20 &&
              J3D::hostVertexArrayBytes(&image[vtx], 1) == 0,
          "vertex array spans");

    const std::size_t jnt = findBlock(image, "JNT1");
    const std::size_t joint = jnt + hostAt< std::uint32_t >(image, jnt + 0x0C);
    check(hostAt< float >(image, joint + 0x08) == 2.0f && hostAt< std::int16_t >(image, joint + 0x12) == -16384 &&
              hostAt< float >(image, joint + 0x20) == 30.5f && hostAt< float >(image, joint + 0x24) == 50.0f,
          "joint init data");

    const std::size_t shp = findBlock(image, "SHP1");
    const std::size_t dl = shp + hostAt< std::uint32_t >(image, shp + 0x20);
    check(image[dl] == 0x90 && image[dl + 2] == 0x03 && image[dl + 12] == 0x02, "shape display list stays big-endian");

    const std::size_t tex1 = findBlock(image, "TEX1");
    const std::size_t timg = tex1 + hostAt< std::uint32_t >(image, tex1 + 0x0C);
    check(hostAt< std::uint16_t >(image, timg + 2) == 4 && hostAt< std::int16_t >(image, timg + 0x1A) == -16 &&
              hostAt< std::uint32_t >(image, timg + 0x1C) == 0x20 && image[timg + 0x21] == 1,
          "ResTIMG header host order, image bytes unchanged");
    const std::size_t names = tex1 + hostAt< std::uint32_t >(image, tex1 + 0x10);
    check(hostAt< std::uint16_t >(image, names) == 1 && std::strcmp(reinterpret_cast< const char* >(&image[names + hostAt< std::uint16_t >(image, names + 6)]), "image") == 0,
          "name table host order");

    // Malformed inputs are rejected instead of converted.
    Buffer bad = file;
    put32(bad, 0x20 + 4, 0x7FFFFFFF);
    check(J3D::makeHostModelImage(bad.data(), static_cast< std::uint32_t >(bad.size()), image.data()) != nullptr, "oversized block rejected");
    bad = file;
    put32(bad, 0x20 + 0x14, 0x00100000);
    check(J3D::makeHostModelImage(bad.data(), static_cast< std::uint32_t >(bad.size()), image.data()) != nullptr, "table offset outside block rejected");
    bad = file;
    std::memcpy(&bad[0x20], "XXX1", 4);
    check(J3D::makeHostModelImage(bad.data(), static_cast< std::uint32_t >(bad.size()), image.data()) != nullptr, "unknown block rejected");
    bad = file;
    put32(bad, 0x08, static_cast< std::uint32_t >(file.size() + 4));
    check(J3D::makeHostModelImage(bad.data(), static_cast< std::uint32_t >(file.size()), image.data()) != nullptr, "size mismatch rejected");
}

// Loads a model through the real J3D loader and checks the constructed objects against the
// counts in the (converted) file.
static bool loadAndCheck(const Buffer& file, const std::string& where, bool isMaterialTable) {
    Buffer image(file.size());
    if (const char* pError = J3D::makeHostModelImage(file.data(), static_cast< std::uint32_t >(file.size()), image.data())) {
        std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pError);
        return false;
    }

    auto blockCount = [&](const char* pType) -> std::uint32_t {
        const std::size_t block = findBlock(image, pType);
        return block != 0 ? hostAt< std::uint16_t >(image, block + 8) : 0;
    };

    if (isMaterialTable) {
        J3DMaterialTable* pTable = J3DModelLoaderDataBase::loadMaterialTable(file.data());
        const bool ok = pTable != nullptr && pTable->getMaterialNum() == blockCount("MAT3") && pTable->getTexture() != nullptr &&
                        pTable->getTexture()->getNum() == blockCount("TEX1");
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s: material table\n", where.c_str());
        }
        return ok;
    }

    const bool isBdl = std::memcmp(&file[4], "bdl", 3) == 0;
    J3DModelData* pData = isBdl ? J3DModelLoaderDataBase::loadBinaryDisplayList(file.data(), 0x51100000 | J3DMLF_UseUniqueMaterials)
                                : J3DModelLoaderDataBase::load(file.data(), 0x51100000 | J3DMLF_UseUniqueMaterials);
    if (pData == nullptr) {
        std::fprintf(stderr, "FAIL: %s: loader returned NULL\n", where.c_str());
        return false;
    }

    bool ok = true;
    auto expect = [&](bool condition, const char* pWhat) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pWhat);
            ok = false;
        }
    };

    expect(pData->getJointNum() == blockCount("JNT1"), "joint count");
    expect(pData->getMaterialNum() == blockCount("MAT3"), "material count");
    expect(pData->getShapeNum() == blockCount("SHP1"), "shape count");
    expect(pData->getTexture() != nullptr && pData->getTexture()->getNum() == blockCount("TEX1"), "texture count");
    expect(pData->getJointTree().getRootNode() != nullptr, "joint hierarchy root");
    expect(pData->getVertexData().getVtxPosArray() != nullptr && pData->getVertexData().mNativeArrayBytes[0] > 0, "position array");

    // Every joint is reachable from the root, and every shape is attached to a material.
    std::vector< J3DJoint* > stack = {pData->getJointTree().getRootNode()};
    std::size_t reached = 0;
    while (!stack.empty() && reached <= pData->getJointNum()) {
        J3DJoint* pJoint = stack.back();
        stack.pop_back();
        if (pJoint == nullptr) {
            continue;
        }
        reached++;
        stack.push_back(pJoint->getYounger());
        stack.push_back(pJoint->getChild());
    }
    expect(reached == pData->getJointNum(), "all joints reachable in the hierarchy");

    for (u16 i = 0; i < pData->getMaterialNum(); i++) {
        J3DMaterial* pMaterial = pData->getMaterialNodePointer(i);
        expect(pMaterial != nullptr && pMaterial->getShape() != nullptr && pMaterial->getTevBlock() != nullptr, "material shape/tev block");
        if (pMaterial != nullptr && pMaterial->getTevBlock() != nullptr) {
            expect(pMaterial->getTevBlock()->getTevStageNum() >= 1 && pMaterial->getTevBlock()->getTevStageNum() <= 16, "TEV stage count");
        }
    }

    const J3DVertexData& vertex = pData->getVertexData();
    for (u16 i = 0; i < pData->getShapeNum(); i++) {
        J3DShape* pShape = pData->getShapeNodePointer(i);
        expect(pShape != nullptr && pShape->getVtxDesc() != nullptr, "shape vertex descriptors");
    }

    // Positions must be finite host floats when stored as F32. INF1's vertex count is the
    // number of positions; the array span also covers the block's alignment padding
    // ("This is padding data..." text), which is not vertex data.
    if (vertex.getVtxPosType() == GX_F32) {
        const f32* pPositions = static_cast< const f32* >(vertex.getVtxPosArray());
        const u32 count = vertex.getVtxNum() * 3;
        expect(count * sizeof(f32) <= vertex.mNativeArrayBytes[0], "INF1 vertex count exceeds the position array");
        for (u32 i = 0; i < count && i * sizeof(f32) < vertex.mNativeArrayBytes[0]; i++) {
            if (!(pPositions[i] > -1.0e7f && pPositions[i] < 1.0e7f)) {
                expect(false, "F32 positions decoded to implausible values");
                break;
            }
        }
    }

    const ResTIMG* pTexture = pData->getTexture() != nullptr && pData->getTexture()->getNum() != 0 ? pData->getTexture()->getResTIMG(0) : nullptr;
    if (pTexture != nullptr) {
        expect(pTexture->mWidth > 0 && pTexture->mWidth <= 1024 && pTexture->mHeight > 0 && pTexture->mHeight <= 1024, "texture header");
    }
    return ok;
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

static void testAssets(const std::filesystem::path& filesRoot, std::size_t loadLimit) {
    namespace fs = std::filesystem;
    std::vector< fs::path > archives;
    for (const char* pDir : {"ObjectData", "StageData"}) {
        if (fs::is_directory(filesRoot / pDir)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(filesRoot / pDir)) {
                if (entry.path().extension() == ".arc") {
                    archives.push_back(entry.path());
                }
            }
        }
    }
    std::sort(archives.begin(), archives.end());
    check(!archives.empty(), "no archives found under the asset directory");

    std::size_t converted = 0, loaded = 0, failed = 0;
    for (const fs::path& path : archives) {
        Buffer bytes;
        if (!readFile(path, &bytes)) {
            continue;
        }
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            const auto& entry = archive.entries()[i];
            const std::string& name = entry.name;
            if (entry.isDirectory() || name.size() < 4) {
                continue;
            }
            const std::string ext = name.substr(name.size() - 4);
            if (ext != ".bdl" && ext != ".bmd" && ext != ".bmt") {
                continue;
            }

            const Buffer file = archive.resourceData(i);
            const std::string where = path.filename().string() + ":" + name;
            Buffer image(file.size());
            if (const char* pError = J3D::makeHostModelImage(file.data(), static_cast< std::uint32_t >(file.size()), image.data())) {
                std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pError);
                failed++;
                continue;
            }
            converted++;

            if (loaded < loadLimit) {
                JKRExpHeap* pHeap = JKRExpHeap::create(16 * 1024 * 1024, JKRHeap::sRootHeap, false);
                JKRHeap* pPrevious = pHeap->becomeCurrentHeap();
                if (!loadAndCheck(file, where, ext == ".bmt")) {
                    failed++;
                }
                pPrevious->becomeCurrentHeap();
                JKRHeap::destroy(pHeap);
                loaded++;
            }
        }
    }

    std::printf("J3D models: %zu converted, %zu loaded into J3DModelData, %zu failed\n", converted, loaded, failed);
    sFailures += static_cast< int >(failed);
}

int main(int argc, char** argv) {
    testConversion();

    if (argc >= 3 && std::strcmp(argv[1], "--assets") == 0) {
        OSInit();
        JKRExpHeap::createRoot(1, false);
        const std::size_t limit = argc >= 5 && std::strcmp(argv[3], "--load-limit") == 0 ? std::strtoul(argv[4], nullptr, 10) : 200;
        testAssets(argv[2], limit);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR [--load-limit N]]\n", argv[0]);
        return 2;
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d J3D model check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("J3D model tests passed");
    return 0;
}

// Material animators must reach native materials. J3DMaterial::getMaterialAnm()
// separated Wii RAM pointers from 32-bit sentinels with "< 0xC0000000", which
// rejects every host pointer above 4 GiB: BTP, BRK, BPK and BTK animators were
// then never attached (file-select Mario kept closed eyes).
//
// Usage: petari_j3d_material_anm_tests [--assets GAME_FILES_DIR]
// Links: petari_j3d, native/tests/heap_diagnostics.cpp, native/gx/legacy_commands.cpp.
#include "archive.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DMaterialAnm.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" void OSInit();

static int sFailures;
static int sChecks;

#define CHECK(expr)                                                                                                                                  \
    do {                                                                                                                                             \
        sChecks++;                                                                                                                                   \
        if (!(expr)) {                                                                                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                                                          \
            sFailures++;                                                                                                                             \
        }                                                                                                                                            \
    } while (0)

namespace {
using Buffer = std::vector< std::uint8_t >;

// The accessor keeps the Wii's sentinel range and accepts host pointers.
void testAccessor() {
    static J3DMaterialAnm sAnm;
    J3DMaterial material;
    material.mMaterialAnm = nullptr;
    CHECK(material.getMaterialAnm() == nullptr);
    material.mMaterialAnm = &sAnm;
    CHECK(reinterpret_cast< std::uintptr_t >(&sAnm) > 0xFFFFFFFFu);
    CHECK(material.getMaterialAnm() == &sAnm);
    for (std::uintptr_t sentinel : {std::uintptr_t(0xC0000000u), std::uintptr_t(0xFFFFFFFFu)}) {
        material.mMaterialAnm = reinterpret_cast< J3DMaterialAnm* >(sentinel);
        CHECK(material.getMaterialAnm() == nullptr);
    }
    material.mMaterialAnm = reinterpret_cast< J3DMaterialAnm* >(std::uintptr_t(0x80400000u));
    CHECK(material.getMaterialAnm() == reinterpret_cast< J3DMaterialAnm* >(std::uintptr_t(0x80400000u)));
    material.mMaterialAnm = nullptr;
}

struct Archive {
    Buffer bytes;
    std::vector< Buffer > files;
    std::vector< std::string > names;

    explicit Archive(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator< char >(stream), {});
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size(); i++) {
            if (!archive.entries()[i].isDirectory()) {
                names.push_back(archive.entries()[i].name);
                files.push_back(archive.resourceData(i));
            }
        }
    }

    Buffer* find(const std::string& name) {
        for (std::size_t i = 0; i < names.size(); i++) {
            if (names[i] == name) {
                return &files[i];
            }
        }
        return nullptr;
    }
};

// As MaterialAnmBuffer::attachMaterialAnmBuffer does for animated models.
J3DModelData* loadAnimatedModel(Buffer& bdl) {
    J3DModelData* data = J3DModelLoaderDataBase::loadBinaryDisplayList(bdl.data(), J3DMLF_Material_UseIndirect | J3DMLF_UseUniqueMaterials);
    J3DMaterialAnm* anms = new J3DMaterialAnm[data->getMaterialNum()];
    for (u16 i = 0; i < data->getMaterialNum(); i++) {
        data->getMaterialNodePointer(i)->mMaterialAnm = &anms[i];
    }
    return data;
}

// The file-select head opens its eyes: wait.btp sets EyeMat_v to FileSelectDataMarioEye.0.
void testEyePattern(const std::filesystem::path& files) {
    Archive archive(files / "ObjectData" / "FileSelectDataMario.arc");
    Buffer* bdl = archive.find("fileselectdatamario.bdl");
    Buffer* btp = archive.find("wait.btp");
    CHECK(bdl != nullptr && btp != nullptr);
    if (bdl == nullptr || btp == nullptr) {
        return;
    }
    J3DModelData* data = loadAnimatedModel(*bdl);
    J3DMaterial* eye = data->getMaterialNodePointer(2);
    CHECK(std::strcmp(data->getMaterialName()->getName(2), "EyeMat_v") == 0);
    CHECK(eye->getTevBlock()->getTexNo(0) == 4);  // FileSelectDataMarioEye.2, closed

    J3DAnmTexPattern* pattern = static_cast< J3DAnmTexPattern* >(J3DAnmLoaderDataBase::load(btp->data()));
    pattern->searchUpdateMaterialID(data);
    CHECK(data->getMaterialTable().entryTexNoAnimator(pattern) == 0);
    pattern->setFrame(0.0f);
    CHECK(eye->getMaterialAnm() != nullptr);
    if (eye->getMaterialAnm() != nullptr) {
        eye->getMaterialAnm()->calc(eye);
    }
    const u16 texNo = eye->getTevBlock()->getTexNo(0);
    CHECK(texNo == 1);
    CHECK(std::strcmp(data->getTextureName()->getName(texNo), "FileSelectDataMarioEye.0") == 0);
}

// Toad clothes take their color from ColorChange.brk (a TEV register animation).
void testClothesColor(const std::filesystem::path& files) {
    Archive archive(files / "ObjectData" / "Kinopio.arc");
    Buffer* bdl = archive.find("kinopio.bdl");
    Buffer* brk = archive.find("colorchange.brk");
    CHECK(bdl != nullptr && brk != nullptr);
    if (bdl == nullptr || brk == nullptr) {
        return;
    }
    J3DModelData* data = loadAnimatedModel(*bdl);
    J3DMaterial* clothes = data->getMaterialNodePointer(0);
    CHECK(std::strcmp(data->getMaterialName()->getName(0), "Clothes_v") == 0);

    J3DAnmTevRegKey* key = static_cast< J3DAnmTevRegKey* >(J3DAnmLoaderDataBase::load(brk->data()));
    key->searchUpdateMaterialID(data);
    CHECK(data->getMaterialTable().entryTevRegAnimator(key) == 0);
    // Key frame 1 of Clothes_v register C0 is (-103, 315, -103, -103); the MAT3 default is
    // (211, -103, -107, -103), so an unattached animator keeps a different color.
    key->setFrame(1.0f);
    if (clothes->getMaterialAnm() != nullptr) {
        clothes->getMaterialAnm()->calc(clothes);
    }
    const J3DGXColorS10* color = clothes->getTevBlock()->getTevColor(0);
    CHECK(color->r == -103 && color->g == 315 && color->b == -103 && color->a == -103);
}

// Material color (BPK) and texture SRT (BTK) animators on StarCursor: the values the
// material holds after J3DMaterialAnm::calc are the animation's own at that frame.
void testColorAndTexMtx(const std::filesystem::path& files) {
    Archive archive(files / "ObjectData" / "StarCursor.arc");
    Buffer* bdl = archive.find("starcursor.bdl");
    Buffer* bpk = archive.find("starcursorstart.bpk");
    Buffer* btk = archive.find("starcursor.btk");
    CHECK(bdl != nullptr && bpk != nullptr && btk != nullptr);
    if (bdl == nullptr || bpk == nullptr || btk == nullptr) {
        return;
    }
    J3DModelData* data = loadAnimatedModel(*bdl);

    J3DAnmColor* color = static_cast< J3DAnmColor* >(J3DAnmLoaderDataBase::load(bpk->data()));
    color->searchUpdateMaterialID(data);
    CHECK(color->getUpdateMaterialNum() > 0 && color->isValidUpdateMaterialID(0));
    CHECK(data->getMaterialTable().entryMatColorAnimator(color) == 0);
    color->setFrame(color->getFrameMax() * 0.5f);
    bool colorOk = color->getUpdateMaterialNum() > 0;
    for (u16 i = 0; i < color->getUpdateMaterialNum(); i++) {
        if (!color->isValidUpdateMaterialID(i)) {
            continue;
        }
        J3DMaterial* material = data->getMaterialNodePointer(color->getUpdateMaterialID(i));
        colorOk &= material->getMaterialAnm() != nullptr;
        if (material->getMaterialAnm() == nullptr) {
            continue;
        }
        material->getMaterialAnm()->calc(material);
        GXColor expected;
        color->getColor(i, &expected);
        const GXColor* actual = material->getColorBlock()->getMatColor(0);
        colorOk &= std::memcmp(actual, &expected, sizeof(expected)) == 0;
    }
    CHECK(colorOk);

    J3DAnmTextureSRTKey* srt = static_cast< J3DAnmTextureSRTKey* >(J3DAnmLoaderDataBase::load(btk->data()));
    srt->searchUpdateMaterialID(data);
    CHECK(srt->getUpdateMaterialNum() > 0 && srt->isValidUpdateMaterialID(0));
    int rv = data->getMaterialTable().entryTexMtxAnimator(srt);
    if (rv == 4) {
        // The first call creates missing texture matrices and stops, as on the Wii.
        rv = data->getMaterialTable().entryTexMtxAnimator(srt);
    }
    CHECK(rv == 0);
    srt->setFrame(srt->getFrameMax() * 0.5f);
    bool srtOk = srt->getUpdateMaterialNum() > 0;
    for (u16 i = 0; i < srt->getUpdateMaterialNum(); i++) {
        if (!srt->isValidUpdateMaterialID(i) || srt->getUpdateTexMtxID(i) == 0xff) {
            continue;
        }
        J3DMaterial* material = data->getMaterialNodePointer(srt->getUpdateMaterialID(i));
        srtOk &= material->getMaterialAnm() != nullptr;
        if (material->getMaterialAnm() == nullptr) {
            continue;
        }
        material->getMaterialAnm()->calc(material);
        J3DTextureSRTInfo expected;
        srt->getTransform(i, &expected);
        const J3DTextureSRTInfo& actual = material->getTexMtx(srt->getUpdateTexMtxID(i))->getTexMtxInfo().mSRT;
        srtOk &= actual.mScaleX == expected.mScaleX && actual.mScaleY == expected.mScaleY && actual.mRotation == expected.mRotation &&
                 actual.mTranslationX == expected.mTranslationX && actual.mTranslationY == expected.mTranslationY;
    }
    CHECK(srtOk);
}
}  // namespace

int main(int argc, char** argv) {
    OSInit();
    JKRExpHeap::createRoot(1, false);
    testAccessor();

    const char* assets = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--assets") == 0) {
            assets = argv[i + 1];
        }
    }
    if (assets != nullptr) {
        testEyePattern(assets);
        testClothesColor(assets);
        testColorAndTexMtx(assets);
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d material animation check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("j3d material animation tests passed (%d checks%s)\n", sChecks, assets ? ", with assets" : "");
    return 0;
}

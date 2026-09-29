// Material animators must reach native materials. J3DMaterial::getMaterialAnm()
// separated Wii RAM pointers from 32-bit sentinels with "< 0xC0000000", which
// rejects every host pointer above 4 GiB: BTP, BRK, BPK and BTK animators were
// then never attached (file-select Mario kept closed eyes).
//
// Usage: petari_j3d_material_anm_tests [--assets GAME_FILES_DIR] [--corpus GAME_FILES_DIR]
// Links: petari_j3d, native/tests/heap_diagnostics.cpp, native/gx/legacy_commands.cpp.
#include "archive.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphAnimator/J3DMaterialAnm.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <petari/host_allocation.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

bool endsWith(const std::string& name, const char* suffix) {
    const std::size_t length = std::strlen(suffix);
    return name.size() >= length && name.compare(name.size() - length, length, suffix) == 0;
}

// Binds one material animation to its model the way the game does. Returns the entry
// result, or -2 when the loader rejects the file; pTargets/pMatched count its materials.
int bindMaterialAnimation(J3DModelData* data, const std::string& name, const Buffer& file, int* pTargets, int* pMatched) {
    J3DAnmBase* anm = J3DAnmLoaderDataBase::load(file.data());
    if (anm == nullptr) {
        return -2;
    }
    J3DMaterialTable& table = data->getMaterialTable();
    if (endsWith(name, ".btp")) {
        J3DAnmTexPattern* pattern = static_cast< J3DAnmTexPattern* >(anm);
        pattern->searchUpdateMaterialID(data);
        *pTargets = pattern->getUpdateMaterialNum();
        for (u16 i = 0; i < pattern->getUpdateMaterialNum(); i++) {
            *pMatched += pattern->isValidUpdateMaterialID(i);
        }
        return table.entryTexNoAnimator(pattern);
    }
    if (endsWith(name, ".bpk")) {
        J3DAnmColor* color = static_cast< J3DAnmColor* >(anm);
        color->searchUpdateMaterialID(data);
        *pTargets = color->getUpdateMaterialNum();
        for (u16 i = 0; i < color->getUpdateMaterialNum(); i++) {
            *pMatched += color->isValidUpdateMaterialID(i);
        }
        return table.entryMatColorAnimator(color);
    }
    if (endsWith(name, ".btk")) {
        J3DAnmTextureSRTKey* srt = static_cast< J3DAnmTextureSRTKey* >(anm);
        srt->searchUpdateMaterialID(data);
        *pTargets = srt->getUpdateMaterialNum();
        for (u16 i = 0; i < srt->getUpdateMaterialNum(); i++) {
            *pMatched += srt->isValidUpdateMaterialID(i);
        }
        const int rv = table.entryTexMtxAnimator(srt);
        // 4: the first call created missing texture matrices and stopped, as on the Wii.
        return rv == 4 ? table.entryTexMtxAnimator(srt) : rv;
    }
    J3DAnmTevRegKey* key = static_cast< J3DAnmTevRegKey* >(anm);
    key->searchUpdateMaterialID(data);
    *pTargets = key->getCRegUpdateMaterialNum() + key->getKRegUpdateMaterialNum();
    for (u16 i = 0; i < key->getCRegUpdateMaterialNum(); i++) {
        *pMatched += key->isValidCRegUpdateMaterialID(i);
    }
    for (u16 i = 0; i < key->getKRegUpdateMaterialNum(); i++) {
        *pMatched += key->isValidKRegUpdateMaterialID(i);
    }
    return table.entryTevRegAnimator(key);
}

// Every ObjectData archive with a BDL and material animations: each BTP/BRK/BPK/BTK binds to
// materials of that model. Each archive gets its own JKR heap, destroyed afterwards; file
// buffers are host allocations and results are plain counters, so nothing outlives it.
void testCorpusBinding(const std::filesystem::path& files) {
    const char* const cKinds[] = {".btp", ".brk", ".bpk", ".btk"};
    int bound[4] = {};
    int archives = 0;
    int failed = 0;
    int unmatched = 0;
    std::vector< std::filesystem::path > paths;
    {
        PetariNative::HostAllocationScope host;
        for (const auto& entry : std::filesystem::directory_iterator(files / "ObjectData")) {
            if (entry.path().extension() == ".arc") {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
    }
    for (const auto& path : paths) {
        std::unique_ptr< Archive > owned;
        {
            PetariNative::HostAllocationScope host;
            owned.reset(new Archive(path));
        }
        Archive& archive = *owned;
        int model = -1;
        bool animated = false;
        for (std::size_t i = 0; i < archive.names.size(); i++) {
            if (model < 0 && endsWith(archive.names[i], ".bdl")) {
                model = static_cast< int >(i);
            }
            for (const char* kind : cKinds) {
                animated |= endsWith(archive.names[i], kind);
            }
        }
        if (model < 0 || !animated) {
            PetariNative::HostAllocationScope host;
            owned.reset();
            continue;
        }
        archives++;

        JKRExpHeap* heap = JKRExpHeap::create(16 * 1024 * 1024, JKRHeap::sRootHeap, false);
        JKRHeap* previous = heap->becomeCurrentHeap();
        {
            J3DModelData* data = loadAnimatedModel(archive.files[model]);
            for (std::size_t i = 0; i < archive.names.size(); i++) {
                int kind = -1;
                for (int k = 0; k < 4; k++) {
                    if (endsWith(archive.names[i], cKinds[k])) {
                        kind = k;
                    }
                }
                if (kind < 0) {
                    continue;
                }
                int targets = 0;
                int matched = 0;
                const int rv = bindMaterialAnimation(data, archive.names[i], archive.files[i], &targets, &matched);
                if (rv != 0) {
                    failed++;
                    std::fprintf(stderr, "  %s:%s: entry result %d (%d of %d targets)\n", path.filename().c_str(), archive.names[i].c_str(), rv, matched,
                                 targets);
                } else if (matched == 0) {
                    unmatched++;
                    std::fprintf(stderr, "  %s:%s: none of %d targets is a material of the model\n", path.filename().c_str(), archive.names[i].c_str(),
                                 targets);
                } else {
                    bound[kind]++;
                }
            }
        }
        CHECK(heap->check());
        previous->becomeCurrentHeap();
        JKRHeap::destroy(heap);
        PetariNative::HostAllocationScope host;
        owned.reset();
    }
    std::printf("material animation corpus: %d archives; bound btp %d, brk %d, bpk %d, btk %d; %d failed, %d unmatched\n", archives, bound[0], bound[1],
                bound[2], bound[3], failed, unmatched);
    CHECK(failed == 0 && unmatched == 0);
    // The US disc: 524 archives, btp 351, brk 502, bpk 55, btk 379.
    CHECK(archives >= 500 && bound[0] >= 340 && bound[1] >= 490 && bound[2] >= 50 && bound[3] >= 370);
    CHECK(JKRHeap::sRootHeap->check());
}
}  // namespace

int main(int argc, char** argv) {
    OSInit();
    JKRExpHeap::createRoot(1, false);
    testAccessor();

    const char* assets = nullptr;
    const char* corpus = nullptr;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--assets") == 0) {
            assets = argv[i + 1];
        } else if (std::strcmp(argv[i], "--corpus") == 0) {
            corpus = argv[i + 1];
        }
    }
    if (assets != nullptr) {
        testEyePattern(assets);
        testClothesColor(assets);
        testColorAndTexMtx(assets);
    }
    if (corpus != nullptr) {
        testCorpusBinding(corpus);
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d material animation check(s) failed\n", sFailures, sChecks);
        return 1;
    }
    std::printf("j3d material animation tests passed (%d checks%s%s)\n", sChecks, assets ? ", with assets" : "", corpus ? ", with corpus" : "");
    return 0;
}

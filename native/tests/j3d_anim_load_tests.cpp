// J3D animation loading tests: real J3DAnm objects built through J3DAnmLoaderDataBase and
// evaluated over their frame range, so every table and key/value array the runtime reads is
// exercised (run under ASan to catch out-of-range reads).
//
// Usage: petari_j3d_anim_load_tests --assets GAME_FILES_DIR [--limit N]
// Links: petari_j3d (J3DGraphBase/Animator/Loader), src/JSystem/J3DGraphLoader/J3DAnmLoader.cpp,
// native/resource/j3d_animation.cpp, petari_kernel/core/resources.
#include "archive.hpp"
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphBase/J3DStruct.hpp>
#include <JSystem/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <petari/endian.hpp>
#include <petari/boot.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using Buffer = std::vector< std::uint8_t >;

static bool plausible(f32 value) {
    return std::isfinite(value) && std::fabs(value) < 1.0e7f;
}

// Evaluates the animation at several frames. Returns false with a reason on bad values.
static bool evaluate(J3DAnmBase* pAnm, const char** ppReason) {
    const s16 frameMax = pAnm->getFrameMax();
    const f32 frames[] = {0.0f, 0.5f, frameMax * 0.5f, static_cast< f32 >(frameMax > 0 ? frameMax - 1 : 0), static_cast< f32 >(frameMax)};
    for (f32 frame : frames) {
        pAnm->setFrame(frame);
        switch (pAnm->getKind()) {
        case 8: {  // J3DAnmTransformKey (bck)
            J3DAnmTransformKey* pTransform = static_cast< J3DAnmTransformKey* >(pAnm);
            for (u16 joint = 0; joint < pTransform->field_0x1e; joint++) {
                J3DTransformInfo info;
                pTransform->getTransform(joint, &info);
                if (!plausible(info.mScale.x) || !plausible(info.mScale.y) || !plausible(info.mScale.z) || !plausible(info.mTranslate.x) ||
                    !plausible(info.mTranslate.y) || !plausible(info.mTranslate.z)) {
                    *ppReason = "transform key produced implausible scale/translation";
                    return false;
                }
            }
            break;
        }
        case 4: {  // J3DAnmTextureSRTKey (btk)
            J3DAnmTextureSRTKey* pSRT = static_cast< J3DAnmTextureSRTKey* >(pAnm);
            for (u16 track = 0; track < pSRT->mTrackNum / 3; track++) {
                J3DTextureSRTInfo info;
                pSRT->getTransform(track, &info);
                if (!plausible(info.mScaleX) || !plausible(info.mScaleY) || !plausible(info.mTranslationX) || !plausible(info.mTranslationY)) {
                    *ppReason = "texture SRT key produced implausible values";
                    return false;
                }
            }
            break;
        }
        case 11: {  // J3DAnmColorKey (bpk)
            J3DAnmColorKey* pColor = static_cast< J3DAnmColorKey* >(pAnm);
            for (u16 i = 0; i < pColor->getUpdateMaterialNum(); i++) {
                GXColor color;
                pColor->getColor(i, &color);
            }
            break;
        }
        case 5: {  // J3DAnmTevRegKey (brk)
            J3DAnmTevRegKey* pTevReg = static_cast< J3DAnmTevRegKey* >(pAnm);
            for (u16 i = 0; i < pTevReg->getCRegUpdateMaterialNum(); i++) {
                GXColorS10 color;
                pTevReg->getTevColorReg(i, &color);
            }
            for (u16 i = 0; i < pTevReg->getKRegUpdateMaterialNum(); i++) {
                GXColor color;
                pTevReg->getTevKonstReg(i, &color);
            }
            break;
        }
        case 2: {  // J3DAnmTexPattern (btp)
            J3DAnmTexPattern* pPattern = static_cast< J3DAnmTexPattern* >(pAnm);
            for (u16 i = 0; i < pPattern->getUpdateMaterialNum(); i++) {
                u16 texNo = 0xFFFF;
                pPattern->getTexNo(i, &texNo);
                if (texNo >= 0x400) {
                    *ppReason = "texture pattern produced an implausible texture index";
                    return false;
                }
            }
            break;
        }
        case 6: {  // J3DAnmVisibilityFull (bva)
            J3DAnmVisibilityFull* pVisibility = static_cast< J3DAnmVisibilityFull* >(pAnm);
            for (u16 i = 0; i < pVisibility->mUpdateMaterialNum; i++) {
                u8 visible = 0xFF;
                pVisibility->getVisibility(i, &visible);
                if (visible > 1) {
                    *ppReason = "visibility produced a value other than 0/1";
                    return false;
                }
            }
            break;
        }
        default:
            *ppReason = "unexpected animation kind";
            return false;
        }
    }
    return true;
}

static bool readFile(const std::filesystem::path& path, Buffer* pOut) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    pOut->assign(std::istreambuf_iterator< char >(file), std::istreambuf_iterator< char >());
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--assets") != 0) {
        std::fprintf(stderr, "Usage: %s --assets GAME_FILES_DIR [--limit N]\n", argv[0]);
        return 2;
    }
    const std::size_t limit = argc >= 5 && std::strcmp(argv[3], "--limit") == 0 ? std::strtoul(argv[4], nullptr, 10) : ~std::size_t(0);

    OSInit();
    JKRExpHeap::createRoot(1, false);

    namespace fs = std::filesystem;
    std::vector< fs::path > archives;
    for (const char* pDir : {"ObjectData", "StageData"}) {
        const fs::path dir = fs::path(argv[2]) / pDir;
        if (fs::is_directory(dir)) {
            for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
                if (entry.path().extension() == ".arc") {
                    archives.push_back(entry.path());
                }
            }
        }
    }
    std::sort(archives.begin(), archives.end());

    std::map< std::string, std::size_t > loadedKinds;
    std::size_t loaded = 0, failed = 0;
    for (const fs::path& path : archives) {
        Buffer bytes;
        if (loaded >= limit || !readFile(path, &bytes)) {
            continue;
        }
        const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (std::size_t i = 0; i < archive.entries().size() && loaded < limit; i++) {
            const auto& entry = archive.entries()[i];
            if (entry.isDirectory()) {
                continue;
            }
            const Buffer file = archive.resourceData(i);
            if (file.size() < 0x20 || std::memcmp(file.data(), "J3D1", 4) != 0) {
                continue;
            }

            // Native operator new allocates from the current JKR heap on game threads, so the
            // window between becomeCurrentHeap and destroy must not create test-owned objects.
            const std::string where = path.filename().string() + ":" + entry.name;
            const std::string kind(reinterpret_cast< const char* >(&file[4]), 4);
            JKRExpHeap* pHeap = JKRExpHeap::create(4 * 1024 * 1024, JKRHeap::sRootHeap, false);
            JKRHeap* pPrevious = pHeap->becomeCurrentHeap();

            const char* pReason = nullptr;
            J3DAnmBase* pAnm = J3DAnmLoaderDataBase::load(file.data());
            const s16 frameMax = static_cast< s16 >(PetariNative::readU16BE(&file[0x20 + (std::memcmp(&file[0x20], "PAK1", 4) == 0 ? 0x0C : 0x0A)]));
            if (pAnm == nullptr) {
                pReason = "loader returned NULL";
            } else if (pAnm->getFrameMax() != frameMax) {
                pReason = "frame count differs from the file";
            } else {
                evaluate(pAnm, &pReason);
            }
            pPrevious->becomeCurrentHeap();
            JKRHeap::destroy(pHeap);
            loaded++;

            if (pReason != nullptr) {
                std::fprintf(stderr, "FAIL: %s: %s\n", where.c_str(), pReason);
                failed++;
            } else {
                loadedKinds[kind]++;
            }
        }
    }

    std::printf("J3D animations: %zu loaded into J3DAnm objects and evaluated, %zu failed (", loaded, failed);
    for (const auto& kind : loadedKinds) {
        std::printf(" %s %zu", kind.first.c_str(), kind.second);
    }
    std::printf(" )\n");
    if (failed != 0 || loaded == 0) {
        return 1;
    }
    std::puts("J3D animation load tests passed");
    return 0;
}

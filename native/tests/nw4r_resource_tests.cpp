// Native BRLYT/BRLAN normalization and BRFNT loading. Synthetic checks always run;
// with --assets <files dir>, every layout, animation and font in the disc's RARC
// archives is converted and checked against an independent big-endian reading.
#include "nw4r/lyt/common.h"
#include "nw4r/lyt/resources.h"
#include "nw4r/lyt/texMap.h"
#include "nw4r/lyt/animation.h"
#include "nw4r/lyt/layout.h"
#include <revolution/mem/allocator.h>
#include <revolution/mem/expHeap.h>
#include "nw4r/ut/ResFont.h"
#include <archive.hpp>
#include <petari/endian.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace nw4r;

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
using PetariNative::readU16BE;
using PetariNative::readU32BE;

void put16(std::vector< u8 >& b, size_t at, u32 v) {
    b[at] = static_cast< u8 >(v >> 8);
    b[at + 1] = static_cast< u8 >(v);
}

void put32(std::vector< u8 >& b, size_t at, u32 v) {
    put16(b, at, v >> 16);
    put16(b, at + 2, v);
}

void putF32(std::vector< u8 >& b, size_t at, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put32(b, at, bits);
}

f32 hostF32(const u8* p) {
    f32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// RLYT with lyt1, a pane and a text box whose string is "Hi".
std::vector< u8 > buildLayout() {
    const u32 panSize = sizeof(lyt::res::Pane);
    const u32 txtSize = sizeof(lyt::res::TextBox) + 8;
    std::vector< u8 > b(16 + 20 + panSize + txtSize, 0);
    std::memcpy(&b[0], "RLYT", 4);
    put16(b, 4, 0xFEFF);
    put16(b, 6, 0x000A);
    put32(b, 8, static_cast< u32 >(b.size()));
    put16(b, 12, 16);
    put16(b, 14, 3);

    std::memcpy(&b[16], "lyt1", 4);
    put32(b, 20, 20);
    putF32(b, 28, 640.0f);
    putF32(b, 32, 456.0f);

    size_t pan = 36;
    std::memcpy(&b[pan], "pan1", 4);
    put32(b, pan + 4, panSize);
    std::memcpy(&b[pan + 12], "RootPane", 8);
    putF32(b, pan + 36, 1.5f);
    putF32(b, pan + 60, 2.0f);
    putF32(b, pan + 68, 100.0f);

    size_t txt = pan + panSize;
    std::memcpy(&b[txt], "txt1", 4);
    put32(b, txt + 4, txtSize);
    put16(b, txt + 76, 8);
    put16(b, txt + 78, 6);
    put32(b, txt + 88, sizeof(lyt::res::TextBox));
    put32(b, txt + 92, 0x11223344);
    putF32(b, txt + 100, 24.0f);
    put16(b, txt + sizeof(lyt::res::TextBox), u'H');
    put16(b, txt + sizeof(lyt::res::TextBox) + 2, u'i');
    return b;
}

void testSyntheticLayout() {
    std::vector< u8 > file = buildLayout();
    const std::vector< u8 > original = file;
    CHECK(lyt::detail::NativeNormalizeResource(file.data(), lyt::res::FILESIGNATURE_RLYT));

    const lyt::res::BinaryFileHeader* header = reinterpret_cast< const lyt::res::BinaryFileHeader* >(file.data());
    CHECK(header->byteOrder == 0xFEFF && header->fileSize == file.size() && header->headerSize == 16 && header->dataBlocks == 3);
    CHECK(lyt::detail::TestFileHeader(*header, lyt::res::FILESIGNATURE_RLYT));
    CHECK(lyt::detail::GetSignatureInt(reinterpret_cast< const char* >(&file[16])) == lyt::res::DATABLOCKKIND_LAYOUT);

    const lyt::res::Layout* layout = reinterpret_cast< const lyt::res::Layout* >(&file[16]);
    CHECK(layout->blockHeader.size == 20 && layout->layoutSize.width == 640.0f && layout->layoutSize.height == 456.0f);

    const lyt::res::Pane* pane = reinterpret_cast< const lyt::res::Pane* >(&file[36]);
    CHECK(std::strcmp(pane->name, "RootPane") == 0 && pane->translate.x == 1.5f && pane->scale.x == 2.0f && pane->size.width == 100.0f);

    const lyt::res::TextBox* text = reinterpret_cast< const lyt::res::TextBox* >(&file[36 + sizeof(lyt::res::Pane)]);
    CHECK(text->textBufBytes == 8 && text->textStrBytes == 6 && text->textStrOffset == sizeof(lyt::res::TextBox));
    CHECK(text->textCols[0] == 0x11223344 && ut::Color(text->textCols[0]).r == 0x11 && ut::Color(text->textCols[0]).a == 0x44);
    CHECK(text->fontSize.width == 24.0f);
    const u16* string = reinterpret_cast< const u16* >(reinterpret_cast< const u8* >(text) + text->textStrOffset);
    CHECK(string[0] == u'H' && string[1] == u'i' && string[2] == 0);

    // Converted files are left alone.
    std::vector< u8 > converted = file;
    CHECK(lyt::detail::NativeNormalizeResource(file.data(), lyt::res::FILESIGNATURE_RLYT));
    CHECK(file == converted);

    // Malformed files are rejected without modification.
    std::vector< u8 > bad = original;
    put32(bad, 36 + sizeof(lyt::res::Pane) + 88, 0x7FFFFFF0);
    std::vector< u8 > badCopy = bad;
    CHECK(!lyt::detail::NativeNormalizeResource(bad.data(), lyt::res::FILESIGNATURE_RLYT));
    CHECK(bad == badCopy);
    CHECK(!lyt::detail::NativeNormalizeResource(bad.data(), lyt::res::FILESIGNATURE_RLAN));
}

// RLAN with one pai1 block: pane "P" with an RLPA Hermite target and an RLVI step target.
void testSyntheticAnimation() {
    std::vector< u8 > b(16 + 256, 0);
    std::memcpy(&b[0], "RLAN", 4);
    put16(b, 4, 0xFEFF);
    put16(b, 6, 0x000A);
    put16(b, 12, 16);
    put16(b, 14, 1);

    const size_t pai = 16;
    std::memcpy(&b[pai], "pai1", 4);
    put16(b, pai + 8, 60);
    put16(b, pai + 14, 1);
    put32(b, pai + 16, 20);
    put32(b, pai + 20, 24);  // content 0 at block + 24
    const size_t content = pai + 24;
    b[content] = 'P';
    b[content + 20] = 2;
    put32(b, content + 24, 32);  // info 0
    put32(b, content + 28, 80);  // info 1, after info 0 and its Hermite key
    const size_t info0 = content + 32;
    std::memcpy(&b[info0], "RLPA", 4);
    b[info0 + 4] = 1;
    put32(b, info0 + 8, 12);
    const size_t target0 = info0 + 12;
    b[target0 + 2] = 2;
    put16(b, target0 + 4, 1);
    put32(b, target0 + 8, 12);
    putF32(b, target0 + 12, 30.0f);
    putF32(b, target0 + 16, 5.0f);
    putF32(b, target0 + 20, -1.0f);
    const size_t info1 = content + 80;
    std::memcpy(&b[info1], "RLVI", 4);
    b[info1 + 4] = 1;
    put32(b, info1 + 8, 12);
    const size_t target1 = info1 + 12;
    b[target1 + 2] = 1;
    put16(b, target1 + 4, 1);
    put32(b, target1 + 8, 12);
    putF32(b, target1 + 12, 10.0f);
    put16(b, target1 + 16, 1);
    const u32 fileSize = static_cast< u32 >(target1 + 20);
    b.resize(fileSize);
    put32(b, 8, fileSize);
    put32(b, pai + 4, fileSize - pai);

    CHECK(lyt::detail::NativeNormalizeResource(b.data(), lyt::res::FILESIGNATURE_RLAN));
    const lyt::res::AnimationBlock* block = reinterpret_cast< const lyt::res::AnimationBlock* >(&b[pai]);
    CHECK(block->frameSize == 60 && block->animContNum == 1 && block->animContOffsetsOffset == 20);
    const lyt::res::AnimationInfo* anim0 = reinterpret_cast< const lyt::res::AnimationInfo* >(&b[info0]);
    CHECK(anim0->kind == lyt::res::ANIMATIONTYPE_RLPA);
    const lyt::res::AnimationTarget* t0 = reinterpret_cast< const lyt::res::AnimationTarget* >(&b[target0]);
    const lyt::res::HermiteKey* hermite = reinterpret_cast< const lyt::res::HermiteKey* >(&b[target0 + t0->keysOffset]);
    CHECK(t0->keyNum == 1 && hermite->frame == 30.0f && hermite->value == 5.0f && hermite->slope == -1.0f);
    const lyt::res::AnimationTarget* t1 = reinterpret_cast< const lyt::res::AnimationTarget* >(&b[target1]);
    const lyt::res::StepKey* step = reinterpret_cast< const lyt::res::StepKey* >(&b[target1 + t1->keysOffset]);
    CHECK(step->frame == 10.0f && step->value == 1);
}

// Checks a converted layout against the unconverted copy.
bool checkLayout(const std::vector< u8 >& before, const std::vector< u8 >& after) {
    const auto* header = reinterpret_cast< const lyt::res::BinaryFileHeader* >(after.data());
    bool ok = header->byteOrder == 0xFEFF && header->fileSize == readU32BE(&before[8]) && header->dataBlocks == readU16BE(&before[14]);
    u32 block = header->headerSize;
    for (u32 i = 0; i < header->dataBlocks && ok; i++) {
        const auto* blockHeader = reinterpret_cast< const lyt::res::DataBlockHeader* >(&after[block]);
        s32 kind = lyt::detail::GetSignatureInt(blockHeader->kind);
        ok &= blockHeader->size == readU32BE(&before[block + 4]);
        if (kind == lyt::res::DATABLOCKKIND_PANE || kind == lyt::res::DATABLOCKKIND_PICTURE || kind == lyt::res::DATABLOCKKIND_TEXTBOX ||
            kind == lyt::res::DATABLOCKKIND_WINDOW || kind == lyt::res::DATABLOCKKIND_BOUNDING) {
            const auto* pane = reinterpret_cast< const lyt::res::Pane* >(blockHeader);
            ok &= std::isfinite(pane->translate.x) && std::isfinite(pane->scale.y) && pane->size.width >= 0.0f;
            u32 bits = readU32BE(&before[block + 36]);
            f32 translateX;
            std::memcpy(&translateX, &bits, sizeof(translateX));
            ok &= pane->translate.x == translateX;
        }
        if (kind == lyt::res::DATABLOCKKIND_TEXTBOX) {
            const auto* text = reinterpret_cast< const lyt::res::TextBox* >(blockHeader);
            for (u32 unit = 0; unit < text->textStrBytes / 2u; unit++) {
                u16 host = *reinterpret_cast< const u16* >(&after[block + text->textStrOffset + unit * 2]);
                ok &= host == readU16BE(&before[block + text->textStrOffset + unit * 2]);
            }
        }
        block += blockHeader->size;
    }
    return ok;
}

bool checkAnimation(const std::vector< u8 >& after) {
    const auto* header = reinterpret_cast< const lyt::res::BinaryFileHeader* >(after.data());
    bool ok = header->byteOrder == 0xFEFF;
    u32 block = header->headerSize;
    for (u32 i = 0; i < header->dataBlocks && ok; i++) {
        const auto* blockHeader = reinterpret_cast< const lyt::res::DataBlockHeader* >(&after[block]);
        if (lyt::detail::GetSignatureInt(blockHeader->kind) == lyt::res::DATABLOCKKIND_PANEANIMINFO) {
            const auto* anim = reinterpret_cast< const lyt::res::AnimationBlock* >(blockHeader);
            const u32* contents = reinterpret_cast< const u32* >(&after[block + anim->animContOffsetsOffset]);
            for (u32 c = 0; c < anim->animContNum; c++) {
                const u8* content = &after[block + contents[c]];
                const u32* infos = reinterpret_cast< const u32* >(content + sizeof(lyt::res::AnimationContent));
                for (u32 j = 0; j < content[20]; j++) {
                    const auto* info = reinterpret_cast< const lyt::res::AnimationInfo* >(content + infos[j]);
                    ok &= info->kind == lyt::res::ANIMATIONTYPE_RLPA || info->kind == lyt::res::ANIMATIONTYPE_RLVI ||
                          info->kind == lyt::res::ANIMATIONTYPE_RLVC || info->kind == lyt::res::ANIMATIONTYPE_RLMC ||
                          info->kind == lyt::res::ANIMATIONTYPE_RLTS || info->kind == lyt::res::ANIMATIONTYPE_RLTP ||
                          info->kind == lyt::res::ANIMATIONTYPE_RLIM;
                    const u32* targets = reinterpret_cast< const u32* >(reinterpret_cast< const u8* >(info) + sizeof(*info));
                    for (u32 t = 0; t < info->num; t++) {
                        const auto* target = reinterpret_cast< const lyt::res::AnimationTarget* >(reinterpret_cast< const u8* >(info) + targets[t]);
                        const u8* keys = reinterpret_cast< const u8* >(target) + target->keysOffset;
                        for (u32 k = 0; k < target->keyNum; k++) {
                            // Every key starts with its frame number.
                            f32 frame = hostF32(keys + k * (target->curveType == 1 ? sizeof(lyt::res::StepKey) : sizeof(lyt::res::HermiteKey)));
                            // Keys may lie outside [0, frameSize] (real files use -4 and 40 with frameSize 21).
                            bool keyOk = std::isfinite(frame) && std::fabs(frame) < 100000.0f;
                            if (!keyOk && std::getenv("NW4R_TEST_VERBOSE")) {
                                std::fprintf(stderr, "    kind %.4s curve %u key %u frame %g frameSize %u\n", reinterpret_cast< const char* >(&info->kind), target->curveType, k,
                                             frame, anim->frameSize);
                            }
                            ok &= keyOk;
                        }
                    }
                }
            }
        }
        block += blockHeader->size;
    }
    return ok;
}

bool checkFont(std::vector< u8 >& file) {
    const std::vector< u8 > original = file;
    ut::ResFont font;
    if (!font.SetResource(file.data())) {
        return false;
    }

    // The native loader copies metadata; the caller's bytes are not changed.
    bool ok = file == original && font.GetHeight() > 0 && font.GetWidth() > 0;
    // Every game font, including the digits-only number font, has '0'.
    ok &= font.HasGlyph(u'0') && font.GetCharWidth(u'0') > 0;
    font.RemoveResource();
    return ok;
}

// Every image of a TPL through TexMap::ReplaceImage, compared with an independent parse.
bool checkTexturePalette(std::vector< u8 >& file, int* images) {
    if (file.size() < 12 || readU32BE(&file[0]) != 0x0020AF30) {
        return false;
    }
    const std::vector< u8 > original = file;
    u32 imageNum = readU32BE(&file[4]);
    u32 table = readU32BE(&file[8]);
    bool ok = imageNum > 0 && table + imageNum * 8 <= file.size();
    for (u32 i = 0; i < imageNum && ok; i++) {
        u32 imageOffset = readU32BE(&file[table + i * 8]);
        u32 paletteOffset = readU32BE(&file[table + i * 8 + 4]);
        ok &= imageOffset + 0x24 <= file.size();
        if (!ok) {
            break;
        }

        lyt::TexMap texMap;
        texMap.ReplaceImage(reinterpret_cast< TPLPalette* >(file.data()), i);
        const u8* image = &file[imageOffset];
        ok &= texMap.GetSize().width == readU16BE(image + 2) && texMap.GetSize().height == readU16BE(image);
        ok &= texMap.GetTexelFormat() == GXTexFmt(readU32BE(image + 4));
        ok &= texMap.mImage == &file[readU32BE(image + 8)] && readU32BE(image + 8) < file.size();
        if (paletteOffset != 0) {
            ok &= texMap.GetPalette() == &file[readU32BE(&file[paletteOffset + 8])] && texMap.GetPaletteEntryNum() == readU16BE(&file[paletteOffset]);
        } else {
            ok &= texMap.GetPalette() == nullptr && texMap.GetPaletteEntryNum() == 0;
        }
        (*images)++;
    }
    return ok && file == original;
}


// The Layout::CreateAnimTransform sequence (boot crash): allocate through the
// layout allocator, link into an AnimTransformList, then call a virtual. The
// list node must not overlap the vtable pointer.
bool checkAnimTransform(const std::vector< u8 >& converted, int* transforms) {
    lyt::AnimResource resource(converted.data());
    const lyt::res::AnimationBlock* block = resource.GetResourceBlock();
    if (block == nullptr) {
        return false;
    }
    if (block->fileNum != 0) {
        return true;  // Texture pattern animations need a resource accessor.
    }

    lyt::AnimTransformList list;
    lyt::AnimTransformBasic* transform = lyt::Layout::NewObj< lyt::AnimTransformBasic >();
    if (transform == nullptr) {
        return false;
    }
    list.PushBack(transform);
    transform->SetResource(block, nullptr);
    bool ok = transform->GetFrameSize() == block->frameSize && &*list.GetBeginIter() == transform;
    list.Erase(transform);
    lyt::Layout::DeleteObj(transform);
    (*transforms)++;
    return ok;
}

void testDisc(const std::filesystem::path& root) {
    int layouts = 0;
    int animations = 0;
    int fonts = 0;
    int tpls = 0;
    int tplImages = 0;
    int transforms = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (!item.is_regular_file() || item.path().extension() != ".arc") {
            continue;
        }

        std::ifstream stream(item.path(), std::ios::binary);
        std::vector< u8 > bytes((std::istreambuf_iterator< char >(stream)), std::istreambuf_iterator< char >());
        if (bytes.size() >= 4 && (std::memcmp(bytes.data(), "Yaz0", 4) == 0 || std::memcmp(bytes.data(), "Yay0", 4) == 0)) {
            bytes = PetariNative::Resource::decompress({bytes.data(), bytes.size()});
        }
        if (bytes.size() < 4 || std::memcmp(bytes.data(), "RARC", 4) != 0) {
            continue;
        }

        PetariNative::Resource::Archive archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
        for (size_t i = 0; i < archive.entries().size(); i++) {
            const auto& entry = archive.entries()[i];
            if (entry.isDirectory() || entry.name.size() < 6) {
                continue;
            }
            std::string extension = entry.name.substr(entry.name.find_last_of('.') + 1);
            if (extension != "brlyt" && extension != "brlan" && extension != "brfnt" && extension != "tpl") {
                continue;
            }

            std::vector< u8 > data = archive.resourceData(i);
            bool ok;
            if (extension == "brfnt") {
                ok = checkFont(data);
                fonts++;
            } else if (extension == "tpl") {
                ok = checkTexturePalette(data, &tplImages);
                tpls++;
            } else {
                const std::vector< u8 > before = data;
                u32 signature = extension == "brlyt" ? lyt::res::FILESIGNATURE_RLYT : lyt::res::FILESIGNATURE_RLAN;
                ok = lyt::detail::NativeNormalizeResource(data.data(), signature);
                if (!ok) {
                    std::fprintf(stderr, "  normalize rejected ");
                }
                std::vector< u8 > converted = data;
                ok = ok && lyt::detail::NativeNormalizeResource(data.data(), signature) && data == converted;
                ok = ok && (extension == "brlyt" ? checkLayout(before, data) : checkAnimation(data) && checkAnimTransform(data, &transforms));
                (extension == "brlyt" ? layouts : animations)++;
            }

            CHECK(ok);
            if (!ok) {
                std::fprintf(stderr, "  failed: %s in %s\n", entry.name.c_str(), item.path().c_str());
            }
        }
    }

    std::printf("disc: %d layouts, %d animations (%d through AnimTransformBasic), %d fonts, %d TPL files (%d images)\n", layouts, animations, transforms, fonts, tpls, tplImages);
    CHECK(layouts > 0 && animations > 0 && fonts > 0 && tpls > 0);
}
}  // namespace

extern "C" void OSInit();

int main(int argc, char** argv) {
    OSInit();
    static std::vector< u8 > layoutHeapMemory(4 << 20);
    static MEMAllocator allocator;
    MEMInitAllocatorForExpHeap(&allocator, MEMCreateExpHeapEx(layoutHeapMemory.data(), static_cast< u32 >(layoutHeapMemory.size()), 0), 8);
    lyt::Layout::mspAllocator = &allocator;

    testSyntheticLayout();
    testSyntheticAnimation();

    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--assets") == 0) {
            testDisc(argv[i + 1]);
        }
    }

    if (sFailures != 0) {
        std::fprintf(stderr, "%d of %d nw4r resource check(s) failed\n", sFailures, sChecks);
        return 1;
    }

    std::printf("nw4r resource tests passed (%d checks)\n", sChecks);
    return 0;
}

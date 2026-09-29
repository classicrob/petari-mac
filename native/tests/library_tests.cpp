// Focused checks for native replacements in JSystem and the MSL/runtime shims.
#include "JSystem/J3DGraphBase/J3DDrawBuffer.hpp"
#include "JSystem/J3DGraphBase/J3DStruct.hpp"
#include "JSystem/J3DGraphBase/J3DTransform.hpp"
#include "JSystem/JAudio2/JAISound.hpp"
#include "JSystem/JAudio2/JASChannel.hpp"
#include "JSystem/JAudio2/JAUAudible.hpp"
#include "JSystem/JUtility/JUTNameTab.hpp"
#include "JSystem/JUtility/JUTTexture.hpp"
#include <extras.h>
#include <petari/host_allocation.hpp>
#include <petari/utf16.h>
#include <revolution/mtx.h>
#include <runtime.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

static int sFailures;

#define CHECK(expr)                                                                                                                                  \
    do {                                                                                                                                             \
        if (!(expr)) {                                                                                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                                                          \
            sFailures++;                                                                                                                             \
        }                                                                                                                                            \
    } while (0)

static bool near(f32 a, f32 b, f32 tolerance = 1.0e-5f) {
    return std::fabs(a - b) <= tolerance * (1.0f + std::fabs(b));
}

static void testCvtDblUsll() {
    CHECK(__cvt_dbl_usll(0.0) == 0);
    CHECK(__cvt_dbl_usll(0.99) == 0);
    CHECK(__cvt_dbl_usll(-0.99) == 0);
    CHECK(__cvt_dbl_usll(1.0) == 1);
    CHECK(__cvt_dbl_usll(3.75) == 3);
    CHECK(__cvt_dbl_usll(-3.75) == static_cast< uint64_t >(-3));
    CHECK(__cvt_dbl_usll(121500000.0 * 0.5) == 60750000);
    CHECK(__cvt_dbl_usll(4503599627370497.0) == 4503599627370497ull);
    CHECK(__cvt_dbl_usll(9223372036854774784.0) == 9223372036854774784ull);
    CHECK(__cvt_dbl_usll(9223372036854775808.0) == 0x7FFFFFFFFFFFFFFFull);
    CHECK(__cvt_dbl_usll(-1.0e30) == 0x8000000000000000ull);
    CHECK(__cvt_dbl_usll(INFINITY) == 0x7FFFFFFFFFFFFFFFull);
    CHECK(__cvt_dbl_usll(-INFINITY) == 0x8000000000000000ull);
}

static void testStricmp() {
    CHECK(stricmp("StageData", "stagedata") == 0);
    CHECK(stricmp("abc", "ABD") == -1);
    CHECK(stricmp("abd", "ABC") == 1);
    CHECK(stricmp("ab", "abc") == -1);
    CHECK(stricmp("", "") == 0);
    // '[' sorts after 'Z' but before 'z'; only A-Z are lowered.
    CHECK(stricmp("[", "z") == -1);
    CHECK(stricmp("[", "Z") == -1);
}

static void testHostAllocationScope() {
    // Threads are host threads until registered.
    CHECK(!PetariNative::isGameAllocationThread());
    CHECK(PetariNative::isHostAllocationActive());

    PetariNative::setGameAllocationThread(true);
    CHECK(PetariNative::isGameAllocationThread());
    CHECK(!PetariNative::isHostAllocationActive());
    {
        PetariNative::HostAllocationScope outer;
        CHECK(PetariNative::isHostAllocationActive());
        {
            PetariNative::HostAllocationScope inner;
            CHECK(PetariNative::isHostAllocationActive());
        }
        CHECK(PetariNative::isHostAllocationActive());
    }
    CHECK(!PetariNative::isHostAllocationActive());

    // Registration is per thread.
    bool otherThreadIsHost = false;
    std::thread([&] { otherThreadIsHost = PetariNative::isHostAllocationActive() && !PetariNative::isGameAllocationThread(); }).join();
    CHECK(otherThreadIsHost);

    PetariNative::setGameAllocationThread(false);
    CHECK(PetariNative::isHostAllocationActive());
}

static void makeAffine(Mtx m, f32 seed) {
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            m[row][col] = std::sin(seed + row * 4 + col) * 2.0f + (row == col ? 3.0f : 0.0f);
        }
    }
}

static void testInverseTranspose() {
    Mtx src;
    makeAffine(src, 0.25f);
    Mtx33 invXpose;
    J3DPSCalcInverseTranspose(src, invXpose);

    // (A^-1)^T * A^T == I, so row i of invXpose dotted with row j of A is delta(i, j).
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            f32 dot = invXpose[i][0] * src[j][0] + invXpose[i][1] * src[j][1] + invXpose[i][2] * src[j][2];
            CHECK(near(dot, i == j ? 1.0f : 0.0f, 1.0e-4f));
        }
    }

    Mtx singular = {{1, 2, 3, 0}, {2, 4, 6, 0}, {0, 0, 1, 0}};
    Mtx33 untouched;
    std::memset(untouched, 0x5A, sizeof(untouched));
    Mtx33 expected;
    std::memcpy(expected, untouched, sizeof(expected));
    J3DPSCalcInverseTranspose(singular, untouched);
    CHECK(std::memcmp(untouched, expected, sizeof(expected)) == 0);
}

static void testArrayConcat() {
    Mtx a;
    makeAffine(a, 1.5f);
    Mtx b[3];
    Mtx ab[3];
    for (int i = 0; i < 3; i++) {
        makeAffine(b[i], 4.0f + i);
    }

    J3DPSMtxArrayConcat(a, b[0], ab[0], 3);
    for (int i = 0; i < 3; i++) {
        Mtx expected;
        PSMTXConcat(a, b[i], expected);
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 4; col++) {
                CHECK(near(ab[i][row][col], expected[row][col]));
            }
        }
    }
}

static void testProjConcat() {
    Mtx m;
    makeAffine(m, 2.0f);
    Mtx44 proj;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            proj[row][col] = std::cos(row * 4.0f + col);
        }
    }

    Mtx dst;
    J3DMtxProjConcat(m, proj, dst);
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 4; col++) {
            f32 expected = 0.0f;
            for (int k = 0; k < 4; k++) {
                expected += m[row][k] * proj[k][col];
            }
            CHECK(near(dst[row][col], expected));
        }
    }
}

static void testScaleNrm() {
    Mtx m;
    makeAffine(m, 3.0f);
    Mtx original;
    std::memcpy(original, m, sizeof(Mtx));
    Vec scale = {2.0f, -0.5f, 4.0f};
    J3DScaleNrmMtx(m, scale);
    for (int row = 0; row < 3; row++) {
        CHECK(m[row][0] == original[row][0] * 2.0f);
        CHECK(m[row][1] == original[row][1] * -0.5f);
        CHECK(m[row][2] == original[row][2] * 4.0f);
        CHECK(m[row][3] == original[row][3]);
    }

    Mtx33 m33 = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    J3DScaleNrmMtx33(m33, scale);
    CHECK(m33[1][0] == 8.0f && m33[1][1] == -2.5f && m33[1][2] == 24.0f);
}

static void testCalcZValue() {
    Mtx m;
    makeAffine(m, 5.0f);
    Vec v = {1.5f, -2.0f, 0.75f};
    f32 expected = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];
    CHECK(near(J3DCalcZValue(m, v), expected));
}

static void testStructAssignment() {
    J3DTransformInfo src = {{1, 2, 3}, {4, 5, 6}, 0x1234, {7, 8, 9}};
    J3DTransformInfo dst = {{0, 0, 0}, {0, 0, 0}, 0xBEEF, {0, 0, 0}};
    dst = src;
    CHECK(dst.mScale.y == 2 && dst.mRotation.z == 6 && dst.mTranslate.z == 9);
    CHECK(dst._12 == 0xBEEF);

    J3DTextureSRTInfo srt = {1.5f, 2.5f, -300, 3.5f, 4.5f};
    J3DTextureSRTInfo copy = {};
    copy = srt;
    CHECK(copy.mScaleX == 1.5f && copy.mScaleY == 2.5f && copy.mRotation == -300);
    CHECK(copy.mTranslationX == 3.5f && copy.mTranslationY == 4.5f);
}

static bool equals16(const char16_t* actual, const char16_t* expected) {
    return petari_utf16_wcscmp(actual, expected) == 0;
}

static void testUtf16Strings() {
    CHECK(petari_utf16_wcslen(u"") == 0);
    CHECK(petari_utf16_wcslen(u"Marioマ") == 6);
    CHECK(petari_utf16_wcscmp(u"abc", u"abd") == -1);
    CHECK(petari_utf16_wcscmp(u"￿", u"a") == 0xFFFF - 'a');
    char16_t padded[6] = {1, 1, 1, 1, 1, 1};
    petari_utf16_wcsncpy(padded, u"ab", 5);
    CHECK(padded[0] == u'a' && padded[1] == u'b' && padded[2] == 0 && padded[4] == 0 && padded[5] == 1);
    const char16_t* text = u"a%b";
    CHECK(petari_utf16_wcschr(text, u'%') == text + 1);
    CHECK(petari_utf16_wcschr(text, 0) == text + 3);
    CHECK(petari_utf16_wcschr(text, u'z') == nullptr);
}

static void testUtf16Format() {
    char16_t buffer[256];

    // Formats used by CustomTagProcessor, ReplaceTagProcessor and NWC24Messenger.
    CHECK(petari_utf16_swprintf(buffer, 256, u"%03d", 7) == 3 && equals16(buffer, u"007"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%06d", -42) == 6 && equals16(buffer, u"-00042"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%d/%d", 12, 120) == 6 && equals16(buffer, u"12/120"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%d", -2147483647 - 1) == 11 && equals16(buffer, u"-2147483648"));

    // GalaxyMapGalaxyPlain and PowerStarList, including the in-place append.
    CHECK(petari_utf16_swprintf(buffer, 256, u"%ls ", u"ギャ") == 3 && equals16(buffer, u"ギャ "));
    petari_utf16_swprintf(buffer, 256, u"Star");
    CHECK(petari_utf16_swprintf(buffer, 256, u"%ls%s%ls", buffer, "\n", u"Race") == 9 && equals16(buffer, u"Star\nRace"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"") == 0 && buffer[0] == 0);

    // Flags, widths, lengths, and MSL's error handling.
    CHECK(petari_utf16_swprintf(buffer, 256, u"[%-4d|%+d|% d|%#x|%#o|%X]", 5, 5, 5, 255, 8, 0xBEEF) == 26);
    CHECK(equals16(buffer, u"[5   |+5| 5|0xff|010|BEEF]"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%*d|%.3d|%hd|%lld", 4, 1, 2, 70000, 1234567890123ll) > 0);
    CHECK(equals16(buffer, u"   1|002|4464|1234567890123"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%c%lc%%", 'A', u'マ') == 3 && equals16(buffer, u"Aマ%"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%.2f|%e", 3.14159, 1.5) > 0 && equals16(buffer, u"3.14|1.500000e+00"));
    CHECK(petari_utf16_swprintf(buffer, 256, u"%.0d", 0) == 0 && buffer[0] == 0);
    int position = 0;
    CHECK(petari_utf16_swprintf(buffer, 256, u"ab%ncd", &position) == 4 && position == 2);
    CHECK(petari_utf16_swprintf(buffer, 256, u"x%qy%d", 3) == 6 && equals16(buffer, u"x%qy%d"));

    // Output that does not fit is truncated and reported as -1.
    char16_t small[4];
    CHECK(petari_utf16_swprintf(small, 4, u"%d", 12345) == -1 && equals16(small, u"123"));
    CHECK(petari_utf16_swprintf(small, 4, u"abcd") == -1 && equals16(small, u"abc"));
    CHECK(petari_utf16_swprintf(small, 4, u"abc") == 3 && equals16(small, u"abc"));
    CHECK(petari_utf16_swprintf(small, 4, u"ab") == 2 && equals16(small, u"ab"));
}

static void putBE16(u8* p, u32 v) {
    p[0] = static_cast< u8 >(v >> 8);
    p[1] = static_cast< u8 >(v);
}

static void putBE32(u8* p, u32 v) {
    putBE16(p, v >> 16);
    putBE16(p + 2, v);
}

static void testResTIMGNormalize() {
    alignas(4) u8 bti[0x40] = {};
    bti[0x00] = 0x0E;  // CMPR
    putBE16(bti + 0x02, 256);
    putBE16(bti + 0x04, 128);
    bti[0x06] = 1;
    bti[0x08] = 1;
    putBE16(bti + 0x0A, 4);
    putBE32(bti + 0x0C, 0x20);
    putBE16(bti + 0x1A, static_cast< u16 >(-5));
    putBE32(bti + 0x1C, 0x28);
    ResTIMG* timg = reinterpret_cast< ResTIMG* >(bti);
    CHECK(JUTNativeNormalizeResTIMG(timg, sizeof(bti)));
    CHECK(timg->mFormat == 0x0E && timg->mWidth == 256 && timg->mHeight == 128 && timg->mWrapS == 1);
    CHECK(timg->mPaletteNum == 4 && timg->mPaletteDataOffset == 0x20 && timg->mLodBias == -5 && timg->mImageDataOffset == 0x28);

    alignas(4) u8 bad[0x20] = {};
    putBE32(bad + 0x1C, 0x40);
    u8 before[sizeof(bad)];
    std::memcpy(before, bad, sizeof(bad));
    CHECK(!JUTNativeNormalizeResTIMG(reinterpret_cast< ResTIMG* >(bad), sizeof(bad)));
    CHECK(std::memcmp(before, bad, sizeof(bad)) == 0);
    CHECK(!JUTNativeNormalizeResTIMG(reinterpret_cast< ResTIMG* >(bad), 0x10));
}

static u16 nameKey(const char* name) {
    u32 key = 0;
    while (*name) {
        key = key * 3 + *name++;
    }
    return static_cast< u16 >(key);
}

static void testResNTABNormalize() {
    alignas(4) u8 table[0x30] = {};
    const char* names[] = {"Body", "HeadJoint"};
    putBE16(table, 2);
    putBE16(table + 2, 0xFFFF);
    u32 stringOffset = 4 + 2 * 4;
    for (int i = 0; i < 2; i++) {
        putBE16(table + 4 + i * 4, nameKey(names[i]));
        putBE16(table + 6 + i * 4, stringOffset);
        std::strcpy(reinterpret_cast< char* >(table + stringOffset), names[i]);
        stringOffset += std::strlen(names[i]) + 1;
    }

    ResNTAB* ntab = reinterpret_cast< ResNTAB* >(table);
    CHECK(JUTNativeNormalizeResNTAB(ntab, stringOffset));
    CHECK(ntab->mEntryNum == 2 && ntab->_2 == 0xFFFF);
    JUTNameTab nameTab(ntab);
    CHECK(nameTab.getIndex("HeadJoint") == 1);
    CHECK(nameTab.getIndex("Body") == 0);
    CHECK(nameTab.getIndex("Tail") == -1);
    CHECK(std::strcmp(nameTab.getName(1), "HeadJoint") == 0);

    alignas(4) u8 unterminated[12] = {};
    putBE16(unterminated, 1);
    putBE16(unterminated + 6, 8);
    std::memcpy(unterminated + 8, "abcd", 4);
    CHECK(!JUTNativeNormalizeResNTAB(reinterpret_cast< ResNTAB* >(unterminated), sizeof(unterminated)));
    alignas(4) u8 tooMany[8] = {};
    putBE16(tooMany, 5);
    CHECK(!JUTNativeNormalizeResNTAB(reinterpret_cast< ResNTAB* >(tooMany), sizeof(tooMany)));
}

// JASTrack sets mix configurations numerically and JASChannel reads bitfields.
static void testMixConfigBitfields() {
    JASChannel::MixConfig config;
    config.whole = 0x352;
    CHECK(config.upper == 3 && config.lower0 == 5 && config.lower1 == 2);
    config.upper = 11;
    CHECK(config.whole == 0xB52);
    CHECK(sizeof(JASChannel::MixConfig) == 2);
}

// Sound IDs are built numerically and read through byte fields (and vice versa).
static void testSoundIDFields() {
    JAISoundID id(0x01230456);
    CHECK(id.getSectionID() == 0x01 && id.getGroupID() == 0x23 && id.getWaveID() == 0x0456);
    CHECK(id.mID.info.type.value == 0x0123);
    JAISoundID built(2, 0x45, 0x1234);
    CHECK(static_cast< u32 >(built) == 0x02451234);
    built.setWaveID(0xBEEF);
    CHECK(static_cast< u32 >(built) == 0x0245BEEF);
}

// Audible parameters come from the sound table's u16 switch word.
static void testAudibleParam() {
    JAUAudibleParam param(0xA530, 0xFFFF);
    CHECK(param.getDoppler() == 0xA && param.calcDoppler());
    CHECK(param.getVolDistBit() == (1u << 3) && param.calcVolume() == ((0xA530 >> 10) & 1));
    CHECK(static_cast< u32 >(param) == 0xA530FFFF);
    JAUAudibleParam fromWord(0x0010BEEFu);
    CHECK(fromWord.getAudibleSw() == 0x0010 && fromWord.getDoppler() == 0 && !fromWord.calcDoppler() && fromWord.getVolDistBit() == 2);
}

int main() {
    testCvtDblUsll();
    testStricmp();
    testHostAllocationScope();
    testInverseTranspose();
    testArrayConcat();
    testProjConcat();
    testScaleNrm();
    testCalcZValue();
    testStructAssignment();
    testUtf16Strings();
    testUtf16Format();
    testResTIMGNormalize();
    testResNTABNormalize();
    testMixConfigBitfields();
    testSoundIDFields();
    testAudibleParam();

    if (sFailures != 0) {
        std::fprintf(stderr, "%d library check(s) failed\n", sFailures);
        return 1;
    }

    std::puts("library tests passed");
    return 0;
}

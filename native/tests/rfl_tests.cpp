// Tests for native RVLFaceLib support: the packed Mii record codec, RFL's
// own default database and validation through it, and RFL_Res.dat
// validation and byte-order conversion.
//
//        petari_rfl_tests                   synthetic data and the built-in defaults
//        petari_rfl_tests --assets FILES    also the disc's ObjectData/MiiFaceDatabase.arc

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "RVLFaceLibInternal.h"
#include "archive.hpp"
#include "rfl_native.h"
#include "rfl_test_resource.hpp"

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;
using namespace RflTestResource;

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

// --- Codec: bit positions -------------------------------------------------

// Each field's word and CodeWarrior bit position (shift of its least
// significant bit, first field at the top), transcribed from RFLi_Types.h.
struct FieldCase {
    const char* name;
    unsigned offset;
    unsigned shift;
    unsigned width;
    void (*set)(RFLiCharData*, unsigned);
    unsigned (*get)(const RFLiCharData*);
};

#define FIELD(f, off, sh, w)                                                          \
    FieldCase {                                                                       \
        #f, off, sh, w, [](RFLiCharData* d, unsigned v) { d->f = v; },                \
            [](const RFLiCharData* d) { return static_cast<unsigned>(d->f); }         \
    }

const FieldCase kFields[] = {
    FIELD(padding0, 0x00, 15, 1),      FIELD(sex, 0x00, 14, 1),           FIELD(birthMonth, 0x00, 10, 4),
    FIELD(birthDay, 0x00, 5, 5),       FIELD(favoriteColor, 0x00, 1, 4),  FIELD(favorite, 0x00, 0, 1),
    FIELD(faceType, 0x20, 13, 3),      FIELD(faceColor, 0x20, 10, 3),     FIELD(faceTex, 0x20, 6, 4),
    FIELD(padding2, 0x20, 3, 3),       FIELD(localonly, 0x20, 2, 1),      FIELD(type, 0x20, 0, 2),
    FIELD(hairType, 0x22, 9, 7),       FIELD(hairColor, 0x22, 6, 3),      FIELD(hairFlip, 0x22, 5, 1),
    FIELD(padding3, 0x22, 0, 5),       FIELD(eyebrowType, 0x24, 11, 5),   FIELD(eyebrowRotate, 0x24, 6, 5),
    FIELD(padding4, 0x24, 0, 6),       FIELD(eyebrowColor, 0x26, 13, 3),  FIELD(eyebrowScale, 0x26, 9, 4),
    FIELD(eyebrowY, 0x26, 4, 5),       FIELD(eyebrowX, 0x26, 0, 4),       FIELD(eyeType, 0x28, 10, 6),
    FIELD(eyeRotate, 0x28, 5, 5),      FIELD(eyeY, 0x28, 0, 5),           FIELD(eyeColor, 0x2A, 13, 3),
    FIELD(eyeScale, 0x2A, 9, 4),       FIELD(eyeX, 0x2A, 5, 4),           FIELD(padding5, 0x2A, 0, 5),
    FIELD(noseType, 0x2C, 12, 4),      FIELD(noseScale, 0x2C, 8, 4),      FIELD(noseY, 0x2C, 3, 5),
    FIELD(padding6, 0x2C, 0, 3),       FIELD(mouthType, 0x2E, 11, 5),     FIELD(mouthColor, 0x2E, 9, 2),
    FIELD(mouthScale, 0x2E, 5, 4),     FIELD(mouthY, 0x2E, 0, 5),         FIELD(glassType, 0x30, 12, 4),
    FIELD(glassColor, 0x30, 9, 3),     FIELD(glassScale, 0x30, 5, 4),     FIELD(glassY, 0x30, 0, 5),
    FIELD(mustacheType, 0x32, 14, 2),  FIELD(beardType, 0x32, 12, 2),     FIELD(beardColor, 0x32, 9, 3),
    FIELD(beardScale, 0x32, 5, 4),     FIELD(beardY, 0x32, 0, 5),         FIELD(moleType, 0x34, 15, 1),
    FIELD(moleScale, 0x34, 11, 4),     FIELD(moleY, 0x34, 6, 5),          FIELD(moleX, 0x34, 1, 5),
    FIELD(padding8, 0x34, 0, 1),
};
#undef FIELD

void testFieldPositions() {
    check(sizeof(kFields) / sizeof(kFields[0]) == 52, "every RFLiCharData bitfield is listed");
    for (const FieldCase& f : kFields) {
        const unsigned max = (1u << f.width) - 1;
        RFLiCharData host;
        std::memset(&host, 0, sizeof(host));
        f.set(&host, max);
        std::uint8_t wire[RFLi_NATIVE_CHAR_DATA_SIZE];
        RFLiNativeEncodeCharData(&host, wire);
        Bytes expected(RFLi_NATIVE_CHAR_DATA_SIZE, 0);
        const unsigned word = max << f.shift;
        expected[f.offset] = static_cast<std::uint8_t>(word >> 8);
        expected[f.offset + 1] = static_cast<std::uint8_t>(word);
        check(std::memcmp(wire, expected.data(), sizeof(wire)) == 0, std::string("encode places ") + f.name);

        RFLiCharData decoded;
        RFLiNativeDecodeCharData(expected.data(), &decoded);
        bool only = f.get(&decoded) == max;
        for (const FieldCase& other : kFields) {
            if (&other != &f && other.get(&decoded) != 0) {
                only = false;
            }
        }
        check(only, std::string("decode reads only ") + f.name);
    }

    // Hidden records: a 9-bit birthPadding replaces the birthday.
    RFLiHiddenCharData hidden;
    std::memset(&hidden, 0, sizeof(hidden));
    hidden.birthPadding = 0x1FF;
    hidden.favoriteColor = 0xA;
    std::uint8_t wire[RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE];
    RFLiNativeEncodeHiddenCharData(&hidden, wire);
    check(be16(wire) == ((0x1FFu << 5) | (0xAu << 1)), "hidden record first word");
    RFLiHiddenCharData back;
    RFLiNativeDecodeHiddenCharData(wire, &back);
    check(back.birthPadding == 0x1FF && back.favoriteColor == 0xA && back.sex == 0, "hidden record decodes");
}

// --- Codec: names and round trips -----------------------------------------

std::uint32_t gSeed = 0x2468ACE1u;
std::uint8_t nextByte() {
    gSeed = gSeed * 1664525u + 1013904223u;
    return static_cast<std::uint8_t>(gSeed >> 24);
}

void testRoundTrips() {
    for (int i = 0; i < 2000; ++i) {
        std::uint8_t wire[RFLi_NATIVE_CHAR_DATA_SIZE];
        for (auto& b : wire) {
            b = nextByte();
        }
        RFLiCharData host;
        RFLiNativeDecodeCharData(wire, &host);
        std::uint8_t again[RFLi_NATIVE_CHAR_DATA_SIZE];
        RFLiNativeEncodeCharData(&host, again);
        check(std::memcmp(wire, again, sizeof(wire)) == 0, "stored record round-trips bit for bit");

        std::uint8_t hiddenWire[RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE];
        std::memcpy(hiddenWire, wire, sizeof(hiddenWire));
        RFLiHiddenCharData hidden;
        RFLiNativeDecodeHiddenCharData(hiddenWire, &hidden);
        std::uint8_t hiddenAgain[RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE];
        RFLiNativeEncodeHiddenCharData(&hidden, hiddenAgain);
        check(std::memcmp(hiddenWire, hiddenAgain, sizeof(hiddenWire)) == 0, "hidden record round-trips");
    }

    RFLiCharData host;
    std::memset(&host, 0, sizeof(host));
    const char16_t name[] = u"Miiéマ";
    const char16_t creator[] = u"Petari";
    std::memcpy(host.name, name, sizeof(name) - 2);
    std::memcpy(host.creatorName, creator, sizeof(creator) - 2);
    host.height = 0x7F;
    host.createID.data[0] = 0x80;
    host.createID.data[7] = 0x42;
    std::uint8_t wire[RFLi_NATIVE_CHAR_DATA_SIZE];
    RFLiNativeEncodeCharData(&host, wire);
    check(be16(wire + 0x02) == u'M' && be16(wire + 0x0A) == 0x30DE && be16(wire + 0x36) == u'P', "names stored UTF-16BE");
    check(wire[0x16] == 0x7F && wire[0x18] == 0x80 && wire[0x1F] == 0x42, "height and create ID stored as bytes");
}

// --- RFL's default database through the codec ------------------------------

// The first built-in record ("no name"), as stored in RFL_DefaultDatabase.c,
// decoded by hand for comparison.
const std::uint8_t kDefault0[RFLi_NATIVE_CHAR_DATA_SIZE] = {
    0x00, 0x08, 0x00, 0x6E, 0x00, 0x6F, 0x00, 0x20, 0x00, 0x6E, 0x00, 0x61, 0x00, 0x6D, 0x00, 0x65, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x40, 0x40, 0x80, 0x00, 0x00, 0x00, 0xEC, 0xFF, 0x82, 0xD2, 0x10, 0x04, 0x88, 0x00, 0x31, 0x80,
    0x08, 0xA2, 0x08, 0x8C, 0x08, 0x58, 0x14, 0x4A, 0xB8, 0x8D, 0x00, 0x8A, 0x00, 0x8A, 0x25, 0x04, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

bool matchesDefault0(const RFLiCharInfo& info) {
    const char16_t expected[] = u"no name";
    return std::memcmp(info.personal.name, expected, sizeof(expected)) == 0 && info.personal.color == 4 &&
           info.body.height == 64 && info.body.build == 64 && info.nose.type == 1 && info.nose.y == 9 &&
           info.mouth.type == 23 && info.mouth.y == 13 && info.mole.x == 2 && info.mole.y == 20 &&
           info.glass.scale == 4 && info.glass.y == 10 && info.beard.scale == 4 && info.beard.y == 10 &&
           info.createID.data[0] == 0x80 && info.createID.data[7] == 0xD2;
}

void testDefaultDatabase() {
    for (u16 i = 0; i < 6; ++i) {
        RFLiCharInfo info;
        std::memset(&info, 0xCD, sizeof(info));
        RFLiGetDefaultData(&info, i);
        check(RFLiCheckValidInfo(&info), "RFL accepts built-in default Mii " + std::to_string(i));
        check(info.personal.name[0] != 0 && info.personal.name[RFL_NAME_LEN] == 0, "default Mii has a terminated name");
    }
    RFLiCharInfo first;
    RFLiGetDefaultData(&first, 0);
    check(matchesDefault0(first), "default Mii 0 decodes to its hand-decoded fields");

    // Control: reading the stored bytes as host bitfields, as an unported
    // RFL would, does not give the Mii.
    RFLiCharInfo wrong;
    RFLiConvertRaw2Info(reinterpret_cast<const RFLiCharData*>(kDefault0), &wrong);
    check(!matchesDefault0(wrong), "stored bytes read in place are not the Mii");
    RFLiCharData decoded;
    RFLiNativeDecodeCharData(kDefault0, &decoded);
    RFLiCharInfo viaCodec;
    RFLiConvertRaw2Info(&decoded, &viaCodec);
    check(matchesDefault0(viaCodec), "the codec is what makes it right");
}

// --- Create IDs ----------------------------------------------------------------

void testCreateIDs() {
    // Stored byte order, tested by RFL as big-endian words, at an odd offset
    // as inside RFLiCharInfo.
    struct {
        std::uint8_t pad;
        RFLCreateID id;
    } unaligned;
    const std::uint8_t normal[8] = {0x80, 0x00, 0x00, 0x00, 0xEC, 0xFF, 0x82, 0xD2};  // default Mii 0
    std::memcpy(unaligned.id.data, normal, 8);
    check(RFLiIsValidID(&unaligned.id) && !RFLiIsSpecialID(&unaligned.id) && !RFLiIsTemporaryID(&unaligned.id),
          "an ordinary Mii ID (top bit set) is valid, not special, not temporary");
    const std::uint8_t special[8] = {0x00, 0x00, 0x00, 0x80, 0x01, 0x02, 0x03, 0x04};
    std::memcpy(unaligned.id.data, special, 8);
    check(RFLiIsSpecialID(&unaligned.id), "top bit clear in the first big-endian word: special");
    const std::uint8_t temporary[8] = {0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(unaligned.id.data, temporary, 8);
    check(RFLiIsTemporaryID(&unaligned.id) && !RFLiIsSpecialID(&unaligned.id), "0x20000000: temporary");
    std::memset(unaligned.id.data, 0, 8);
    check(!RFLiIsValidID(&unaligned.id), "all-zero ID invalid");
    unaligned.id.data[7] = 1;
    check(RFLiIsValidID(&unaligned.id), "any nonzero byte makes an ID valid");

    RFLCreateID a;
    RFLCreateID b;
    std::memcpy(a.data, normal, 8);
    std::memcpy(b.data, normal, 8);
    check(RFLiIsSameID(&a, &b), "same IDs match");
    b.data[5] ^= 1;
    check(!RFLiIsSameID(&a, &b), "different IDs do not");

    RFLiCharInfo info;
    std::memset(&info, 0xEE, sizeof(info));
    RFLiSetTemporaryID(&info);
    check(info.createID.data[0] == 0x20 && info.createID.data[3] == 0 && info.createID.data[7] == 0 &&
              RFLiIsTemporaryID(&info.createID),
          "RFLiSetTemporaryID stores 0x20000000 big-endian");
}

// --- Resource: synthetic ----------------------------------------------------

std::string validate(const Bytes& b) {
    char error[160] = "";
    return RFLiNativeValidateResource(b.data(), static_cast<u32>(b.size()), error, sizeof(error)) ? "" : error;
}

void rejects(Bytes b, const std::string& expected, const std::string& label) {
    const std::string error = validate(b);
    check(!error.empty() && error.find(expected) != std::string::npos, label + " (" + error + ")");
}

void testSyntheticResource() {
    const Resource good = syntheticResource();
    check(validate(good.bytes).empty(), "synthetic resource valid: " + validate(good.bytes));
    check(!RFLiNativeValidateResource(nullptr, 0, nullptr, 0), "null resource rejected without an error buffer");

    Bytes b = good.bytes;
    b[1] = 17;
    rejects(b, "17 archives", "archive count");
    rejects(Bytes(good.bytes.begin(), good.bytes.begin() + 40), "shorter than its header", "truncated header");
    rejects(Bytes(good.bytes.begin(), good.bytes.end() - 1), "outside its section", "truncated last file");
    b = good.bytes;
    set32(b, 8, 0x10);
    rejects(b, "out of order", "section before the header");
    b = good.bytes;
    b[good.fileStart[Eye] - 12 - 2] ^= 1;  // Eye biggestSize
    rejects(b, "header says", "biggest size");
    b = good.bytes;
    b[good.fileStart[Eye]] = 9;  // texture format
    rejects(b, "unsupported texture format 9", "texture format");
    b = good.bytes;
    b[good.fileStart[Eye] + 3] = 64;  // width 64 needs more than stored
    rejects(b, "image bytes", "texture image too small");
    b = good.bytes;
    b[good.fileStart[Eye] + 8] = 1;
    rejects(b, "palette", "palette texture");
    b = good.bytes;
    b[good.fileStart[Nose]] = 'N';
    rejects(b, "shape tag", "shape tag");
    b = good.bytes;
    // Nose: tag 4, count 2, 3 positions, count 2, 1 normal, primitives 1, header 2, then indices.
    b[good.fileStart[Nose] + 4 + 2 + 18 + 2 + 6 + 1 + 2] = 3;
    rejects(b, "index out of range", "position index");
    b = good.bytes;
    b[good.fileStart[Nose] + 4 + 2 + 18 + 2 + 6 + 1 + 1] = 0x91;
    rejects(b, "GX opcode 0x91", "primitive opcode");
    b = good.bytes;
    b[good.fileStart[Nose] + 4 + 2 + 18 + 2 + 6] = 2;  // two primitives, one present
    rejects(b, "truncated", "primitive list");

    // Byte-order conversion of copies.
    Bytes eye(good.bytes.begin() + good.fileStart[Eye], good.bytes.begin() + good.fileStart[Eye] + 0x20 + 2560);
    RFLiNativeTextureHeaderToHost(reinterpret_cast<RFLiTexture*>(eye.data()));
    const auto* tex = reinterpret_cast<const RFLiTexture*>(eye.data());
    check(tex->format == 5 && tex->width == 38 && tex->height == 32 && tex->imageOfs == 0x20 && tex->lodBias == -2 &&
              tex->mipmapLevel == 1,
          "texture header in host order");
    check(eye[0x20] == 0x5A, "texture image untouched");

    Bytes face = shape(Faceline);
    check(RFLiNativeShapeToHost(face.data(), static_cast<u32>(face.size())), "faceline shape converts");
    float transforms[9];
    std::memcpy(transforms, face.data() + 4, sizeof(transforms));
    check(transforms[0] == 1.0f && transforms[5] == 0.25f && transforms[7] == -200.0f, "faceline transforms are host floats");
    std::uint16_t count;
    std::int16_t coords[3];
    std::memcpy(&count, face.data() + 40, 2);
    std::memcpy(coords, face.data() + 42, 6);
    check(count == 3 && coords[0] == -100 && coords[1] == 200 && coords[2] == 300, "positions are host s16");
    std::int16_t texCoord[2];
    std::memcpy(texCoord, face.data() + 40 + 2 + 18 + 2 + 6 + 2 + 4, 4);
    check(texCoord[0] == -2 && texCoord[1] == -32768, "texture coordinates are host s16");
    Bytes badShape = shape(Nose);
    badShape[0] = 'x';
    const Bytes before = badShape;
    check(!RFLiNativeShapeToHost(badShape.data(), static_cast<u32>(badShape.size())) && badShape == before,
          "an invalid shape is refused unchanged");
}

// --- Resource: the disc ------------------------------------------------------

void testDiscResource(const std::string& files) {
    const std::string path = files + "/ObjectData/MiiFaceDatabase.arc";
    std::ifstream in(path, std::ios::binary);
    check(in.good(), "open " + path);
    const Bytes arc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto archive = PetariNative::Resource::Archive::parse({arc.data(), arc.size()});
    Bytes res;
    for (std::size_t i = 0; i < archive.entries().size(); ++i) {
        const auto& entry = archive.entries()[i];
        std::string lower = entry.name;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (!entry.isDirectory() && lower == "rfl_res.dat") {
            res = archive.resourceData(i);
        }
    }
    check(res.size() == 686372, "RFL_Res.dat found in MiiFaceDatabase.arc (" + std::to_string(res.size()) + " bytes)");
    check(validate(res).empty(), "disc RFL_Res.dat validates: " + validate(res));
    check(be16(res.data() + 2) == 0x039D, "resource version 0x039D");

    // Every file converts; texture sizes match RFL's own formulas.
    unsigned textures = 0;
    unsigned shapes = 0;
    unsigned emptyShapes = 0;
    for (int a = 0; a < 18; ++a) {
        const std::uint32_t section = RFLiNativeLoadU32(res.data() + 4 + a * 4);
        const unsigned count = be16(res.data() + section);
        const std::size_t table = section + 4;
        const std::size_t data = table + 4 * (count + 1);
        for (unsigned f = 0; f < count; ++f) {
            const std::uint32_t begin = RFLiNativeLoadU32(res.data() + table + f * 4);
            const std::uint32_t end = RFLiNativeLoadU32(res.data() + table + f * 4 + 4);
            Bytes copy(res.begin() + data + begin, res.begin() + data + end);
            if (kIsTexture[a]) {
                RFLiNativeTextureHeaderToHost(reinterpret_cast<RFLiTexture*>(copy.data()));
                const auto* tex = reinterpret_cast<const RFLiTexture*>(copy.data());
                check(tex->imageOfs == 0x20 && tex->width > 0 && tex->height > 0 && tex->mipmapLevel == 1,
                      "disc texture header " + std::to_string(a) + ":" + std::to_string(f));
                ++textures;
            } else {
                check(RFLiNativeShapeToHost(copy.data(), static_cast<u32>(copy.size())),
                      "disc shape converts " + std::to_string(a) + ":" + std::to_string(f));
                std::uint16_t positions;
                std::memcpy(&positions, copy.data() + (a == Faceline ? 40 : 4), 2);
                emptyShapes += positions == 0 ? 1 : 0;
                ++shapes;
            }
        }
    }
    check(textures == 50 + 24 + 12 + 10 + 2 + 25 + 4 + 12 + 72 && shapes == 4 + 8 + 72 + 1 + 72 + 8 + 12 + 12 + 72,
          "all 211 textures and 261 shapes (" + std::to_string(textures) + ", " + std::to_string(shapes) + ")");
    check(emptyShapes > 0, "the disc has empty parts (" + std::to_string(emptyShapes) + ")");

    // Damage to the real file is caught.
    Bytes damaged = res;
    const std::uint32_t hairSection = RFLiNativeLoadU32(res.data() + 4 + Hair * 4);
    damaged[hairSection + 4 + 4 * 10 + 1] ^= 0x40;  // a Hair file offset
    check(!validate(damaged).empty(), "damaged disc resource rejected: " + validate(damaged));
}

}  // namespace

int main(int argc, char** argv) {
    testFieldPositions();
    testRoundTrips();
    testDefaultDatabase();
    testCreateIDs();
    testSyntheticResource();
    if (argc == 3 && std::strcmp(argv[1], "--assets") == 0) {
        testDiscResource(argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--assets GAME_FILES_DIR]\n", argv[0]);
        return 2;
    }
    std::printf("native RFL tests passed (%d checks)\n", checks);
    return 0;
}

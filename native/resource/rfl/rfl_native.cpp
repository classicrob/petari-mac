#include "rfl_native.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

static_assert(sizeof(RFLiCharData) == RFLi_NATIVE_CHAR_DATA_SIZE, "RFLiCharData layout");
static_assert(sizeof(RFLiHiddenCharData) == RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE, "RFLiHiddenCharData layout");
static_assert(offsetof(RFLiCharData, name) == 0x02 && offsetof(RFLiCharData, height) == 0x16 &&
                  offsetof(RFLiCharData, createID) == 0x18 && offsetof(RFLiCharData, creatorName) == 0x36,
              "RFLiCharData offsets");
static_assert(offsetof(RFLiHiddenCharData, name) == 0x02 && offsetof(RFLiHiddenCharData, createID) == 0x18,
              "RFLiHiddenCharData offsets");
static_assert(sizeof(RFLiTexture) == 0x20, "RFLiTexture layout");
// Public opaque structs that RFL casts to its internal ones. The game
// allocates RFLCharModel itself (MiiFaceParts), so the public size must hold
// the native internal struct.
static_assert(sizeof(RFLCharModel) == sizeof(RFLiCharModel) && alignof(RFLCharModel) >= alignof(RFLiCharModel),
              "RFLCharModel must hold RFLiCharModel");
static_assert(sizeof(RFLMiddleDB) == sizeof(RFLiMiddleDB) && alignof(RFLMiddleDB) >= alignof(RFLiMiddleDB),
              "RFLMiddleDB must hold RFLiMiddleDB");

namespace {

// A 16-bit word of bitfields: its offset and field widths in declaration
// order (RFLi_Types.h). Every word's widths sum to 16.
struct BitWord {
    unsigned offset;
    unsigned char widths[8];
};

constexpr BitWord kCharWords[] = {
    {0x00, {1, 1, 4, 5, 4, 1}},     // padding0 sex birthMonth birthDay favoriteColor favorite
    {0x20, {3, 3, 4, 3, 1, 2}},     // faceType faceColor faceTex padding2 localonly type
    {0x22, {7, 3, 1, 5}},           // hairType hairColor hairFlip padding3
    {0x24, {5, 5, 6}},              // eyebrowType eyebrowRotate padding4
    {0x26, {3, 4, 5, 4}},           // eyebrowColor eyebrowScale eyebrowY eyebrowX
    {0x28, {6, 5, 5}},              // eyeType eyeRotate eyeY
    {0x2A, {3, 4, 4, 5}},           // eyeColor eyeScale eyeX padding5
    {0x2C, {4, 4, 5, 3}},           // noseType noseScale noseY padding6
    {0x2E, {5, 2, 4, 5}},           // mouthType mouthColor mouthScale mouthY
    {0x30, {4, 3, 4, 5}},           // glassType glassColor glassScale glassY
    {0x32, {2, 2, 3, 4, 5}},        // mustacheType beardType beardColor beardScale beardY
    {0x34, {1, 4, 5, 5, 1}},        // moleType moleScale moleY moleX padding8
};

// Hidden records differ only in the first word (no birthday).
constexpr unsigned char kHiddenFirstWord[8] = {1, 1, 9, 4, 1};  // padding0 sex birthPadding favoriteColor favorite

// CodeWarrior (big-endian) allocates the first field at the most significant
// bit; clang on arm64 at the least significant.
std::uint16_t wireToHostWord(std::uint16_t wire, const unsigned char* widths) {
    std::uint16_t host = 0;
    unsigned wireShift = 16;
    unsigned hostShift = 0;
    for (int i = 0; i < 8 && widths[i] != 0; ++i) {
        const unsigned width = widths[i];
        wireShift -= width;
        const std::uint16_t value = (wire >> wireShift) & ((1u << width) - 1);
        host |= static_cast<std::uint16_t>(value << hostShift);
        hostShift += width;
    }
    return host;
}

std::uint16_t hostToWireWord(std::uint16_t host, const unsigned char* widths) {
    std::uint16_t wire = 0;
    unsigned wireShift = 16;
    unsigned hostShift = 0;
    for (int i = 0; i < 8 && widths[i] != 0; ++i) {
        const unsigned width = widths[i];
        wireShift -= width;
        const std::uint16_t value = (host >> hostShift) & ((1u << width) - 1);
        wire |= static_cast<std::uint16_t>(value << wireShift);
        hostShift += width;
    }
    return wire;
}

void storeBE16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
}

std::uint16_t hostWord(const void* base, unsigned offset) {
    std::uint16_t value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, 2);
    return value;
}

void setHostWord(void* base, unsigned offset, std::uint16_t value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, 2);
}

// Byte ranges copied verbatim: height, build, createID (bytes on both sides).
void decodeRecord(const std::uint8_t* wire, void* out, unsigned size, const unsigned char* firstWord, bool creator) {
    std::memcpy(out, wire, size);  // bytes; words are overwritten below
    const unsigned char* widths0 = firstWord;
    setHostWord(out, 0x00, wireToHostWord(RFLiNativeLoadU16(wire), widths0));
    for (std::size_t w = 1; w < sizeof(kCharWords) / sizeof(kCharWords[0]); ++w) {
        const BitWord& word = kCharWords[w];
        setHostWord(out, word.offset, wireToHostWord(RFLiNativeLoadU16(wire + word.offset), word.widths));
    }
    auto* bytes = static_cast<std::uint8_t*>(out);
    for (unsigned i = 0; i < RFL_NAME_LEN; ++i) {
        const std::uint16_t c = RFLiNativeLoadU16(wire + 0x02 + i * 2);
        std::memcpy(bytes + 0x02 + i * 2, &c, 2);
    }
    if (creator) {
        for (unsigned i = 0; i < RFL_CREATOR_LEN; ++i) {
            const std::uint16_t c = RFLiNativeLoadU16(wire + 0x36 + i * 2);
            std::memcpy(bytes + 0x36 + i * 2, &c, 2);
        }
    }
}

void encodeRecord(const void* in, std::uint8_t* wire, unsigned size, const unsigned char* firstWord, bool creator) {
    std::memcpy(wire, in, size);
    storeBE16(wire, hostToWireWord(hostWord(in, 0x00), firstWord));
    for (std::size_t w = 1; w < sizeof(kCharWords) / sizeof(kCharWords[0]); ++w) {
        const BitWord& word = kCharWords[w];
        storeBE16(wire + word.offset, hostToWireWord(hostWord(in, word.offset), word.widths));
    }
    for (unsigned i = 0; i < RFL_NAME_LEN; ++i) {
        storeBE16(wire + 0x02 + i * 2, hostWord(in, 0x02 + i * 2));
    }
    if (creator) {
        for (unsigned i = 0; i < RFL_CREATOR_LEN; ++i) {
            storeBE16(wire + 0x36 + i * 2, hostWord(in, 0x36 + i * 2));
        }
    }
}

// --- Resource validation ---

enum class Kind { Texture, Shape };

struct ArchiveInfo {
    const char* name;
    Kind kind;
    std::uint32_t tag;       // shapes: RFLiInitShapeRes's csHeader
    bool texCoords;          // shapes with texture coordinates
    bool facelineExtras;     // three Vec after the tag
};

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return (std::uint32_t(std::uint8_t(s[0])) << 24) | (std::uint32_t(std::uint8_t(s[1])) << 16) |
           (std::uint32_t(std::uint8_t(s[2])) << 8) | std::uint32_t(std::uint8_t(s[3]));
}

// In RFLiArcID order.
const ArchiveInfo kArchives[RFLi_NATIVE_RESOURCE_ARCHIVES] = {
    {"Beard", Kind::Shape, fourcc("berd"), false, false},
    {"Eye", Kind::Texture, 0, false, false},
    {"Eyebrow", Kind::Texture, 0, false, false},
    {"Faceline", Kind::Shape, fourcc("face"), true, true},
    {"FaceTex", Kind::Texture, 0, false, false},
    {"ForeHead", Kind::Shape, fourcc("frhd"), false, false},
    {"Glass", Kind::Shape, fourcc("glas"), true, false},
    {"GlassTex", Kind::Texture, 0, false, false},
    {"Hair", Kind::Shape, fourcc("hair"), false, false},
    {"Mask", Kind::Shape, fourcc("mask"), true, false},
    {"Mole", Kind::Texture, 0, false, false},
    {"Mouth", Kind::Texture, 0, false, false},
    {"Mustache", Kind::Texture, 0, false, false},
    {"Nose", Kind::Shape, fourcc("nose"), false, false},
    {"Nline", Kind::Shape, fourcc("nsln"), true, false},
    {"NlineTex", Kind::Texture, 0, false, false},
    {"Cap", Kind::Shape, fourcc("cap_"), true, false},
    {"CapTex", Kind::Texture, 0, false, false},
};

class Failure {
public:
    Failure(char* buffer, u32 size) : mBuffer(buffer), mSize(size) {}
    template <class... Args>
    BOOL operator()(const char* format, Args... args) const {
        if (mBuffer != nullptr && mSize > 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
            std::snprintf(mBuffer, mSize, format, args...);  // formats are literals at every call site
#pragma clang diagnostic pop
        }
        return FALSE;
    }

private:
    char* mBuffer;
    u32 mSize;
};

// GX texture storage: tile width, tile height, bytes per tile.
bool tileGeometry(unsigned format, unsigned* tw, unsigned* th, unsigned* bytes) {
    switch (format) {
    case 0x0: *tw = 8; *th = 8; *bytes = 32; return true;   // I4
    case 0x1: *tw = 8; *th = 4; *bytes = 32; return true;   // I8
    case 0x2: *tw = 8; *th = 4; *bytes = 32; return true;   // IA4
    case 0x3: *tw = 4; *th = 4; *bytes = 32; return true;   // IA8
    case 0x4: *tw = 4; *th = 4; *bytes = 32; return true;   // RGB565
    case 0x5: *tw = 4; *th = 4; *bytes = 32; return true;   // RGB5A3
    case 0x6: *tw = 4; *th = 4; *bytes = 64; return true;   // RGBA8
    case 0xE: *tw = 8; *th = 8; *bytes = 32; return true;   // CMPR
    default: return false;
    }
}

BOOL validateTexture(const std::uint8_t* p, std::uint32_t size, const char* arc, unsigned file, const Failure& fail) {
    if (size < sizeof(RFLiTexture)) {
        return fail("%s[%u]: texture shorter than its header (%u bytes)", arc, file, size);
    }
    const unsigned format = p[0];
    const unsigned width = RFLiNativeLoadU16(p + 2);
    const unsigned height = RFLiNativeLoadU16(p + 4);
    const unsigned indexTexture = p[8];
    const unsigned mipmaps = p[0x18];
    const std::uint32_t imageOffset = RFLiNativeLoadU32(p + 0x1C);
    unsigned tw, th, tileBytes;
    if (!tileGeometry(format, &tw, &th, &tileBytes)) {
        return fail("%s[%u]: unsupported texture format %u", arc, file, format);
    }
    if (indexTexture != 0) {
        return fail("%s[%u]: palette textures are not used by RFL", arc, file);
    }
    if (width == 0 || height == 0 || width > 1024 || height > 1024 || mipmaps == 0 || mipmaps > 11) {
        return fail("%s[%u]: texture %ux%u with %u levels", arc, file, width, height, mipmaps);
    }
    if (imageOffset < sizeof(RFLiTexture) || imageOffset > size) {
        return fail("%s[%u]: image offset 0x%x outside the texture", arc, file, imageOffset);
    }
    std::uint64_t bytes = 0;
    unsigned w = width;
    unsigned h = height;
    for (unsigned level = 0; level < mipmaps; ++level) {
        bytes += std::uint64_t((w + tw - 1) / tw) * ((h + th - 1) / th) * tileBytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (imageOffset + bytes > size) {
        return fail("%s[%u]: %ux%u format %u needs %llu image bytes, %u available", arc, file, width, height, format,
                    static_cast<unsigned long long>(bytes), size - imageOffset);
    }
    return TRUE;
}

BOOL validateShape(const std::uint8_t* p, std::uint32_t size, const ArchiveInfo& info, unsigned file, const Failure& fail) {
    std::uint32_t at = 0;
    auto need = [&](std::uint32_t bytes) { return at + bytes <= size; };
    if (!need(6)) {
        return fail("%s[%u]: shape shorter than its header (%u bytes)", info.name, file, size);
    }
    if (RFLiNativeLoadU32(p) != info.tag) {
        return fail("%s[%u]: shape tag 0x%08x", info.name, file, RFLiNativeLoadU32(p));
    }
    at = 4;
    if (info.facelineExtras) {
        if (!need(36)) {
            return fail("%s[%u]: faceline transforms truncated", info.name, file);
        }
        at += 36;
    }
    if (!need(2)) {
        return fail("%s[%u]: position count truncated", info.name, file);
    }
    const unsigned positions = RFLiNativeLoadU16(p + at);
    at += 2;
    if (positions == 0) {
        return TRUE;  // an empty part (RFLiInitShapeRes stops here)
    }
    if (!need(positions * 6 + 2)) {
        return fail("%s[%u]: %u positions truncated", info.name, file, positions);
    }
    at += positions * 6;
    const unsigned normals = RFLiNativeLoadU16(p + at);
    at += 2;
    if (!need(normals * 6)) {
        return fail("%s[%u]: %u normals truncated", info.name, file, normals);
    }
    at += normals * 6;
    unsigned texCoords = 0;
    if (info.texCoords) {
        if (!need(2)) {
            return fail("%s[%u]: texture coordinate count truncated", info.name, file);
        }
        texCoords = RFLiNativeLoadU16(p + at);
        at += 2;
        if (!need(texCoords * 4)) {
            return fail("%s[%u]: %u texture coordinates truncated", info.name, file, texCoords);
        }
        at += texCoords * 4;
    }
    if (!need(1)) {
        return fail("%s[%u]: primitive count truncated", info.name, file);
    }
    const unsigned primitives = p[at++];
    const unsigned stride = info.texCoords ? 3 : 2;
    for (unsigned i = 0; i < primitives; ++i) {
        if (!need(2)) {
            return fail("%s[%u]: primitive %u header truncated", info.name, file, i);
        }
        const unsigned vertices = p[at];
        const unsigned primitive = p[at + 1];
        at += 2;
        if ((primitive & 0x07) != 0 || primitive < 0x80 || primitive > 0xB8) {
            return fail("%s[%u]: primitive %u has GX opcode 0x%02x", info.name, file, i, primitive);
        }
        if (!need(vertices * stride)) {
            return fail("%s[%u]: primitive %u indices truncated", info.name, file, i);
        }
        for (unsigned v = 0; v < vertices; ++v) {
            const std::uint8_t* index = p + at + v * stride;
            if (index[0] >= positions || index[1] >= normals || (info.texCoords && index[2] >= texCoords)) {
                return fail("%s[%u]: primitive %u vertex %u index out of range", info.name, file, i, v);
            }
        }
        at += vertices * stride;
    }
    return TRUE;
}

}  // namespace

extern "C" {

void RFLiNativeDecodeCharData(const void* wire, RFLiCharData* out) {
    decodeRecord(static_cast<const std::uint8_t*>(wire), out, RFLi_NATIVE_CHAR_DATA_SIZE, kCharWords[0].widths, true);
}

void RFLiNativeEncodeCharData(const RFLiCharData* in, void* wire) {
    encodeRecord(in, static_cast<std::uint8_t*>(wire), RFLi_NATIVE_CHAR_DATA_SIZE, kCharWords[0].widths, true);
}

void RFLiNativeDecodeHiddenCharData(const void* wire, RFLiHiddenCharData* out) {
    decodeRecord(static_cast<const std::uint8_t*>(wire), out, RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE, kHiddenFirstWord, false);
}

void RFLiNativeEncodeHiddenCharData(const RFLiHiddenCharData* in, void* wire) {
    encodeRecord(in, static_cast<std::uint8_t*>(wire), RFLi_NATIVE_HIDDEN_CHAR_DATA_SIZE, kHiddenFirstWord, false);
}

BOOL RFLiNativeValidateResource(const void* res, u32 size, char* error, u32 errorSize) {
    const Failure fail(error, errorSize);
    const auto* p = static_cast<const std::uint8_t*>(res);
    if (p == nullptr) {
        return fail("no resource");
    }
    const std::uint32_t headerSize = 4 + 4 * RFLi_NATIVE_RESOURCE_ARCHIVES;
    if (size < headerSize) {
        return fail("resource shorter than its header (%u bytes)", size);
    }
    if (RFLiNativeLoadU16(p) != RFLi_NATIVE_RESOURCE_ARCHIVES) {
        return fail("resource lists %u archives, RFL expects %u", RFLiNativeLoadU16(p), RFLi_NATIVE_RESOURCE_ARCHIVES);
    }
    std::uint32_t previousEnd = headerSize;
    for (unsigned a = 0; a < RFLi_NATIVE_RESOURCE_ARCHIVES; ++a) {
        const ArchiveInfo& info = kArchives[a];
        const std::uint32_t section = RFLiNativeLoadU32(p + 4 + a * 4);
        const std::uint32_t limit = a + 1 < RFLi_NATIVE_RESOURCE_ARCHIVES ? RFLiNativeLoadU32(p + 8 + a * 4) : size;
        if (section < previousEnd || limit < section || limit > size) {
            return fail("%s: section 0x%x..0x%x out of order or outside the resource", info.name, section, limit);
        }
        if (limit - section < 4) {
            return fail("%s: section header truncated", info.name);
        }
        const unsigned files = RFLiNativeLoadU16(p + section);
        const unsigned biggest = RFLiNativeLoadU16(p + section + 2);
        const std::uint32_t table = section + 4;
        const std::uint64_t dataStart = std::uint64_t(table) + 4u * (files + 1);
        if (files == 0 || dataStart > limit) {
            return fail("%s: %u files; offset table does not fit", info.name, files);
        }
        unsigned largest = 0;
        for (unsigned f = 0; f < files; ++f) {
            const std::uint32_t begin = RFLiNativeLoadU32(p + table + f * 4);
            const std::uint32_t end = RFLiNativeLoadU32(p + table + f * 4 + 4);
            if ((f == 0 && begin != 0) || end < begin || dataStart + end > limit) {
                return fail("%s[%u]: file 0x%x..0x%x outside its section", info.name, f, begin, end);
            }
            const std::uint32_t length = end - begin;
            largest = length > largest ? length : largest;
            const std::uint8_t* file = p + dataStart + begin;
            const BOOL ok = info.kind == Kind::Texture ? validateTexture(file, length, info.name, f, fail)
                                                       : validateShape(file, length, info, f, fail);
            if (!ok) {
                return FALSE;
            }
        }
        if (largest != biggest) {
            return fail("%s: largest file is %u bytes, header says %u", info.name, largest, biggest);
        }
        previousEnd = limit;
    }
    return TRUE;
}

BOOL RFLiNativeShapeToHost(void* shape, u32 size) {
    auto* p = static_cast<std::uint8_t*>(shape);
    if (p == nullptr || size < 4) {
        return FALSE;
    }
    const std::uint32_t tag = RFLiNativeLoadU32(p);
    const ArchiveInfo* info = nullptr;
    for (const ArchiveInfo& candidate : kArchives) {
        if (candidate.kind == Kind::Shape && candidate.tag == tag) {
            info = &candidate;
        }
    }
    if (info == nullptr || !validateShape(p, size, *info, 0, Failure(nullptr, 0))) {
        return FALSE;
    }
    auto swap16 = [&](std::uint32_t at) {
        const std::uint16_t v = RFLiNativeLoadU16(p + at);
        std::memcpy(p + at, &v, 2);
    };
    auto swapArray = [&](std::uint32_t at, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            swap16(at + i * 2);
        }
    };
    std::uint32_t at = 4;
    if (info->facelineExtras) {
        for (int i = 0; i < 9; ++i) {
            const std::uint32_t v = RFLiNativeLoadU32(p + at + i * 4);
            std::memcpy(p + at + i * 4, &v, 4);
        }
        at += 36;
    }
    const std::uint16_t positions = RFLiNativeLoadU16(p + at);
    swap16(at);
    at += 2;
    if (positions == 0) {
        return TRUE;
    }
    swapArray(at, positions * 3u);
    at += positions * 6u;
    const std::uint16_t normals = RFLiNativeLoadU16(p + at);
    swap16(at);
    at += 2;
    swapArray(at, normals * 3u);
    at += normals * 6u;
    if (info->texCoords) {
        const std::uint16_t texCoords = RFLiNativeLoadU16(p + at);
        swap16(at);
        at += 2;
        swapArray(at, texCoords * 2u);
    }
    return TRUE;
}

void RFLiNativeTextureHeaderToHost(RFLiTexture* texture) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(texture);
    RFLiTexture host;
    std::memcpy(&host, texture, sizeof(host));  // byte fields
    host.width = RFLiNativeLoadU16(p + 0x02);
    host.height = RFLiNativeLoadU16(p + 0x04);
    host.numColors = RFLiNativeLoadU16(p + 0x0A);
    host.paletteOfs = RFLiNativeLoadU32(p + 0x0C);
    host.lodBias = RFLiNativeLoadS16(p + 0x1A);
    host.imageOfs = RFLiNativeLoadU32(p + 0x1C);
    std::memcpy(texture, &host, sizeof(host));
}

}  // extern "C"

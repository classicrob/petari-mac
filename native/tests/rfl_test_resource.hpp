#pragma once
// A small valid RFL_Res.dat for tests: every archive holds one real part and
// one empty or small one, in the disc's layout (big-endian).

#include <cstdint>
#include <cstring>
#include <vector>

namespace RflTestResource {

using Bytes = std::vector<std::uint8_t>;

inline void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void put32(Bytes& b, std::uint32_t v) {
    put16(b, static_cast<std::uint16_t>(v >> 16));
    put16(b, static_cast<std::uint16_t>(v));
}
inline void set32(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 24);
    b[at + 1] = static_cast<std::uint8_t>(v >> 16);
    b[at + 2] = static_cast<std::uint8_t>(v >> 8);
    b[at + 3] = static_cast<std::uint8_t>(v);
}

enum ArcIndex { Beard, Eye, Eyebrow, Faceline, FaceTex, ForeHead, Glass, GlassTex, Hair, Mask, Mole, Mouth, Mustache, Nose,
                Nline, NlineTex, Cap, CapTex };
// Eye, Eyebrow, FaceTex, GlassTex, Mole, Mouth, Mustache, NlineTex, CapTex.
inline const bool kIsTexture[18] = {false, true, true,  false, true,  false, false, true,  false,
                             false, true, true,  true,  false, false, true,  false, true};
inline const char* const kTags[18] = {"berd", "", "", "face", "", "frhd", "glas", "", "hair", "mask", "", "", "", "nose", "nsln", "", "cap_", ""};
inline const bool kTexCoords[18] = {false, false, false, true, false, false, true, false, false, true,
                             false, false, false, false, true, false, true, false};

inline Bytes texture(unsigned format, unsigned width, unsigned height, unsigned imageBytes) {
    Bytes t(0x20, 0);
    t[0] = static_cast<std::uint8_t>(format);
    t[2] = static_cast<std::uint8_t>(width >> 8);
    t[3] = static_cast<std::uint8_t>(width);
    t[4] = static_cast<std::uint8_t>(height >> 8);
    t[5] = static_cast<std::uint8_t>(height);
    t[0x18] = 1;
    t[0x1A] = 0xFF;  // lodBias -2
    t[0x1B] = 0xFE;
    set32(t, 0x1C, 0x20);
    t.resize(0x20 + imageBytes, 0x5A);
    return t;
}

inline Bytes shape(int arc) {
    Bytes s;
    for (int i = 0; i < 4; ++i) {
        s.push_back(static_cast<std::uint8_t>(kTags[arc][i]));
    }
    if (arc == Faceline) {
        const float values[9] = {1.0f, 2.0f, 3.0f, -1.0f, 0.5f, 0.25f, 100.0f, -200.0f, 3.5f};
        for (float f : values) {
            std::uint32_t u;
            std::memcpy(&u, &f, 4);
            put32(s, u);
        }
    }
    put16(s, 3);  // positions
    const std::int16_t positions[9] = {-100, 200, 300, 400, -500, 600, 7, 8, -9};
    for (std::int16_t v : positions) {
        put16(s, static_cast<std::uint16_t>(v));
    }
    put16(s, 1);  // normals
    put16(s, 0x1000);
    put16(s, 0xF000);
    put16(s, 0x0001);
    if (kTexCoords[arc]) {
        put16(s, 2);
        put16(s, 0x0102);
        put16(s, 0x0304);
        put16(s, 0xFFFE);
        put16(s, 0x8000);
    }
    s.push_back(1);     // primitives
    s.push_back(3);     // vertices
    s.push_back(0x90);  // GX_TRIANGLES
    for (int v = 0; v < 3; ++v) {
        s.push_back(static_cast<std::uint8_t>(v));
        s.push_back(0);
        if (kTexCoords[arc]) {
            s.push_back(static_cast<std::uint8_t>(v & 1));
        }
    }
    return s;
}

struct Resource {
    Bytes bytes;
    std::size_t fileStart[18];  // offset of each section's first file
};

inline Resource buildResource(const std::vector<Bytes> files[18]) {
    Resource r;
    Bytes& b = r.bytes;
    put16(b, 18);
    put16(b, 0x039D);
    for (int a = 0; a < 18; ++a) {
        put32(b, 0);
    }
    for (int a = 0; a < 18; ++a) {
        set32(b, 4 + a * 4, static_cast<std::uint32_t>(b.size()));
        std::uint32_t biggest = 0;
        for (const Bytes& f : files[a]) {
            biggest = f.size() > biggest ? static_cast<std::uint32_t>(f.size()) : biggest;
        }
        put16(b, static_cast<std::uint16_t>(files[a].size()));
        put16(b, static_cast<std::uint16_t>(biggest));
        std::uint32_t offset = 0;
        put32(b, 0);
        for (const Bytes& f : files[a]) {
            offset += static_cast<std::uint32_t>(f.size());
            put32(b, offset);
        }
        r.fileStart[a] = b.size();
        for (const Bytes& f : files[a]) {
            b.insert(b.end(), f.begin(), f.end());
        }
    }
    return r;
}

inline Resource syntheticResource() {
    std::vector<Bytes> files[18];
    for (int a = 0; a < 18; ++a) {
        if (kIsTexture[a]) {
            files[a].push_back(texture(5, 38, 32, 40 * 32 * 2));  // RGB5A3, width padded to 4x4 tiles
            files[a].push_back(texture(0, 8, 8, 32));             // I4
        } else {
            files[a].push_back(shape(a));
            Bytes empty;
            for (int i = 0; i < 4; ++i) {
                empty.push_back(static_cast<std::uint8_t>(kTags[a][i]));
            }
            if (a == Faceline) {
                empty.resize(empty.size() + 36, 0);
            }
            put16(empty, 0);  // an empty part
            empty.resize(empty.size() + 3, 0);
            files[a].push_back(empty);
        }
    }
    return buildResource(files);
}

}  // namespace RflTestResource

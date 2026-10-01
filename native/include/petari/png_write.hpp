#pragma once
// Minimal uncompressed (stored-deflate) RGB PNG writer for the opt-in EFB/XFB dumps. Debug tool only.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace PetariNative {
inline std::uint32_t crc32(const unsigned char* data, std::size_t size, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}
inline void putBE(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<unsigned char>(v >> shift));
}
inline void putChunk(std::FILE* file, const char* type, const std::vector<unsigned char>& body) {
    std::vector<unsigned char> head;
    putBE(head, static_cast<std::uint32_t>(body.size()));
    std::fwrite(head.data(), 1, 4, file);
    std::vector<unsigned char> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    std::fwrite(typed.data(), 1, typed.size(), file);
    std::vector<unsigned char> crc;
    putBE(crc, crc32(typed.data(), typed.size()));
    std::fwrite(crc.data(), 1, 4, file);
}
inline void writePngRgb(const std::string& path, std::uint32_t width, std::uint32_t height, const unsigned char* rgb) {
    std::vector<unsigned char> raw;
    raw.reserve(std::size_t(width) * height * 3 + height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + std::size_t(y) * width * 3, rgb + std::size_t(y + 1) * width * 3);
    }
    std::vector<unsigned char> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n >= raw.size() ? 1 : 0);
        z.push_back(n & 0xff); z.push_back(n >> 8); z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
        if (n == 0) break;
    }
    putBE(z, (b << 16) | a);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::fwrite(signature, 1, 8, file);
    std::vector<unsigned char> ihdr;
    putBE(ihdr, width); putBE(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    putChunk(file, "IHDR", ihdr);
    putChunk(file, "IDAT", z);
    putChunk(file, "IEND", {});
    std::fclose(file);
}
}

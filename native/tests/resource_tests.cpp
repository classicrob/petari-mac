#include "archive.hpp"
#include "u8_archive.hpp"
#include <revolution/arc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

using namespace PetariNative::Resource;
using Buffer = std::vector<std::uint8_t>;
static void check(bool condition, const char* text) {
    if (!condition) { std::fprintf(stderr, "%s\n", text); std::exit(1); }
}
static Bytes view(const Buffer& data) { return {data.data(), data.size()}; }
static void put32(Buffer& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) data[offset + i] = value >> (24 - i * 8);
}
template<class F> static void rejects(F&& f) {
    try { f(); } catch (const std::runtime_error&) { return; }
    check(false, "Malformed input was accepted");
}
static Buffer yazLiteral(const Buffer& input) {
    Buffer out(16);
    std::memcpy(out.data(), "Yaz0", 4);
    put32(out, 4, input.size());
    for (std::size_t i = 0; i < input.size();) {
        out.push_back(0xff);
        for (unsigned n = 0; n < 8 && i < input.size(); ++n) out.push_back(input[i++]);
    }
    return out;
}
static Buffer archiveFixture() {
    Buffer data(0x84);
    std::memcpy(data.data(), "RARC", 4);
    put32(data, 4, data.size()); put32(data, 8, 0x20);
    put32(data, 12, 0x60); put32(data, 16, 4); put32(data, 20, 4);
    put32(data, 0x20, 1); put32(data, 0x24, 0x20);
    put32(data, 0x28, 1); put32(data, 0x2c, 0x30);
    put32(data, 0x30, 16); put32(data, 0x34, 0x44);
    std::memcpy(data.data() + 0x40, "ROOT", 4);
    data[0x4b] = 1;
    data[0x51] = 7; data[0x54] = 0x11; data[0x57] = 5;
    put32(data, 0x5c, 4);
    std::memcpy(data.data() + 0x64, "root\0sample.bin\0", 16);
    std::memcpy(data.data() + 0x80, "test", 4);
    return data;
}
static void checkU8() {
    Buffer bytes(0x87);
    put32(bytes, 0, 0x55aa382d); put32(bytes, 4, 0x20);
    put32(bytes, 8, 0x45); put32(bytes, 12, 0x80);
    bytes[0x20] = 1; put32(bytes, 0x28, 4);
    put32(bytes, 0x2c, 0x01000001); put32(bytes, 0x34, 4);
    put32(bytes, 0x38, 5); put32(bytes, 0x3c, 0x80); put32(bytes, 0x40, 3);
    put32(bytes, 0x44, 13); put32(bytes, 0x48, 0x84); put32(bytes, 0x4c, 3);
    std::memcpy(bytes.data() + 0x50, "\0dir\0one.bin\0two.bin\0", 21);
    std::memcpy(bytes.data() + 0x80, "one\0two", 7);
    const auto entries = parseU8(view(bytes));
    check(entries.size() == 4 && entries[2].name == "one.bin", "U8 table decode failed");
    ARCHandle handle{};
    check(ARCInitHandle(bytes.data(), &handle), "Native SDK U8 initialization failed");
    ARCFileInfo file{};
    check(ARCOpen(&handle, "/DIR/one.bin", &file) && ARCGetLength(&file) == 3 &&
          std::memcmp(ARCGetStartAddrInMem(&file), "one", 3) == 0, "SDK ARC file lookup failed");
    check(ARCConvertPathToEntrynum(&handle, "/dir/../dir/./two.bin") == 3, "SDK ARC relative path components failed");
    check(ARCConvertPathToEntrynum(&handle, "/dir/one.bin/") == -1, "SDK ARC file accepted as directory");
    check(ARCChangeDir(&handle, "/dir"), "SDK ARC change directory failed");
    char path[32];
    check(ARCGetCurrentDir(&handle, path, sizeof(path)) && std::strcmp(path, "/dir/") == 0, "SDK ARC current directory failed");
    check(!ARCGetCurrentDir(&handle, path, 0), "SDK ARC zero-size output accepted");
    ARCDir dir{}; ARCDirEntry entry{};
    check(ARCOpenDir(&handle, ".", &dir) && ARCReadDir(&dir, &entry) && std::strcmp(entry.name, "one.bin") == 0 &&
          ARCReadDir(&dir, &entry) && std::strcmp(entry.name, "two.bin") == 0 && !ARCReadDir(&dir, &entry), "SDK ARC enumeration failed");
    for (std::size_t length = 0; length < bytes.size(); ++length)
        rejects([&] { parseU8({bytes.data(), length}); });
    auto bad = bytes; put32(bad, 0x30, 1);
    rejects([&] { parseU8(view(bad)); });
    bad = bytes; put32(bad, 0x34, 5);
    rejects([&] { parseU8(view(bad)); });
    bad = bytes; put32(bad, 0x3c, 0x40);
    rejects([&] { parseU8(view(bad)); });
}
int main() {
    checkU8();
    Buffer yaz(16); std::memcpy(yaz.data(), "Yaz0", 4); put32(yaz, 4, 9);
    yaz.insert(yaz.end(), {0xe0, 'a', 'b', 'c', 0x40, 0x02});
    check(decompress(view(yaz)) == Buffer({'a','b','c','a','b','c','a','b','c'}), "Yaz0 overlapping back-reference failed");
    Buffer yay(16); std::memcpy(yay.data(), "Yay0", 4); put32(yay, 4, 9);
    put32(yay, 8, 20); put32(yay, 12, 22);
    yay.insert(yay.end(), {0xe0,0,0,0, 0x40,0x02, 'a','b','c'});
    check(decompress(view(yay)) == decompress(view(yaz)), "Yay0 split-stream decoding failed");
    Buffer longRun(16); std::memcpy(longRun.data(), "Yaz0", 4); put32(longRun, 4, 20);
    longRun.insert(longRun.end(), {0x80, 'x', 0, 0, 1});
    check(decompress(view(longRun)) == Buffer(20, 'x'), "Long Yaz0 run failed");
    for (std::size_t length = 0; length < yaz.size(); ++length)
        rejects([&] { decompress({yaz.data(), length}); });
    for (std::size_t length = 0; length < yay.size(); ++length)
        rejects([&] { decompress({yay.data(), length}); });
    auto bad = yaz; bad.back() = 10;
    rejects([&] { decompress(view(bad)); });
    rejects([&] { decompress(view(yaz), 8); });
    auto bytes = archiveFixture();
    auto archive = Archive::parse(view(bytes));
    check(archive.directories().size() == 1 && archive.directories()[0].name == "root", "RARC directory decode failed");
    check(archive.entries().size() == 1 && archive.entries()[0].id == 7 && archive.entries()[0].name == "sample.bin", "RARC entry decode failed");
    check(archive.resourceData(0) == Buffer({'t','e','s','t'}), "RARC payload decode failed");
    auto compressed = yazLiteral(bytes);
    check(Archive::parse(view(compressed)).resourceData(0) == archive.resourceData(0), "Compressed RARC failed");
    auto packedFile = bytes;
    const auto packedPayload = yazLiteral({'t','e','s','t'});
    packedFile.resize(0x80);
    packedFile.insert(packedFile.end(), packedPayload.begin(), packedPayload.end());
    put32(packedFile, 4, packedFile.size());
    put32(packedFile, 16, packedPayload.size());
    put32(packedFile, 20, packedPayload.size());
    put32(packedFile, 0x5c, packedPayload.size());
    packedFile[0x54] |= 0x84;
    const auto packedArchive = Archive::parse(view(packedFile));
    check(packedArchive.resourceData(0) == archive.resourceData(0), "Compressed RARC file payload failed");
    rejects([&] { packedArchive.resourceData(0, 3); });
    auto moved = std::move(archive);
    check(moved.resourceData(0) == Buffer({'t','e','s','t'}), "Archive move invalidated resource storage");
    for (std::size_t length = 0; length < bytes.size(); ++length)
        rejects([&] { Archive::parse({bytes.data(), length}); });
    bad = bytes; put32(bad, 0x58, 0xfffffff0);
    rejects([&] { Archive::parse(view(bad)); });
    bad = bytes; put32(bad, 0x2c, 0xfffffff0);
    rejects([&] { Archive::parse(view(bad)); });
    bad = bytes; bad[0x57] = 16;
    rejects([&] { Archive::parse(view(bad)); });
    bad = bytes; bad[0x4b] = 2;
    rejects([&] { Archive::parse(view(bad)); });
    bad = bytes; std::memset(bad.data() + 0x64, 'x', 16);
    rejects([&] { Archive::parse(view(bad)); });
    std::puts("Big-endian RARC, Yaz0/Yay0, overlapping runs and malformed resource checks passed.");
}

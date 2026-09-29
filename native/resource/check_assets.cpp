#include "archive.hpp"
#include "u8_archive.hpp"
#include <petari/endian.hpp>
#include <revolution/arc.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: petari_asset_check ARCHIVE_OR_DIRECTORY\n";
        return 2;
    }
    namespace fs = std::filesystem;
    std::size_t archives = 0, u8Archives = 0, resources = 0, failures = 0;
    auto check = [&](const fs::path& path) {
        try {
            const auto size = fs::file_size(path);
            if (size > 256 * 1024 * 1024) throw std::runtime_error("Archive exceeds 256 MiB input limit");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::ifstream file(path, std::ios::binary);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Could not read archive");
            if (bytes.size() >= 4 && PetariNative::readU32BE(bytes.data()) == 0x55aa382d) {
                const auto entries = PetariNative::Resource::parseU8({bytes.data(), bytes.size()});
                ARCHandle handle{};
                if (!ARCInitHandle(bytes.data(), &handle)) throw std::runtime_error("SDK ARC rejected validated archive");
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    const auto& entry = entries[i];
                    if (entry.directory) continue;
                    ARCFileInfo file{};
                    if (!ARCFastOpen(&handle, i, &file) || ARCGetLength(&file) != entry.endOrSize ||
                        ARCGetStartAddrInMem(&file) != bytes.data() + entry.parentOrOffset)
                        throw std::runtime_error("SDK ARC file extent disagrees with validated metadata");
                    ++resources;
                }
                ++u8Archives;
                return;
            }
            const auto archive = PetariNative::Resource::Archive::parse({bytes.data(), bytes.size()});
            for (std::size_t i = 0; i < archive.entries().size(); ++i) {
                if (!archive.entries()[i].isDirectory()) {
                    archive.resourceData(i);
                    ++resources;
                }
            }
            ++archives;
        } catch (const std::exception& e) {
            std::cerr << path << ": " << e.what() << '\n';
            ++failures;
        }
    };
    try {
        const fs::path input(argv[1]);
        if (fs::is_directory(input)) {
            for (const auto& file : fs::recursive_directory_iterator(input)) {
                if (file.is_regular_file() && file.path().extension() == ".arc") check(file.path());
            }
        } else {
            check(input);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Validated " << archives << " RARC archives, " << u8Archives << " U8 archives and " << resources
              << " resource payloads; " << failures << " failures.\n"
              << "This checks archive structure and decompression, not resource format decoding or gameplay.\n";
    return failures || !(archives + u8Archives) ? 1 : 0;
}

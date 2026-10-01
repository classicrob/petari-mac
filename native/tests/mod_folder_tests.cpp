// Tests for the disc-file mod folder (native/platform/dvd/mod_folder.cpp): discovery,
// path normalization, enable states, precedence. Synthetic files only.

#include "petari/platform/mod_folder.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;
namespace MF = PetariNative::Platform::ModFolder;

namespace {
int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}
// A minimal valid RARC (0x40-byte header) followed by a tag: distinct content, accepted by the archive check.
std::string arc(const std::string& tag) {
    std::string d = "RARC";
    for (std::uint32_t v : {0x40u, 0x20u, 0x20u, 0x20u, 0u, 0u, 0u}) {
        d += static_cast<char>(v >> 24); d += static_cast<char>(v >> 16); d += static_cast<char>(v >> 8); d += static_cast<char>(v);
    }
    d.resize(0x40, '\0');
    return d + tag;
}
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
const PetariNative::Platform::DVD::OverlayFile* find(const MF::Resolved& r, const std::string& path) {
    for (const auto& f : r.files) {
        if (f.path == path) return &f;
    }
    return nullptr;
}
}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / ("petari_modfolder_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // Path normalization: case kept, separators and leading slashes cleaned, escapes rejected.
    check(MF::normalizePath("/StageData//Foo/./Bar.arc") == "StageData/Foo/Bar.arc", "leading slash, double slash, dot");
    check(MF::normalizePath("stagedata\\foo\\bar.arc") == "stagedata/foo/bar.arc", "backslashes");
    check(MF::normalizePath("../x.arc").empty() && MF::normalizePath("a/../../x").empty(), "parent escapes are rejected");
    check(MF::normalizePath("").empty() && MF::normalizePath("///").empty() && MF::normalizePath("./.").empty(), "empty paths");
    check(MF::normalizePath("a/b/") == "a/b", "trailing slash");

    // Two mods dirs: user and dev. Names are folders; mod.txt is optional.
    const fs::path user = root / "user" / "mods", dev = root / "dev";
    write(user / "Alpha/files/StageData/Foo.arc", arc("alpha-foo"));
    write(user / "Alpha/files/LayoutData/Bar.arc", arc("alpha-bar"));
    write(user / "Alpha/mod.txt", "name = Alpha Pack\ndescription=first\npriority=5\n");
    write(user / "Beta/files/stagedata/FOO.ARC", arc("beta-foo"));  // same disc path as Alpha, other case
    write(user / "Beta/files/New/Thing.bin", "beta-new");
    write(user / "Beta/files/.DS_Store", "junk");
    write(user / "Gamma/mod.txt", "priority=9\n");            // no files/
    write(user / "Delta/files/StageData/Foo.arc", arc("delta-foo"));  // same priority (0) as Beta, name sorts first
    write(user / "Bad=Name/files/x", "x");                    // cannot be a mods.txt key
    write(dev / "Alpha/files/Other.arc", arc("dev-alpha"));        // shadowed by the user's Alpha
    write(dev / "DevOnly/files/Dev.arc", arc("dev"));
    const auto mods = MF::discover({user, dev});
    std::vector<std::string> names;
    for (const auto& m : mods) names.push_back(m.name);
    check((names == std::vector<std::string>{"Alpha", "Beta", "Delta", "Gamma", "DevOnly"}), "discover: sorted per directory, user wins, bad names skipped");
    check(mods[0].title == "Alpha Pack" && mods[0].description == "first" && mods[0].priority == 5 && mods[0].fileCount == 2,
          "mod.txt fields and file count");
    check(mods[1].title == "Beta" && mods[1].fileCount == 2, "no mod.txt: folder name; host metadata not counted");
    check(mods[3].fileCount == 0, "a folder without files/ is listed with no files");

    // States live in mods.txt next to gameplay lines, which are kept.
    const fs::path modsTxt = root / "user" / "mods.txt";
    check(MF::readStates(modsTxt).empty(), "missing mods.txt: nothing enabled");
    write(modsTxt, "# comment\nCollectStarBits=on\nFolder.Alpha=on\nShootEnemy=off\n");
    check((MF::readStates(modsTxt) == std::map<std::string, bool>{{"Alpha", true}}), "Folder lines are read, others ignored");
    std::string error;
    check(MF::writeState(modsTxt, "Beta", true, &error), "write adds a line");
    check(MF::writeState(modsTxt, "Alpha", false, &error), "write updates a line");
    check(read(modsTxt) == "# comment\nCollectStarBits=on\nFolder.Alpha=off\nShootEnemy=off\nFolder.Beta=on\n", "other lines are preserved verbatim");

    // Default off: no state, no overlay.
    check(MF::resolve(mods, {}).files.empty(), "nothing enabled: no files");
    check(MF::resolve(mods, {{"Alpha", false}}).files.empty(), "explicitly off: no files");

    // Precedence: priority descending, then name ascending; conflicts are logged.
    auto both = MF::resolve(mods, {{"Alpha", true}, {"Beta", true}});
    check(both.files.size() == 3, "case-insensitive same path collapses to one file");
    const auto* foo = find(both, "StageData/Foo.arc");
    check(foo && foo->mod == "Alpha" && read(foo->host) == arc("alpha-foo"), "higher priority wins");
    bool conflictLogged = false;
    for (const auto& line : both.log) conflictLogged |= line.find("conflict") != std::string::npos && line.find("Alpha wins over Beta") != std::string::npos;
    check(conflictLogged, "the conflict and its winner are logged");
    auto tie = MF::resolve(mods, {{"Beta", true}, {"Delta", true}});
    const auto* tied = find(tie, "stagedata/FOO.ARC");
    const auto* tiedAlt = find(tie, "StageData/Foo.arc");
    check((tied && tied->mod == "Beta") != (tiedAlt && tiedAlt->mod == "Delta") || (tiedAlt && tiedAlt->mod == "Beta"),
          "tie: exactly one of the two supplies the path");
    check(tie.files.size() == 2 && (tied ? tied->mod : tiedAlt->mod) == "Beta", "equal priority: name order, Beta before Delta");
    auto gamma = MF::resolve(mods, {{"Gamma", true}});
    check(gamma.files.empty() && !gamma.log.empty(), "an enabled mod without files/ applies nothing and says so");
    check(MF::resolve(mods, {{"Nope", true}}).files.empty(), "an enabled name that is not installed is ignored");

    // PETARI_MOD_FOLDERS overrides the file for one run.
    ::setenv("PETARI_MOD_FOLDERS", "Beta,Delta", 1);
    const auto forced = MF::effectiveStates(modsTxt);
    check(forced.count("Beta") && forced.at("Beta") && forced.at("Delta") && !forced.at("Alpha"), "env enables the named mods and disables the rest");
    ::setenv("PETARI_MOD_FOLDERS", "none", 1);
    check(!MF::effectiveStates(modsTxt).at("Beta"), "env none disables everything");
    ::unsetenv("PETARI_MOD_FOLDERS");

    // Search directories: user mods first, then PETARI_MOD_DIRS.
    ::setenv("PETARI_MOD_DIRS", (dev.string() + ":" + (root / "more").string()).c_str(), 1);
    const auto dirs = MF::searchDirectories(root / "user");
    check(dirs.size() == 3 && dirs[0] == root / "user" / "mods" && dirs[1] == dev, "search directories");
    ::unsetenv("PETARI_MOD_DIRS");

    // Header validation: a broken archive must never replace a working one.
    {
        auto be = [](std::string& out, std::uint32_t v) {
            out += static_cast<char>(v >> 24); out += static_cast<char>(v >> 16); out += static_cast<char>(v >> 8); out += static_cast<char>(v);
        };
        auto rarc = [&](std::uint32_t sizeField, std::size_t length) {
            std::string d = "RARC";
            be(d, sizeField); be(d, 0x20); be(d, 0x20); be(d, 0x20); be(d, 0); be(d, 0); be(d, 0);
            d.resize(length, '\0');
            return d;
        };
        auto yaz0 = [&](const std::string& payload, std::uint32_t declared) {
            std::string out = "Yaz0";
            be(out, declared); be(out, 0); be(out, 0);
            for (std::size_t i = 0; i < payload.size(); i += 8) {
                const std::size_t n = std::min<std::size_t>(8, payload.size() - i);
                out += static_cast<char>((0xFF << (8 - n)) & 0xFF);
                out += payload.substr(i, n);
            }
            return out;
        };
        const fs::path v = root / "valid";
        write(v / "good.arc", rarc(0x40, 0x40));
        write(v / "padded.arc", rarc(0x40, 0x40 + 4096));          // trailing padding is fine
        write(v / "good_yaz0.arc", yaz0(rarc(0x40, 0x40), 0x40));
        write(v / "good.szs", yaz0(rarc(0x80, 0x80), 0x80));
        {
            std::string u8;
            for (std::uint32_t x : {0x55AA382Du, 0x20u, 0x20u, 0x40u}) be(u8, x);
            u8.resize(0x20, '\0');
            for (std::uint32_t x : {0x01000000u, 0u, 6u}) be(u8, x);   // a root directory node
            u8.resize(0x40, '\0');
            write(v / "good_u8.arc", u8);
            write(v / "good_u8_yaz0.arc", yaz0(u8, 0x40));
            std::string badU8 = u8;
            badU8[7] = 0x40;   // root node offset must be 0x20
            write(v / "bad_u8.arc", badU8);
        }
        write(v / "bad_magic.arc", "XARC" + rarc(0x40, 0x40).substr(4));
        write(v / "truncated.arc", rarc(0x40, 0x40).substr(0, 20));
        write(v / "size_beyond_file.arc", rarc(0x1000, 0x40));
        write(v / "empty.arc", "");
        write(v / "yaz0_insane.arc", yaz0(rarc(0x40, 0x40), 0x7FFFFFFF));
        write(v / "yaz0_tiny.arc", yaz0(rarc(0x40, 0x40), 8));
        write(v / "yaz0_not_rarc.arc", yaz0(std::string(0x40, 'x'), 0x40));
        write(v / "audio_data.szs", yaz0("AA_<bst " + std::string(0x38, 'x'), 0x40));  // .szs may hold non-archive data
        write(v / "yaz0_truncated.arc", yaz0(rarc(0x40, 0x40), 0x40).substr(0, 24));
        write(v / "good.bmg", "MESGbmg1" + std::string(24, '\0'));
        write(v / "bad.bmg", "NOTBMG__" + std::string(24, '\0'));
        write(v / "good.brstm", "RSTM" + std::string(28, '\0'));
        write(v / "bad.brstm", "RIFF" + std::string(28, '\0'));
        write(v / "anything.bcsv", "not checked");
        write(v / "README", "not an archive name");
        for (const char* name : {"good.arc", "padded.arc", "good_yaz0.arc", "good.szs", "audio_data.szs", "good_u8.arc", "good_u8_yaz0.arc", "good.bmg", "good.brstm", "anything.bcsv", "README"})
            check(MF::invalidKind(v / name, name).empty(), name);
        for (const char* name : {"bad_magic.arc", "truncated.arc", "size_beyond_file.arc", "empty.arc", "yaz0_insane.arc", "yaz0_tiny.arc",
                                 "yaz0_not_rarc.arc", "yaz0_truncated.arc", "bad_u8.arc"})
            check(MF::invalidKind(v / name, name) == "archive", name);
        check(MF::invalidKind(v / "bad.bmg", "Msg/Bad.BMG") == "message file", "bad BMG");
        check(MF::invalidKind(v / "bad.brstm", "AudioRes/Stream/x.brstm") == "stream", "bad BRSTM");
        check(MF::invalidKind(v / "missing.arc", "missing.arc") == "archive", "unreadable archive");
        check(MF::invalidKind(v / "good.arc", "StageData/Upper.ARC").empty() && MF::invalidKind(v / "bad_magic.arc", "StageData/Upper.ARC") == "archive",
              "extension match is case-insensitive");

        // In a mod: the broken file is not applied and the log says so; the other files still are.
        const fs::path um = root / "user2" / "mods";
        write(um / "Broken/files/StageData/Corrupt.arc", "XARC" + rarc(0x40, 0x40).substr(4));
        write(um / "Broken/files/StageData/Fine.arc", rarc(0x40, 0x40));
        write(um / "Broken/files/StageData/Both.arc", "XARC" + rarc(0x40, 0x40).substr(4));
        write(um / "Backup/files/StageData/Both.arc", rarc(0x40, 0x40));
        write(um / "Backup/mod.txt", "priority=-5\n");
        const auto broken = MF::discover({um});
        const auto bad = MF::resolve(broken, {{"Broken", true}});
        check(bad.files.size() == 1 && bad.files[0].path == "StageData/Fine.arc", "a corrupt archive is not applied");
        bool said = false;
        for (const auto& line : bad.log) said |= line == "Broken: StageData/Corrupt.arc is not a valid archive; using the disc file";
        check(said, "the log names the mod, the file and the fallback");
        const auto fallback = MF::resolve(broken, {{"Broken", true}, {"Backup", true}});
        const auto* both = find(fallback, "StageData/Both.arc");
        check(both && both->mod == "Backup", "a lower-priority mod's valid file is used instead of the broken one");
        bool named = false;
        for (const auto& line : fallback.log) named |= line == "Broken: StageData/Both.arc is not a valid archive; using the file from mod Backup";
        check(named, "the log names the mod whose file is used instead");

        // Optional: every archive of a real disc must pass (PETARI_DISC_FILES=<RMGE01>/files). Skipped in CI.
        if (const char* discFiles = std::getenv("PETARI_DISC_FILES")) {
            int seen = 0;
            std::vector<std::string> rejectedFiles;
            for (const auto& entry : fs::recursive_directory_iterator(discFiles)) {
                const std::string name = entry.path().filename().string();
                const auto dot = name.rfind('.');
                if (!entry.is_regular_file() || dot == std::string::npos) continue;
                std::string extension = name.substr(dot);
                for (char& c : extension) c = static_cast<char>(std::toupper(c));
                if (extension != ".ARC" && extension != ".SZS" && extension != ".BMG" && extension != ".BRSTM") continue;
                ++seen;
                if (!MF::invalidKind(entry.path(), name).empty()) rejectedFiles.push_back(entry.path().string());
            }
            for (const auto& f : rejectedFiles) std::fprintf(stderr, "disc file rejected: %s\n", f.c_str());
            std::printf("validated %d real disc files, %zu rejected\n", seen, rejectedFiles.size());
            check(rejectedFiles.empty(), "a real disc file must pass validation");
        }
    }

    fs::remove_all(root);
    std::printf("mod folder tests passed (%d checks)\n", checks);
    return 0;
}

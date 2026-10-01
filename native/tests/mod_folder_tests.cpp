// Tests for the disc-file mod folder (native/platform/dvd/mod_folder.cpp): discovery,
// path normalization, enable states, precedence. Synthetic files only.

#include "petari/platform/mod_folder.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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
    write(user / "Alpha/files/StageData/Foo.arc", "alpha-foo");
    write(user / "Alpha/files/LayoutData/Bar.arc", "alpha-bar");
    write(user / "Alpha/mod.txt", "name = Alpha Pack\ndescription=first\npriority=5\n");
    write(user / "Beta/files/stagedata/FOO.ARC", "beta-foo");  // same disc path as Alpha, other case
    write(user / "Beta/files/New/Thing.bin", "beta-new");
    write(user / "Beta/files/.DS_Store", "junk");
    write(user / "Gamma/mod.txt", "priority=9\n");            // no files/
    write(user / "Delta/files/StageData/Foo.arc", "delta-foo");  // same priority (0) as Beta, name sorts first
    write(user / "Bad=Name/files/x", "x");                    // cannot be a mods.txt key
    write(dev / "Alpha/files/Other.arc", "dev-alpha");        // shadowed by the user's Alpha
    write(dev / "DevOnly/files/Dev.arc", "dev");
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
    check(foo && foo->mod == "Alpha" && read(foo->host) == "alpha-foo", "higher priority wins");
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

    fs::remove_all(root);
    std::printf("mod folder tests passed (%d checks)\n", checks);
    return 0;
}

#pragma once
// Disc-file mods (native/MODS.md, "Mod folder"): a mods/ directory with one
// folder per mod, each mirroring the disc's files/ tree:
//
//   mods/<ModName>/files/StageData/..., ObjectData/..., LayoutData/..., AudioRes/...
//   mods/<ModName>/mod.txt   optional: name=, description=, priority=
//
// An enabled mod's file replaces the disc file of the same path (ASCII
// case-insensitive, like the game's own lookup) or adds a new one. The
// native DVD layer applies the result at mount (dvd.hpp, MountOptions::overlay),
// so every reader of the disc sees it: archives, streamed audio, movies.
//
// Every mod is off unless the player turned it on: states live in mods.txt in
// the user directory as "Folder.<ModName>=on|off" lines (the other lines of
// that file belong to the gameplay mods and are preserved). PETARI_MOD_FOLDERS=
// <A>,<B> (or "none") overrides the file for one run. With nothing enabled the
// disc is not touched at all.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "petari/platform/dvd.hpp"

namespace PetariNative::Platform::ModFolder {

struct ModInfo {
    std::string name;          // the folder name; the mods.txt key
    std::string title;         // mod.txt name=, defaults to the folder name
    std::string description;   // mod.txt description=
    int priority = 0;          // mod.txt priority=; higher wins a conflict
    std::filesystem::path root;  // mods/<ModName>
    std::string source;        // the mods directory it was found in
    unsigned fileCount = 0;    // files below files/
};

// "/StageData//Foo/./Bar.arc" and "stagedata\\foo\\bar.arc" -> "StageData/Foo/Bar.arc"
// (case kept; matching is case-insensitive later). Empty for a path that is
// empty, names a directory only, or escapes with "..".
std::string normalizePath(const std::string& path);

// Mods found below the given directories, in directory order; a name already
// found in an earlier directory is ignored (the user directory wins over dev
// directories). Sorted by name within a directory. Folders without a files/
// directory are listed (fileCount 0) so the menu can show them.
std::vector<ModInfo> discover(const std::vector<std::filesystem::path>& directories);

// Enabled states from the "Folder.<name>=on|off" lines of a mods.txt text/file.
// Missing file: empty. Other lines are ignored.
std::map<std::string, bool> parseStates(const std::string& text);
std::map<std::string, bool> readStates(const std::filesystem::path& modsFile);
// Rewrites one Folder.<name> line (adding it if absent) and keeps every other
// line of the file as it is.
bool writeState(const std::filesystem::path& modsFile, const std::string& name, bool on, std::string* error);

// Applies PETARI_MOD_FOLDERS (if set) over the file's states.
std::map<std::string, bool> effectiveStates(const std::filesystem::path& modsFile);

struct Resolved {
    std::vector<DVD::OverlayFile> files;      // one per path, precedence applied
    std::vector<std::string> log;        // human readable: order, conflicts, empty mods
};
// Enabled mods in precedence order (priority descending, then name ascending);
// the first one listed that supplies a path wins it.
Resolved resolve(const std::vector<ModInfo>& mods, const std::map<std::string, bool>& states);

// The mods directories searched: <user>/mods, then each entry of PETARI_MOD_DIRS
// (":"-separated; for development, e.g. the repo's mods/).
std::vector<std::filesystem::path> searchDirectories(const std::filesystem::path& userDirectory);

// discover + effectiveStates + resolve, for the app at start-up. Also
// remembers the directory for the menu calls below.
Resolved resolveForUser(const std::filesystem::path& userDirectory);

// The Home menu's Mod folder page: the mods found for the directory given to
// resolveForUser (none before that), and the saved enable state of one of them.
std::vector<ModInfo> detected();
std::map<std::string, bool> detectedStates();
bool setEnabled(const std::string& name, bool on, std::string* error);

}  // namespace PetariNative::Platform::ModFolder

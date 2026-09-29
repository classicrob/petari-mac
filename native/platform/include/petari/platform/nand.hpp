#pragma once
// Host-side control of the native NAND service (save data).
//
// The game uses the Revolution SDK NAND API from <revolution/nand.h>. This
// header chooses which host directory plays the console's NAND flash. Nothing
// is written anywhere until a root is mounted explicitly or named by
// PETARI_NAND_ROOT; without one, NANDInit() fails like a console whose file
// system is unavailable.

#include <filesystem>
#include <string>

namespace PetariNative::Platform::NAND {

// Mounts an existing, writable host directory as the NAND root. The first
// NANDInit() afterwards performs the console's boot-time setup: system
// directories (/title, /tmp, /shared2, /meta), clearing /tmp, and the running
// title's data directory, derived from the mounted disc's ID.
bool mount(const std::filesystem::path& root, std::string* error);
bool isMounted();

// Host location of a NAND path (for tools and tests), or empty if none is mounted
// or the path is invalid.
std::filesystem::path hostPath(const char* nandPath);

// Closes every NAND file, unmounts, and returns the NAND library to its
// uninitialised state. For tests and application shutdown.
void shutdown();

inline constexpr const char* kRootEnvironmentVariable = "PETARI_NAND_ROOT";

// IOS user id of the running title. IOS assigns the first title uid 0x1000.
inline constexpr unsigned kTitleUid = 0x1000;

}  // namespace PetariNative::Platform::NAND

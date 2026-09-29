#pragma once
// Host-side control of the native DVD service.
//
// The game keeps calling the Revolution SDK DVD API declared in
// <revolution/dvd.h>. This header adds the host operations the Wii has no
// equivalent for: choosing which extracted game directory acts as the disc.

#include <cstdint>
#include <filesystem>
#include <string>

namespace PetariNative::Platform::DVD {

enum class FstSource {
    // Entry numbers, names, and disc offsets come from sys/fst.bin. Every
    // file it lists must exist on the host with exactly the listed size.
    DiscFst,
    // No usable fst.bin: the files/ tree is scanned and an FST is built in
    // the order the Wii disc mastering tools use. Disc offsets are synthetic.
    DirectoryScan,
};

struct MountOptions {
    // Directory containing files/ (and normally sys/), or containing
    // DATA/files as produced by wit.
    std::filesystem::path root;
    // Build the FST from files/ even when sys/fst.bin exists. Needed for
    // trees whose files were modified after extraction.
    bool ignoreDiscFst = false;
};

struct MountInfo {
    std::filesystem::path filesDirectory;
    FstSource source = FstSource::DiscFst;
    std::uint32_t entryCount = 0;
    std::uint32_t fileCount = 0;
    // False when sys/boot.bin was absent; DVDGetCurrentDiskID() is then all zero.
    bool hasDiskId = false;
};

// Mounts an extracted disc. May be called before or after DVDInit(). Commands
// issued while no disc is mounted wait in DVD_STATE_NO_DISK, as on a console
// without a disc, and proceed once a mount succeeds. Remounting while a disc
// is mounted is refused because DVDDirEntry names point into the mounted FST.
bool mount(const MountOptions& options, std::string* error);
bool isMounted();
MountInfo mountInfo();

// Returns the host file backing a file entry, or an empty path for
// directories, invalid entries, or when nothing is mounted. For host-side
// consumers (for example streamed media) that cannot go through DVDRead.
std::filesystem::path hostPathForEntry(std::int32_t entryNum);

// Cancels all queued commands (their callbacks receive -3), stops the drive
// thread, and unmounts. DVDInit() may be called again afterwards. Must not be
// called from a DVD callback.
void shutdown();

// Environment variable DVDInit() consults when nothing has been mounted yet.
inline constexpr const char* kRootEnvironmentVariable = "PETARI_GAME_DIR";

}  // namespace PetariNative::Platform::DVD

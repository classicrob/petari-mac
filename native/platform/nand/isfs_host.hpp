#pragma once
// Host implementation of the IOS NAND file system (ISFS) that the NAND
// library sits on.
//
// The NAND tree is a host directory. Wii metadata that host files lack (owner
// uid, group gid, attribute, owner/group/other access) is kept in the
// extended attribute "com.petari.nand" on each node. Rules follow IOS:
// - absolute paths under 64 bytes, names of at most 12 characters, at most 8
//   levels deep;
// - access checked against the calling title's uid/gid, with uid 0 (system)
//   unrestricted; creating or deleting needs write access to the parent;
// - at most 15 open file descriptors; delete is recursive;
// - usage counted in 16 KiB clusters plus one inode per node.
// Results are ISFS error codes.

#include <cstdint>
#include <filesystem>
#include <string>

#include <revolution/fs.h>

namespace PetariNative::Platform::NAND::Fs {

constexpr u32 kClusterSize = 0x4000;
constexpr int kMaxFds = 15;
constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxNameLength = 12;

struct Attr {
    IOSUid ownerId = 0;
    IOSGid groupId = 0;
    u8 attr = 0;
    u8 ownerAccess = 0;   // bit 0 read, bit 1 write
    u8 groupAccess = 0;
    u8 othersAccess = 0;
};

// Mounts the host directory as the NAND root; it must exist.
bool mount(const std::filesystem::path& root, std::string* error);
bool isMounted();
std::filesystem::path root();
void unmount();  // closes every descriptor

// Identity of the running title, used for access checks and new nodes.
void setCaller(IOSUid uid, IOSGid gid);

// Creates a directory owned by the system (uid 0) if missing, as title
// installation or the system menu would have. Parents must exist.
ISFSError ensureSystemDir(const char* path, const Attr& attr);
// Empties a directory's contents, as IOS does to /tmp at boot.
ISFSError clearDir(const char* path);

ISFSError createFile(const char* path, u32 attr, u32 ownerAcc, u32 groupAcc, u32 othersAcc);
ISFSError createDir(const char* path, u32 attr, u32 ownerAcc, u32 groupAcc, u32 othersAcc);
s32 open(const char* path, u32 access);  // descriptor or error
ISFSError close(s32 fd);
s32 read(s32 fd, u8* buffer, u32 length);
s32 write(s32 fd, const u8* buffer, u32 length);
s32 seek(s32 fd, s32 offset, u32 whence);
ISFSError fileStats(s32 fd, ISFSFileStats* stats);
ISFSError remove(const char* path);
ISFSError rename(const char* oldPath, const char* newPath);
ISFSError getAttr(const char* path, Attr* attr);
ISFSError getUsage(const char* path, u32* blocks, u32* inodes);
// ISFS_ReadDir with a null name buffer: fails with ISFS_ERROR_INVALID for files.
ISFSError countDir(const char* path, u32* count);

}  // namespace PetariNative::Platform::NAND::Fs

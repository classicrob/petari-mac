#include "isfs_host.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

#include "petari/host_allocation.hpp"

namespace fs = std::filesystem;

namespace PetariNative::Platform::NAND::Fs {
namespace {

constexpr const char* kAttrName = "com.petari.nand";
constexpr std::size_t kMaxPath = 64;

struct Descriptor {
    bool used = false;
    int hostFd = -1;
    u32 access = 0;
    bool written = false;
    u32 position = 0;
    std::string path;  // NAND path
};

struct State {
    std::mutex lock;
    fs::path root;
    bool mounted = false;
    IOSUid uid = 0x1000;
    IOSGid gid = 0;
    Descriptor fds[kMaxFds];
};

State& state() {
    static State* instance = [] {
        PetariNative::HostAllocationScope hostAllocations;  // first use may be on a game thread
        return new State;
    }();
    return *instance;
}

using Guard = std::lock_guard<std::mutex>;

// Splits and validates an absolute NAND path.
bool parse(const char* path, std::vector<std::string>& parts) {
    parts.clear();
    if (path == nullptr || path[0] != '/' || std::strlen(path) >= kMaxPath) {
        return false;
    }
    if (path[1] == '\0') {
        return true;
    }
    const char* p = path + 1;
    while (true) {
        const char* end = std::strchr(p, '/');
        const std::size_t length = end ? static_cast<std::size_t>(end - p) : std::strlen(p);
        if (length == 0 || length > kMaxNameLength) {
            return false;
        }
        std::string name(p, length);
        if (name == "." || name == "..") {
            return false;
        }
        parts.push_back(std::move(name));
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return parts.size() <= static_cast<std::size_t>(kMaxDepth);
}

fs::path hostPath(const std::vector<std::string>& parts, std::size_t count) {
    fs::path out = state().root;
    for (std::size_t i = 0; i < count; ++i) {
        out /= parts[i];
    }
    return out;
}

fs::path hostPath(const std::vector<std::string>& parts) {
    return hostPath(parts, parts.size());
}

void encode(const Attr& a, u8 out[12]) {
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<u8>(a.ownerId >> (8 * i));
    }
    out[4] = static_cast<u8>(a.groupId);
    out[5] = static_cast<u8>(a.groupId >> 8);
    out[6] = a.attr;
    out[7] = a.ownerAccess;
    out[8] = a.groupAccess;
    out[9] = a.othersAccess;
    out[10] = out[11] = 0;
}

bool writeAttr(const fs::path& path, const Attr& a) {
    u8 raw[12];
    encode(a, raw);
    return ::setxattr(path.c_str(), kAttrName, raw, sizeof(raw), 0, XATTR_NOFOLLOW) == 0;
}

// Nodes without metadata (placed in the tree by hand) belong to the running
// title with owner and group read/write.
Attr readAttr(const fs::path& path) {
    u8 raw[12];
    const ssize_t got = ::getxattr(path.c_str(), kAttrName, raw, sizeof(raw), 0, XATTR_NOFOLLOW);
    Attr a;
    if (got != static_cast<ssize_t>(sizeof(raw))) {
        a.ownerId = state().uid;
        a.groupId = state().gid;
        a.ownerAccess = 3;
        a.groupAccess = 3;
        return a;
    }
    a.ownerId = u32(raw[0]) | (u32(raw[1]) << 8) | (u32(raw[2]) << 16) | (u32(raw[3]) << 24);
    a.groupId = static_cast<IOSGid>(raw[4] | (raw[5] << 8));
    a.attr = raw[6];
    a.ownerAccess = raw[7];
    a.groupAccess = raw[8];
    a.othersAccess = raw[9];
    return a;
}

u8 accessFor(const Attr& a) {
    const State& s = state();
    if (s.uid == 0) {
        return 3;
    }
    if (s.uid == a.ownerId) {
        return a.ownerAccess;
    }
    if (s.gid == a.groupId) {
        return a.groupAccess;
    }
    return a.othersAccess;
}

enum class Kind { Missing, File, Dir };

Kind kindOf(const fs::path& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        return Kind::Missing;
    }
    return S_ISDIR(st.st_mode) ? Kind::Dir : Kind::File;
}

// Checks that the parent directory exists and is writable by the caller.
ISFSError checkParentWritable(const std::vector<std::string>& parts) {
    const fs::path parent = hostPath(parts, parts.size() - 1);
    if (kindOf(parent) != Kind::Dir) {
        return ISFS_ERROR_NOEXISTS;
    }
    if ((accessFor(readAttr(parent)) & 2) == 0) {
        return ISFS_ERROR_ACCESS;
    }
    return ISFS_ERROR_OK;
}

ISFSError hostError(int err) {
    switch (err) {
    case ENOENT:
        return ISFS_ERROR_NOEXISTS;
    case EEXIST:
        return ISFS_ERROR_EXISTS;
    case EACCES:
    case EPERM:
        return ISFS_ERROR_ACCESS;
    case ENOSPC:
    case EDQUOT:
        return ISFS_ERROR_MAXBLOCKS;
    case ENOTEMPTY:
        return ISFS_ERROR_NOTEMPTY;
    default:
        return ISFS_ERROR_UNKNOWN;
    }
}

ISFSError createNode(const char* path, bool dir, u32 attr, u32 ownerAcc, u32 groupAcc, u32 othersAcc) {
    std::vector<std::string> parts;
    if (!parse(path, parts) || parts.empty() || ownerAcc > 3 || groupAcc > 3 || othersAcc > 3) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    if (ISFSError e = checkParentWritable(parts); e != ISFS_ERROR_OK) {
        return e;
    }
    const fs::path host = hostPath(parts);
    if (kindOf(host) != Kind::Missing) {
        return ISFS_ERROR_EXISTS;
    }
    PetariNative::HostAllocationScope hostAllocations;
    if (dir) {
        if (::mkdir(host.c_str(), 0755) != 0) {
            return hostError(errno);
        }
    } else {
        const int fd = ::open(host.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            return hostError(errno);
        }
        ::close(fd);
    }
    Attr a;
    a.ownerId = state().uid;
    a.groupId = state().gid;
    a.attr = static_cast<u8>(attr);
    a.ownerAccess = static_cast<u8>(ownerAcc);
    a.groupAccess = static_cast<u8>(groupAcc);
    a.othersAccess = static_cast<u8>(othersAcc);
    if (!writeAttr(host, a)) {
        const int err = errno;
        dir ? ::rmdir(host.c_str()) : ::unlink(host.c_str());
        return hostError(err);
    }
    return ISFS_ERROR_OK;
}

Descriptor* lookup(s32 fd) {
    if (fd < 0 || fd >= kMaxFds || !state().fds[fd].used) {
        return nullptr;
    }
    return &state().fds[fd];
}

bool hasOpenDescriptorUnder(const std::string& path) {
    for (const Descriptor& d : state().fds) {
        if (d.used && (d.path == path || (d.path.size() > path.size() && d.path.compare(0, path.size(), path) == 0 && d.path[path.size()] == '/'))) {
            return true;
        }
    }
    return false;
}

void accumulateUsage(const fs::path& dir, u64& blocks, u32& inodes) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        ++inodes;
        if (it->is_directory(ec)) {
            accumulateUsage(it->path(), blocks, inodes);
        } else {
            const u64 size = it->file_size(ec);
            blocks += (size + kClusterSize - 1) / kClusterSize;
        }
    }
}

}  // namespace

bool mount(const fs::path& root, std::string* error) {
    Guard guard(state().lock);
    PetariNative::HostAllocationScope hostAllocations;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (error) {
            *error = root.string() + " is not a directory";
        }
        return false;
    }
    if (::access(root.c_str(), R_OK | W_OK | X_OK) != 0) {
        if (error) {
            *error = root.string() + " is not writable";
        }
        return false;
    }
    if (state().mounted) {
        if (error) {
            *error = "a NAND root is already mounted";
        }
        return false;
    }
    state().root = fs::absolute(root, ec);
    state().mounted = true;
    // The root directory: system-owned, readable and writable by everyone.
    if (::getxattr(state().root.c_str(), kAttrName, nullptr, 0, 0, XATTR_NOFOLLOW) < 0) {
        Attr a;
        a.ownerAccess = 3;
        a.groupAccess = 3;
        a.othersAccess = 3;
        writeAttr(state().root, a);
    }
    return true;
}

bool isMounted() {
    Guard guard(state().lock);
    return state().mounted;
}

fs::path root() {
    Guard guard(state().lock);
    PetariNative::HostAllocationScope hostAllocations;
    return state().root;
}

void unmount() {
    Guard guard(state().lock);
    for (Descriptor& d : state().fds) {
        if (d.used) {
            ::close(d.hostFd);
        }
        PetariNative::HostAllocationScope hostAllocations;
        d = Descriptor{};
    }
    state().mounted = false;
    PetariNative::HostAllocationScope hostAllocations;
    state().root.clear();
}

void setCaller(IOSUid uid, IOSGid gid) {
    Guard guard(state().lock);
    state().uid = uid;
    state().gid = gid;
}

ISFSError ensureSystemDir(const char* path, const Attr& attr) {
    std::vector<std::string> parts;
    if (!parse(path, parts) || parts.empty()) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path host = hostPath(parts);
    switch (kindOf(host)) {
    case Kind::Dir:
        return ISFS_ERROR_OK;
    case Kind::File:
        return ISFS_ERROR_EXISTS;
    case Kind::Missing:
        break;
    }
    if (kindOf(host.parent_path()) != Kind::Dir) {
        return ISFS_ERROR_NOEXISTS;
    }
    if (::mkdir(host.c_str(), 0755) != 0) {
        return hostError(errno);
    }
    return writeAttr(host, attr) ? ISFS_ERROR_OK : hostError(errno);
}

ISFSError clearDir(const char* path) {
    std::vector<std::string> parts;
    if (!parse(path, parts)) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path host = hostPath(parts);
    if (kindOf(host) != Kind::Dir) {
        return ISFS_ERROR_NOEXISTS;
    }
    std::error_code ec;
    for (fs::directory_iterator it(host, ec), end; !ec && it != end; it.increment(ec)) {
        fs::remove_all(it->path(), ec);
        if (ec) {
            return hostError(ec.value());
        }
    }
    return ec ? hostError(ec.value()) : ISFS_ERROR_OK;
}

ISFSError createFile(const char* path, u32 attr, u32 ownerAcc, u32 groupAcc, u32 othersAcc) {
    return createNode(path, false, attr, ownerAcc, groupAcc, othersAcc);
}

ISFSError createDir(const char* path, u32 attr, u32 ownerAcc, u32 groupAcc, u32 othersAcc) {
    return createNode(path, true, attr, ownerAcc, groupAcc, othersAcc);
}

s32 open(const char* path, u32 access) {
    std::vector<std::string> parts;
    if (!parse(path, parts) || access == 0 || access > 3) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    const fs::path host = hostPath(parts);
    switch (kindOf(host)) {
    case Kind::Missing:
        return ISFS_ERROR_NOEXISTS;
    case Kind::Dir:
        return ISFS_ERROR_INVALID;
    case Kind::File:
        break;
    }
    if ((accessFor(readAttr(host)) & access) != access) {
        return ISFS_ERROR_ACCESS;
    }
    s32 slot = -1;
    for (s32 i = 0; i < kMaxFds; ++i) {
        if (!state().fds[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return ISFS_ERROR_MAXFD;
    }
    const int hostFd = ::open(host.c_str(), ((access & 2) ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (hostFd < 0) {
        return hostError(errno);
    }
    PetariNative::HostAllocationScope hostAllocations;
    Descriptor& d = state().fds[slot];
    d.used = true;
    d.hostFd = hostFd;
    d.access = access;
    d.written = false;
    d.position = 0;
    d.path = path;
    return slot;
}

ISFSError close(s32 fd) {
    Guard guard(state().lock);
    Descriptor* d = lookup(fd);
    if (d == nullptr) {
        return ISFS_ERROR_INVALID;
    }
    // IOS commits file data when the descriptor is closed.
    ISFSError result = ISFS_ERROR_OK;
    if (d->written && ::fsync(d->hostFd) != 0) {
        result = hostError(errno);
    }
    ::close(d->hostFd);
    PetariNative::HostAllocationScope hostAllocations;
    *d = Descriptor{};
    return result;
}

s32 read(s32 fd, u8* buffer, u32 length) {
    Guard guard(state().lock);
    Descriptor* d = lookup(fd);
    if (d == nullptr) {
        return ISFS_ERROR_INVALID;
    }
    if ((d->access & 1) == 0) {
        return ISFS_ERROR_ACCESS;
    }
    u32 done = 0;
    while (done < length) {
        const ssize_t got = ::pread(d->hostFd, buffer + done, length - done, static_cast<off_t>(d->position) + done);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return hostError(errno);
        }
        if (got == 0) {
            break;
        }
        done += static_cast<u32>(got);
    }
    d->position += done;
    return static_cast<s32>(done);
}

s32 write(s32 fd, const u8* buffer, u32 length) {
    Guard guard(state().lock);
    Descriptor* d = lookup(fd);
    if (d == nullptr) {
        return ISFS_ERROR_INVALID;
    }
    if ((d->access & 2) == 0) {
        return ISFS_ERROR_ACCESS;
    }
    u32 done = 0;
    while (done < length) {
        const ssize_t put = ::pwrite(d->hostFd, buffer + done, length - done, static_cast<off_t>(d->position) + done);
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            return hostError(errno);
        }
        done += static_cast<u32>(put);
    }
    d->position += done;
    d->written = true;
    return static_cast<s32>(done);
}

s32 seek(s32 fd, s32 offset, u32 whence) {
    Guard guard(state().lock);
    Descriptor* d = lookup(fd);
    if (d == nullptr) {
        return ISFS_ERROR_INVALID;
    }
    struct stat st;
    if (::fstat(d->hostFd, &st) != 0) {
        return hostError(errno);
    }
    s64 base;
    switch (whence) {
    case 0:
        base = 0;
        break;
    case 1:
        base = d->position;
        break;
    case 2:
        base = st.st_size;
        break;
    default:
        return ISFS_ERROR_INVALID;
    }
    const s64 target = base + offset;
    if (target < 0 || target > st.st_size) {
        return ISFS_ERROR_INVALID;
    }
    d->position = static_cast<u32>(target);
    return static_cast<s32>(target);
}

ISFSError fileStats(s32 fd, ISFSFileStats* stats) {
    Guard guard(state().lock);
    Descriptor* d = lookup(fd);
    if (d == nullptr) {
        return ISFS_ERROR_INVALID;
    }
    struct stat st;
    if (::fstat(d->hostFd, &st) != 0) {
        return hostError(errno);
    }
    stats->size = static_cast<u32>(st.st_size);
    stats->offset = d->position;
    return ISFS_ERROR_OK;
}

ISFSError remove(const char* path) {
    std::vector<std::string> parts;
    if (!parse(path, parts) || parts.empty()) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    if (ISFSError e = checkParentWritable(parts); e != ISFS_ERROR_OK) {
        return e;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path host = hostPath(parts);
    if (kindOf(host) == Kind::Missing) {
        return ISFS_ERROR_NOEXISTS;
    }
    if (hasOpenDescriptorUnder(path)) {
        return ISFS_ERROR_OPENFD;
    }
    std::error_code ec;
    fs::remove_all(host, ec);
    return ec ? hostError(ec.value()) : ISFS_ERROR_OK;
}

ISFSError rename(const char* oldPath, const char* newPath) {
    std::vector<std::string> from, to;
    if (!parse(oldPath, from) || !parse(newPath, to) || from.empty() || to.empty()) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    if (ISFSError e = checkParentWritable(from); e != ISFS_ERROR_OK) {
        return e;
    }
    if (ISFSError e = checkParentWritable(to); e != ISFS_ERROR_OK) {
        return e;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path source = hostPath(from);
    const fs::path target = hostPath(to);
    const Kind kind = kindOf(source);
    if (kind == Kind::Missing) {
        return ISFS_ERROR_NOEXISTS;
    }
    if (std::strcmp(oldPath, newPath) == 0) {
        return ISFS_ERROR_OK;
    }
    // IOS keeps a file's name when it moves between directories.
    const bool sameParent = std::vector<std::string>(from.begin(), from.end() - 1) == std::vector<std::string>(to.begin(), to.end() - 1);
    if (kind == Kind::File && !sameParent && from.back() != to.back()) {
        return ISFS_ERROR_INVALID;
    }
    if (std::strncmp(newPath, oldPath, std::strlen(oldPath)) == 0 && newPath[std::strlen(oldPath)] == '/') {
        return ISFS_ERROR_INVALID;  // into its own subtree
    }
    if (hasOpenDescriptorUnder(oldPath)) {
        return ISFS_ERROR_OPENFD;
    }
    // An existing destination of the same type is replaced.
    const Kind existing = kindOf(target);
    if (existing != Kind::Missing) {
        if (existing != kind) {
            return ISFS_ERROR_INVALID;
        }
        if (hasOpenDescriptorUnder(newPath)) {
            return ISFS_ERROR_OPENFD;
        }
        // POSIX rename replaces files atomically. Removing the destination first
        // would lose the old save if rename fails or the process stops here.
        // Keep the existing IOS directory-replacement behavior separately.
        if (kind == Kind::Dir) {
            std::error_code ec;
            fs::remove_all(target, ec);
            if (ec) {
                return hostError(ec.value());
            }
        }
    }
    if (::rename(source.c_str(), target.c_str()) != 0) {
        return hostError(errno);
    }
    return ISFS_ERROR_OK;
}

ISFSError getAttr(const char* path, Attr* attr) {
    std::vector<std::string> parts;
    if (!parse(path, parts)) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path host = hostPath(parts);
    if (kindOf(host) == Kind::Missing) {
        return ISFS_ERROR_NOEXISTS;
    }
    *attr = readAttr(host);
    return ISFS_ERROR_OK;
}

ISFSError getUsage(const char* path, u32* blocks, u32* inodes) {
    std::vector<std::string> parts;
    if (!parse(path, parts)) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path host = hostPath(parts);
    switch (kindOf(host)) {
    case Kind::Missing:
        return ISFS_ERROR_NOEXISTS;
    case Kind::File:
        return ISFS_ERROR_INVALID;
    case Kind::Dir:
        break;
    }
    u64 b = 0;
    u32 n = 1;  // the directory itself
    accumulateUsage(host, b, n);
    *blocks = static_cast<u32>(b);
    *inodes = n;
    return ISFS_ERROR_OK;
}

ISFSError countDir(const char* path, u32* count) {
    std::vector<std::string> parts;
    if (!parse(path, parts)) {
        return ISFS_ERROR_INVALID;
    }
    Guard guard(state().lock);
    if (!state().mounted) {
        return ISFS_ERROR_NOTREADY;
    }
    PetariNative::HostAllocationScope hostAllocations;
    const fs::path host = hostPath(parts);
    switch (kindOf(host)) {
    case Kind::Missing:
        return ISFS_ERROR_NOEXISTS;
    case Kind::File:
        return ISFS_ERROR_INVALID;
    case Kind::Dir:
        break;
    }
    if ((accessFor(readAttr(host)) & 1) == 0) {
        return ISFS_ERROR_ACCESS;
    }
    u32 n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(host, ec), end; !ec && it != end; it.increment(ec)) {
        ++n;
    }
    *count = n;
    return ec ? hostError(ec.value()) : ISFS_ERROR_OK;
}

}  // namespace PetariNative::Platform::NAND::Fs

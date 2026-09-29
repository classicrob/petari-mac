// Native NAND library.
//
// Port of the synchronous parts of src/RVL_SDK/nand/{NANDCore,nand,
// NANDOpenClose,NANDCheck}.c onto the host ISFS in isfs_host.cpp. Path
// resolution, private-path rules, permission packing, error conversion, move
// semantics, and the NANDCheck quotas are the SDK's. Differences:
// - ISFS_OpenLib is replaced by mounting a host root (explicit, or
//   PETARI_NAND_ROOT). The first NANDInit on a mount performs the console's
//   boot-time setup: system directories, clearing /tmp, and the title's data
//   directory.
// - ES identifies the running title from the mounted disc ID:
//   0x00010000'<game code>, home /title/00010000/<code hex>/data.
// - NAND error logging (NANDLoggingAddMessageAsync, which writes a log file on
//   the console) goes to OSReport.
// - The asynchronous NAND API is not implemented; the game only uses the
//   synchronous calls, from its NAND manager thread.

#include <revolution/dvd.h>
#include <revolution/esp.h>
#include <revolution/nand.h>
#include <revolution/os.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isfs_host.hpp"
#include "nand_internal.hpp"
#include "petari/platform/nand.hpp"

namespace Fs = PetariNative::Platform::NAND::Fs;

namespace {

constexpr s32 kEsInvalid = -1017;

enum LibState { STATE_NOT_INITIALIZED, STATE_WORKING, STATE_INITIALIZED };

LibState s_libState = STATE_NOT_INITIALIZED;
char s_currentDir[64] = "/";
char s_homeDir[64] = "";
bool s_booted = false;  // boot-time setup done for the current mount

// ---- Unchanged path helpers from NANDCore.c ----

void nandRemoveTailToken(char* newpath, const char* oldpath) {
    if (oldpath[0] == '/' && oldpath[1] == '\0') {
        newpath[0] = '/';
        newpath[1] = '\0';
    } else {
        for (int i = static_cast<int>(std::strlen(oldpath)) - 1; i >= 0; --i) {
            if (oldpath[i] == '/') {
                if (i != 0) {
                    std::strncpy(newpath, oldpath, static_cast<u32>(i));
                    newpath[i] = '\0';
                } else {
                    newpath[0] = '/';
                    newpath[1] = '\0';
                }
                break;
            }
        }
    }
}

void nandGetHeadToken(char* token, char* newpath, const char* oldpath) {
    for (unsigned int i = 0; i <= std::strlen(oldpath); ++i) {
        if (oldpath[i] == '/') {
            std::strncpy(token, oldpath, i);
            token[i] = '\0';
            if (oldpath[i + 1] == '\0') {
                newpath[0] = '\0';
            } else {
                std::strcpy(newpath, oldpath + i + 1);
            }
            break;
        } else if (oldpath[i] == '\0') {
            std::strncpy(token, oldpath, i);
            token[i] = '\0';
            newpath[0] = '\0';
            break;
        }
    }
}

void nandConvertPath(char* abspath, const char* wd, const char* relpath) {
    char token[128];
    char new_relpath[128];
    if (std::strlen(relpath) == 0) {
        std::strcpy(abspath, wd);
        return;
    }
    nandGetHeadToken(token, new_relpath, relpath);
    if (std::strcmp(token, ".") == 0) {
        nandConvertPath(abspath, wd, new_relpath);
    } else if (std::strcmp(token, "..") == 0) {
        char new_wd[128];
        nandRemoveTailToken(new_wd, wd);
        nandConvertPath(abspath, new_wd, new_relpath);
    } else if (token[0] != '\0') {
        char new_wd[128];
        if (std::strcmp(wd, "/") == 0) {
            std::snprintf(new_wd, sizeof(new_wd), "/%s", token);
        } else {
            std::snprintf(new_wd, sizeof(new_wd), "%s/%s", wd, token);
        }
        nandConvertPath(abspath, new_wd, new_relpath);
    } else {
        std::strcpy(abspath, wd);
    }
}

BOOL nandIsUnderPrivatePath(const char* path) {
    return (std::strncmp(path, "/shared2/", 9) == 0 && path[9] != '\0') ? TRUE : FALSE;
}

// ---- nand.c permission packing ----

BOOL nandInspectPermission(u8 perm) {
    return (perm & 0x10) ? TRUE : FALSE;
}

void nandSplitPerm(u8 perm, u32* ownerAcc, u32* groupAcc, u32* othersAcc) {
    *ownerAcc = ((perm & 0x10) ? 1 : 0) | ((perm & 0x20) ? 2 : 0);
    *groupAcc = ((perm & 0x04) ? 1 : 0) | ((perm & 0x08) ? 2 : 0);
    *othersAcc = ((perm & 0x01) ? 1 : 0) | ((perm & 0x02) ? 2 : 0);
}

u8 nandComposePerm(u32 ownerAcc, u32 groupAcc, u32 othersAcc) {
    u32 p = 0;
    p |= (ownerAcc & 1) ? 0x10 : 0;
    p |= (ownerAcc & 2) ? 0x20 : 0;
    p |= (groupAcc & 1) ? 0x04 : 0;
    p |= (groupAcc & 2) ? 0x08 : 0;
    p |= (othersAcc & 1) ? 0x01 : 0;
    p |= (othersAcc & 2) ? 0x02 : 0;
    return static_cast<u8>(p);
}

// ---- Console boot-time setup ----

Fs::Attr systemDir(u8 others) {
    Fs::Attr a;
    a.ownerAccess = 3;
    a.groupAccess = 3;
    a.othersAccess = others;
    return a;
}

bool mountFromEnvironment() {
    if (Fs::isMounted()) {
        return true;
    }
    const char* root = std::getenv(PetariNative::Platform::NAND::kRootEnvironmentVariable);
    if (root == nullptr || *root == '\0') {
        return false;
    }
    std::string error;
    if (!PetariNative::Platform::NAND::mount(root, &error)) {
        OSReport("NAND: cannot mount %s=%s: %s\n", PetariNative::Platform::NAND::kRootEnvironmentVariable, root, error.c_str());
        return false;
    }
    return true;
}

ISFSError bootFileSystem() {
    if (s_booted) {
        return ISFS_ERROR_OK;
    }
    // Directories every console has; /tmp is world-writable and emptied by IOS at boot.
    const struct {
        const char* path;
        u8 others;
    } dirs[] = {{"/title", 1}, {"/title/00010000", 1}, {"/shared2", 3}, {"/meta", 1}, {"/tmp", 3}};
    for (const auto& dir : dirs) {
        if (ISFSError e = Fs::ensureSystemDir(dir.path, systemDir(dir.others)); e != ISFS_ERROR_OK) {
            return e;
        }
    }
    if (ISFSError e = Fs::clearDir("/tmp"); e != ISFS_ERROR_OK) {
        return e;
    }
    s_booted = true;
    return ISFS_ERROR_OK;
}

IOSGid titleGroup(const DVDDiskID* id) {
    return static_cast<IOSGid>((static_cast<u8>(id->company[0]) << 8) | static_cast<u8>(id->company[1]));
}

// Creates the installed title's directories, as installing the title would.
ISFSError installTitleDirectories(ESTitleId id, IOSGid gid) {
    char titleDir[64];
    char dataDir[64];
    std::snprintf(titleDir, sizeof(titleDir), "/title/%08x/%08x", static_cast<u32>(id >> 32), static_cast<u32>(id));
    std::snprintf(dataDir, sizeof(dataDir), "%s/data", titleDir);
    if (ISFSError e = Fs::ensureSystemDir(titleDir, systemDir(1)); e != ISFS_ERROR_OK) {
        return e;
    }
    Fs::Attr data;
    data.ownerId = PetariNative::Platform::NAND::kTitleUid;
    data.groupId = gid;
    data.ownerAccess = 3;
    return Fs::ensureSystemDir(dataDir, data);
}

// ---- Operation helpers (nand.c / NANDOpenClose.c, synchronous paths) ----

ISFSError nandCreate(const char* path, u8 perm, u8 attr, BOOL privilege_flag, bool dir) {
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    if (!privilege_flag && nandIsPrivatePath(absPath)) {
        return ISFS_ERROR_ACCESS;
    }
    if (!nandInspectPermission(perm)) {
        return ISFS_ERROR_INVALID;
    }
    u32 owner = 0, group = 0, others = 0;
    nandSplitPerm(perm, &owner, &group, &others);
    return dir ? Fs::createDir(absPath, attr, owner, group, others) : Fs::createFile(absPath, attr, owner, group, others);
}

ISFSError nandDelete(const char* path, BOOL privilege_flag) {
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    if (!privilege_flag && nandIsPrivatePath(absPath)) {
        return ISFS_ERROR_ACCESS;
    }
    return Fs::remove(absPath);
}

IOSFd nandOpen(const char* path, u8 accType, BOOL privilege_flag) {
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    if (!privilege_flag && nandIsPrivatePath(absPath)) {
        return ISFS_ERROR_ACCESS;
    }
    u32 access = 0;
    switch (accType) {
    case 1:
    case 2:
    case 3:
        access = accType;
        break;
    default:
        break;
    }
    return Fs::open(absPath, access);
}

s32 openResult(IOSFd fd, NANDFileInfo* info) {
    if (fd >= 0) {
        info->fileDescriptor = fd;
        info->mark = 1;
        return NAND_RESULT_OK;
    }
    return nandConvertErrorCode(fd);
}

ISFSError nandGetStatus(const char* path, NANDStatus* stat, BOOL privilege_flag) {
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    if (!privilege_flag && nandIsUnderPrivatePath(absPath)) {
        return ISFS_ERROR_ACCESS;
    }
    Fs::Attr a;
    ISFSError result = Fs::getAttr(absPath, &a);
    if (result == ISFS_ERROR_OK) {
        stat->ownerId = a.ownerId;
        stat->groupId = a.groupId;
        stat->attribute = a.attr;
        stat->permission = nandComposePerm(a.ownerAccess, a.groupAccess, a.othersAccess);
    }
    return result;
}

const char* USER_DIR_LIST[] = {
    "/meta",          "/ticket",         "/title/00010000", "/title/00010001", "/title/00010003", "/title/00010004",
    "/title/00010005", "/title/00010006", "/title/00010007", "/shared2/title",  nullptr,
};

u32 nandCheck(u32 reqBlock, u32 reqInode, u32 homeBlock, u32 homeInode, u32 userBlock, u32 userInode) {
    u32 answer = 0;
    if (homeBlock + reqBlock > 0x400) {
        answer |= NAND_CHECK_HOME_INSSPACE;
    }
    if (homeInode + reqInode > 0x21) {
        answer |= NAND_CHECK_HOME_INSINODE;
    }
    if (userBlock + reqBlock > 0x4400) {
        answer |= NAND_CHECK_SYS_INSSPACE;
    }
    if (userInode + reqInode > 0xFA0) {
        answer |= NAND_CHECK_SYS_INSINODE;
    }
    return answer;
}

void copyComment(u16* dst, const u16* src) {
    // wcsncpy with 16-bit characters (wchar_t is 16 bits on the Wii).
    static const u16 space[] = {' ', 0};
    if (src[0] == 0) {
        src = space;
    }
    std::size_t i = 0;
    for (; i < NAND_BANNER_COMMENT_SIZE && src[i] != 0; ++i) {
        dst[i] = src[i];
    }
    for (; i < NAND_BANNER_COMMENT_SIZE; ++i) {
        dst[i] = 0;
    }
}

}  // namespace

namespace PetariNative::Platform::NAND {

ISFSError createNode(const char* path, u8 perm, u8 attr, bool privileged, bool directory) {
    return nandCreate(path, perm, attr, privileged ? TRUE : FALSE, directory);
}

ISFSError deleteNode(const char* path, bool privileged) {
    return nandDelete(path, privileged ? TRUE : FALSE);
}

bool mount(const std::filesystem::path& root, std::string* error) {
    if (!Fs::mount(root, error)) {
        return false;
    }
    s_booted = false;
    return true;
}

bool isMounted() {
    return Fs::isMounted();
}

std::filesystem::path hostPath(const char* nandPath) {
    if (!Fs::isMounted() || nandPath == nullptr || nandPath[0] != '/') {
        return {};
    }
    std::filesystem::path out = Fs::root();
    if (nandPath[1] != '\0') {
        out /= std::filesystem::path(nandPath + 1);
    }
    return out;
}

void shutdown() {
    stopAsync();
    Fs::unmount();
    BOOL enabled = OSDisableInterrupts();
    s_libState = STATE_NOT_INITIALIZED;
    std::strcpy(s_currentDir, "/");
    s_homeDir[0] = '\0';
    s_booted = false;
    OSRestoreInterrupts(enabled);
}

}  // namespace PetariNative::Platform::NAND

extern "C" {

// ---- ES title identity ----

s32 ESP_InitLib(void) {
    return 0;
}

s32 ESP_CloseLib(void) {
    return 0;
}

s32 ESP_GetTitleId(ESTitleId* titleId) {
    const DVDDiskID* id = DVDGetCurrentDiskID();
    if (id->gameName[0] == '\0') {
        return kEsInvalid;  // no disc: no running title
    }
    const u32 code = (u32(u8(id->gameName[0])) << 24) | (u32(u8(id->gameName[1])) << 16) | (u32(u8(id->gameName[2])) << 8) |
                     u32(u8(id->gameName[3]));
    *titleId = (ESTitleId(0x00010000) << 32) | code;
    return 0;
}

s32 ESP_GetDataDir(ESTitleId titleId, char* dataDir) {
    std::snprintf(dataDir, 64, "/title/%08x/%08x/data", static_cast<u32>(titleId >> 32), static_cast<u32>(titleId));
    return 0;
}

// ---- NANDCore.c ----

void nandGetRelativeName(char* name, const char* path) {
    if (std::strcmp("/", path) == 0) {
        std::strcpy(name, "");
    } else {
        int i = static_cast<int>(std::strlen(path)) - 1;
        for (; i >= 0; --i) {
            if (path[i] == '/') {
                break;
            }
        }
        std::strcpy(name, path + i + 1);
    }
}

BOOL nandIsPrivatePath(const char* path) {
    return std::strncmp(path, "/shared2", 8) == 0 ? TRUE : FALSE;
}

BOOL nandIsInitialized(void) {
    return s_libState == STATE_INITIALIZED ? TRUE : FALSE;
}

s32 nandConvertErrorCode(const ISFSError err) {
    static const int ERRMAP[] = {
        ISFS_ERROR_OK,             NAND_RESULT_OK,           ISFS_ERROR_ACCESS,         NAND_RESULT_ACCESS,
        ISFS_ERROR_CORRUPT,        NAND_RESULT_CORRUPT,      ISFS_ERROR_ECC_CRIT,       NAND_RESULT_ECC_CRIT,
        ISFS_ERROR_EXISTS,         NAND_RESULT_EXISTS,       ISFS_ERROR_HMAC,           NAND_RESULT_AUTHENTICATION,
        ISFS_ERROR_INVALID,        NAND_RESULT_INVALID,      ISFS_ERROR_MAXBLOCKS,      NAND_RESULT_MAXBLOCKS,
        ISFS_ERROR_MAXFD,          NAND_RESULT_MAXFD,        ISFS_ERROR_MAXFILES,       NAND_RESULT_MAXFILES,
        ISFS_ERROR_MAXDEPTH,       NAND_RESULT_MAXDEPTH,     ISFS_ERROR_NOEXISTS,       NAND_RESULT_NOEXISTS,
        ISFS_ERROR_NOTEMPTY,       NAND_RESULT_NOTEMPTY,     ISFS_ERROR_NOTREADY,       NAND_RESULT_UNKNOWN,
        ISFS_ERROR_OPENFD,         NAND_RESULT_OPENFD,       ISFS_ERROR_UNKNOWN,        NAND_RESULT_UNKNOWN,
        ISFS_ERROR_BUSY,           NAND_RESULT_BUSY,         ISFS_ERROR_SHUTDOWN,       NAND_RESULT_FATAL_ERROR,
        IOS_ERROR_ACCESS,          NAND_RESULT_ACCESS,       IOS_ERROR_EXISTS,          NAND_RESULT_EXISTS,
        IOS_ERROR_INTR,            NAND_RESULT_UNKNOWN,      IOS_ERROR_INVALID,         NAND_RESULT_INVALID,
        IOS_ERROR_MAX,             NAND_RESULT_UNKNOWN,      IOS_ERROR_NOEXISTS,        NAND_RESULT_NOEXISTS,
        IOS_ERROR_QEMPTY,          NAND_RESULT_UNKNOWN,      IOS_ERROR_QFULL,           NAND_RESULT_BUSY,
        IOS_ERROR_UNKNOWN,         NAND_RESULT_UNKNOWN,      IOS_ERROR_NOTREADY,        NAND_RESULT_UNKNOWN,
        IOS_ERROR_ECC,             NAND_RESULT_UNKNOWN,      IOS_ERROR_ECC_CRIT,        NAND_RESULT_ECC_CRIT,
        IOS_ERROR_BADBLOCK,        NAND_RESULT_UNKNOWN,      IOS_ERROR_INVALID_OBJTYPE, NAND_RESULT_UNKNOWN,
        IOS_ERROR_INVALID_RNG,     NAND_RESULT_UNKNOWN,      IOS_ERROR_INVALID_FLAG,    NAND_RESULT_UNKNOWN,
        IOS_ERROR_INVALID_FORMAT,  NAND_RESULT_UNKNOWN,      IOS_ERROR_INVALID_VERSION, NAND_RESULT_UNKNOWN,
        IOS_ERROR_INVALID_SIGNER,  NAND_RESULT_UNKNOWN,      IOS_ERROR_FAIL_CHECKVALUE, NAND_RESULT_UNKNOWN,
        IOS_ERROR_FAIL_INTERNAL,   NAND_RESULT_UNKNOWN,      IOS_ERROR_FAIL_ALLOC,      NAND_RESULT_ALLOC_FAILED,
        IOS_ERROR_INVALID_SIZE,    NAND_RESULT_UNKNOWN,
    };
    if (err >= 0) {
        return err;
    }
    for (std::size_t i = 0; i < sizeof(ERRMAP) / sizeof(ERRMAP[0]); i += 2) {
        if (ERRMAP[i] == err) {
            if (err == ISFS_ERROR_ECC_CRIT || err == ISFS_ERROR_HMAC || err == ISFS_ERROR_UNKNOWN || err == IOS_ERROR_UNKNOWN ||
                err == IOS_ERROR_ECC_CRIT) {
                OSReport("NAND log: ISFS error code: %d\n", err);
            }
            return ERRMAP[i + 1];
        }
    }
    OSReport("CAUTION!  Unexpected error code [%d] was found.\n", err);
    return NAND_RESULT_UNKNOWN;
}

void nandGenerateAbsPath(char* absPath, const char* path) {
    if (std::strlen(path) == 0) {
        std::strcpy(absPath, "");
    } else if (path[0] != '/') {
        char converted[128];
        nandConvertPath(converted, s_currentDir, path);
        // The SDK writes into a 64-byte buffer; longer results are invalid NAND paths.
        if (std::strlen(converted) >= 64) {
            std::strcpy(absPath, "");
        } else {
            std::strcpy(absPath, converted);
        }
    } else if (std::strlen(path) >= 64) {
        // Would overflow the SDK's 64-byte buffer; never a valid NAND path.
        std::strcpy(absPath, "");
    } else {
        std::strcpy(absPath, path);
        const std::size_t len = std::strlen(absPath);
        if (len > 1 && absPath[len - 1] == '/') {
            absPath[len - 1] = '\0';
        }
    }
}

void nandGetParentDirectory(char* parentDir, const char* absPath) {
    int i = static_cast<int>(std::strlen(absPath));
    for (; i >= 0; --i) {
        if (absPath[i] == '/') {
            break;
        }
    }
    if (i == 0) {
        std::strcpy(parentDir, "/");
    } else {
        std::strncpy(parentDir, absPath, static_cast<u32>(i));
        parentDir[i] = '\0';
    }
}

const char* nandGetHomeDir(void) {
    return s_homeDir;
}

s32 NANDInit(void) {
    BOOL enabled = OSDisableInterrupts();
    if (s_libState == STATE_WORKING) {
        OSRestoreInterrupts(enabled);
        return NAND_RESULT_BUSY;
    }
    if (s_libState == STATE_INITIALIZED) {
        OSRestoreInterrupts(enabled);
        return NAND_RESULT_OK;
    }
    s_libState = STATE_WORKING;
    OSRestoreInterrupts(enabled);

    ISFSError result = mountFromEnvironment() ? bootFileSystem() : ISFS_ERROR_NOTREADY;
    if (result != ISFS_ERROR_OK) {
        enabled = OSDisableInterrupts();
        s_libState = STATE_NOT_INITIALIZED;
        OSRestoreInterrupts(enabled);
        if (result == ISFS_ERROR_NOTREADY) {
            OSReport("NAND: no NAND root is mounted (set %s)\n", PetariNative::Platform::NAND::kRootEnvironmentVariable);
        }
        return nandConvertErrorCode(result);
    }

    ESTitleId id = 0;
    s32 rv = ESP_InitLib();
    if (rv == 0) {
        rv = ESP_GetTitleId(&id);
    }
    if (rv == 0) {
        const DVDDiskID* disk = DVDGetCurrentDiskID();
        Fs::setCaller(PetariNative::Platform::NAND::kTitleUid, titleGroup(disk));
        rv = installTitleDirectories(id, titleGroup(disk));
    }
    if (rv == 0) {
        rv = ESP_GetDataDir(id, s_homeDir);
    }
    if (rv == 0) {
        std::strcpy(s_currentDir, s_homeDir);
    }
    ESP_CloseLib();
    if (rv != 0) {
        OSReport("Failed to set home directory.\n");
    }

    enabled = OSDisableInterrupts();
    s_libState = STATE_INITIALIZED;
    OSRestoreInterrupts(enabled);
    return NAND_RESULT_OK;
}

s32 NANDGetHomeDir(char* path) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    std::strcpy(path, s_homeDir);
    return NAND_RESULT_OK;
}

void NANDInitBanner(NANDBanner* bnr, u32 flag, const u16* title, const u16* comment) {
    std::memset(bnr, 0, sizeof(NANDBanner));
    bnr->signature = NAND_BANNER_SIGNATURE;
    bnr->flag = flag;
    copyComment(bnr->comment[0], title);
    copyComment(bnr->comment[1], comment);
}

// ---- nand.c ----

s32 NANDCreate(const char* path, u8 perm, u8 attr) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandCreate(path, perm, attr, FALSE, false));
}

s32 NANDPrivateCreate(const char* path, u8 perm, u8 attr) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandCreate(path, perm, attr, TRUE, false));
}

s32 NANDPrivateCreateDir(const char* path, u8 perm, u8 attr) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandCreate(path, perm, attr, TRUE, true));
}

s32 NANDDelete(const char* path) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandDelete(path, FALSE));
}

s32 NANDPrivateDelete(const char* path) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandDelete(path, TRUE));
}

s32 NANDRead(NANDFileInfo* info, void* buf, u32 length) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(Fs::read(info->fileDescriptor, static_cast<u8*>(buf), length));
}

s32 NANDWrite(NANDFileInfo* info, const void* buf, u32 length) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(Fs::write(info->fileDescriptor, static_cast<const u8*>(buf), length));
}

s32 NANDSeek(NANDFileInfo* info, s32 offset, s32 whence) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    u32 w = 0xFFFFFFFF;
    if (whence >= 0 && whence <= 2) {
        w = static_cast<u32>(whence);
    }
    return nandConvertErrorCode(Fs::seek(info->fileDescriptor, offset, w));
}

s32 NANDMove(const char* path, const char* destDir) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    char absOldPath[64] = "";
    char absNewPath[64] = "";
    char relativeName[64] = "";
    nandGenerateAbsPath(absOldPath, path);
    nandGetRelativeName(relativeName, absOldPath);
    nandGenerateAbsPath(absNewPath, destDir);
    if (std::strcmp(absNewPath, "/") == 0) {
        std::snprintf(absNewPath, sizeof(absNewPath), "/%s", relativeName);
    } else {
        std::strncat(absNewPath, "/", sizeof(absNewPath) - std::strlen(absNewPath) - 1);
        std::strncat(absNewPath, relativeName, sizeof(absNewPath) - std::strlen(absNewPath) - 1);
    }
    if (nandIsPrivatePath(absOldPath) || nandIsPrivatePath(absNewPath)) {
        return nandConvertErrorCode(ISFS_ERROR_ACCESS);
    }
    return nandConvertErrorCode(Fs::rename(absOldPath, absNewPath));
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    ISFSFileStats stats;
    ISFSError result = Fs::fileStats(info->fileDescriptor, &stats);
    if (result == ISFS_ERROR_OK && length) {
        *length = stats.size;
    }
    return nandConvertErrorCode(result);
}

s32 NANDGetStatus(const char* path, NANDStatus* stat) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandGetStatus(path, stat, FALSE));
}

s32 NANDPrivateGetStatus(const char* path, NANDStatus* stat) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return nandConvertErrorCode(nandGetStatus(path, stat, TRUE));
}

void NANDSetUserData(NANDCommandBlock* block, void* data) {
    block->userData = data;
}

void* NANDGetUserData(const NANDCommandBlock* block) {
    return block->userData;
}

// ---- NANDOpenClose.c ----

s32 NANDOpen(const char* path, NANDFileInfo* info, u8 accType) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return openResult(nandOpen(path, accType, FALSE), info);
}

s32 NANDPrivateOpen(const char* path, NANDFileInfo* info, u8 accType) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    return openResult(nandOpen(path, accType, TRUE), info);
}

s32 NANDClose(NANDFileInfo* info) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    if (info->mark != 1) {
        return NAND_RESULT_INVALID;
    }
    ISFSError err = Fs::close(info->fileDescriptor);
    if (err == ISFS_ERROR_OK) {
        info->mark = 2;
    }
    return nandConvertErrorCode(err);
}

// ---- NANDCheck.c ----

s32 NANDCheck(u32 fsBlock, u32 inode, u32* answer) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    u32 homeBlocks = 0xFFFFFFFF, homeInodes = 0xFFFFFFFF;
    ISFSError ret = Fs::getUsage(nandGetHomeDir(), &homeBlocks, &homeInodes);
    if (ret != ISFS_ERROR_OK) {
        return nandConvertErrorCode(ret);
    }
    u32 userBlocks = 0, userInodes = 0;
    for (const char** dir = USER_DIR_LIST; *dir; ++dir) {
        u32 blk = 0, node = 0;
        ret = Fs::getUsage(*dir, &blk, &node);
        if (ret == ISFS_ERROR_OK) {
            userBlocks += blk;
            userInodes += node;
        } else if (ret != ISFS_ERROR_NOEXISTS) {
            return nandConvertErrorCode(ret);
        }
    }
    *answer = nandCheck(fsBlock, inode, homeBlocks, homeInodes, userBlocks, userInodes);
    return NAND_RESULT_OK;
}

}  // extern "C"

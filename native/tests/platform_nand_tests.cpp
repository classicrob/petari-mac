// Tests for native NAND save storage and SC system settings. Every file lives
// in temporary directories; nothing touches the user's home directory.

#include <revolution/dvd.h>
#include <revolution/nand.h>
#include <revolution/os.h>
#include <revolution/sc.h>

#include <sys/xattr.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "petari/platform/dvd.hpp"
#include "petari/platform/nand.hpp"
#include "petari/platform/sc.hpp"

namespace fs = std::filesystem;
namespace PDVD = PetariNative::Platform::DVD;
namespace PNAND = PetariNative::Platform::NAND;
namespace PSC = PetariNative::Platform::SC;

extern "C" OSThreadQueue __OSActiveThreadQueue;
extern "C" void __OSThreadInit(void);
// SDK function (nand.c) not declared in revolution/nand.h.
extern "C" s32 NANDPrivateCreateDir(const char*, u8, u8);

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

fs::path tempDir(const char* name) {
    const char* base = std::getenv("TMPDIR");
    fs::path path = fs::path(base ? base : "/tmp") / (std::string("petari_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

std::string readHost(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Synthetic disc: RMGE01, so the running title is 00010000-524d4745.
fs::path makeDisc() {
    const fs::path disc = tempDir("nand_disc");
    fs::create_directories(disc / "files");
    fs::create_directories(disc / "sys");
    std::ofstream(disc / "files" / "opening.bnr") << "banner";
    std::vector<char> boot(0x440, 0);
    std::memcpy(boot.data(), "RMGE01", 6);
    std::ofstream(disc / "sys" / "boot.bin", std::ios::binary).write(boot.data(), static_cast<std::streamsize>(boot.size()));
    return disc;
}

// NANDManagerThread::executeWriteSequence, verbatim in behaviour.
s32 writeSequence(const char* path, const void* data, u32 size, u8 perm, u32* written) {
    s32 result = NANDCreate(path, perm, 0);
    *written = 0;
    if (result == NAND_RESULT_OK || result == NAND_RESULT_EXISTS) {
        NANDFileInfo info;
        result = NANDOpen(path, &info, NAND_ACCESS_WRITE);
        if (result == NAND_RESULT_OK) {
            result = NANDWrite(&info, data, size);
            if (result < NAND_RESULT_OK) {
                NANDClose(&info);
            } else {
                *written = static_cast<u32>(result);
                result = NANDClose(&info);
            }
        }
    }
    return result;
}

// NANDManagerThread::executeReadSequence.
s32 readSequence(const char* path, void* buffer, u32 capacity, u32* length) {
    NANDFileInfo info;
    s32 result = NANDOpen(path, &info, NAND_ACCESS_READ);
    if (result != NAND_RESULT_OK) {
        return result;
    }
    result = NANDGetLength(&info, length);
    if (result != NAND_RESULT_OK) {
        NANDClose(&info);
        return result;
    }
    if (capacity < *length) {
        NANDClose(&info);
        return NAND_RESULT_AUTHENTICATION;
    }
    result = NANDRead(&info, buffer, *length);
    if (result < NAND_RESULT_OK) {
        NANDClose(&info);
        return result;
    }
    return NANDClose(&info);
}

const char* kHome = "/title/00010000/524d4745/data";

void testWithoutRoot() {
    ::unsetenv(PNAND::kRootEnvironmentVariable);
    check(NANDInit() == NAND_RESULT_UNKNOWN, "NANDInit without a NAND root fails");
    check(NANDCreate("GameData.bin", 0x3C, 0) == NAND_RESULT_FATAL_ERROR, "NAND calls before a successful NANDInit fail");
    char home[64];
    check(NANDGetHomeDir(home) == NAND_RESULT_FATAL_ERROR, "no home directory before NANDInit");
}

void testSaveData(const fs::path& root) {
    // Stale /tmp content from a previous "boot".
    fs::create_directories(root / "tmp");
    std::ofstream(root / "tmp" / "stale.bin") << "old";

    std::string error;
    check(PNAND::mount(root, &error), "mount NAND root");
    check(NANDInit() == NAND_RESULT_OK && NANDInit() == NAND_RESULT_OK, "NANDInit succeeds and is idempotent");
    char home[64];
    check(NANDGetHomeDir(home) == NAND_RESULT_OK && std::strcmp(home, kHome) == 0, "home directory from the disc's title ID");
    check(fs::is_directory(root / "title" / "00010000" / "524d4745" / "data"), "title data directory installed on the host");
    check(!fs::exists(root / "tmp" / "stale.bin") && fs::is_directory(root / "tmp"), "boot clears /tmp");

    // Game save file (SaveDataHandler: GameData.bin, permission 60).
    std::vector<u8> save(0x4000);
    for (std::size_t i = 0; i < save.size(); ++i) {
        save[i] = static_cast<u8>(i * 13 + 5);
    }
    u32 written = 0;
    check(writeSequence("GameData.bin", save.data(), static_cast<u32>(save.size()), 60, &written) == NAND_RESULT_OK && written == 0x4000,
          "write sequence creates and writes GameData.bin");
    const fs::path hostSave = root / "title" / "00010000" / "524d4745" / "data" / "GameData.bin";
    check(readHost(hostSave) == std::string(save.begin(), save.end()), "save bytes land in the host file");
    check(NANDCreate("GameData.bin", 60, 0) == NAND_RESULT_EXISTS, "creating an existing file reports EXISTS");

    std::vector<u8> back(0x10000);
    u32 length = 0;
    check(readSequence("GameData.bin", back.data(), static_cast<u32>(back.size()), &length) == NAND_RESULT_OK && length == 0x4000 &&
              std::memcmp(back.data(), save.data(), save.size()) == 0,
          "read sequence returns the saved data");
    check(readSequence("GameData.bin", back.data(), 0x100, &length) == NAND_RESULT_AUTHENTICATION, "undersized read buffer, as the game checks");
    check(readSequence("missing.bin", back.data(), 0x100, &length) == NAND_RESULT_NOEXISTS, "reading a missing file");

    // Rewriting a shorter save keeps the old tail, as on the console (NANDWrite
    // overwrites in place; the game writes a fixed-size file).
    std::vector<u8> shorter(0x20, 0xAA);
    check(writeSequence("GameData.bin", shorter.data(), 0x20, 60, &written) == NAND_RESULT_OK, "rewrite");
    NANDFileInfo info;
    check(NANDOpen("GameData.bin", &info, NAND_ACCESS_RW) == NAND_RESULT_OK, "open read-write");
    check(NANDGetLength(&info, &length) == NAND_RESULT_OK && length == 0x4000, "length unchanged by an in-place rewrite");
    check(NANDSeek(&info, 0, 2) == 0x4000 && NANDRead(&info, back.data(), 16) == 0, "seek to end, read returns 0");
    check(NANDSeek(&info, 0x4001, 0) == NAND_RESULT_INVALID, "seek past the end is invalid");
    check(NANDSeek(&info, 0x10, 0) == 0x10 && NANDRead(&info, back.data(), 0x20) == 0x20 && back[0] == 0xAA && back[0x10] == save[0x20],
          "seek and read across the rewritten boundary");
    check(NANDWrite(&info, shorter.data(), 0x20) == 0x20, "write through a read-write descriptor");
    check(NANDClose(&info) == NAND_RESULT_OK && NANDClose(&info) == NAND_RESULT_INVALID, "double close is invalid");

    NANDFileInfo readOnly, writeOnly;
    check(NANDOpen("GameData.bin", &readOnly, NAND_ACCESS_READ) == NAND_RESULT_OK, "open read-only");
    check(NANDWrite(&readOnly, save.data(), 4) == NAND_RESULT_ACCESS, "write through a read-only descriptor is refused");
    check(NANDOpen("GameData.bin", &writeOnly, NAND_ACCESS_WRITE) == NAND_RESULT_OK, "open write-only");
    check(NANDRead(&writeOnly, back.data(), 4) == NAND_RESULT_ACCESS, "read through a write-only descriptor is refused");
    check(NANDDelete("GameData.bin") == NAND_RESULT_OPENFD, "deleting an open file is refused");
    NANDClose(&readOnly);
    NANDClose(&writeOnly);

    NANDStatus status;
    check(NANDGetStatus("GameData.bin", &status) == NAND_RESULT_OK, "status");
    check(status.ownerId == PNAND::kTitleUid && status.groupId == 0x3031 && status.permission == 60 && status.attribute == 0,
          "status reports the title owner, maker group, and permission");

    // Banner (SaveDataBannerCreator): build, stage in /tmp, move home.
    static NANDBanner banner;
    const u16 title[] = {'S', 'u', 'p', 'e', 'r', ' ', 'M', 'a', 'r', 'i', 'o', 0};
    const u16 empty[] = {0};
    NANDInitBanner(&banner, NAND_BANNER_FLAG_ANIM_LOOP, title, empty);
    NANDSetIconSpeed(&banner, 0, NAND_BANNER_ICON_ANIM_SPEED_SLOW);
    check(banner.signature == NAND_BANNER_SIGNATURE && banner.comment[0][0] == 'S' && banner.comment[0][10] == 'o' &&
              banner.comment[0][11] == 0 && banner.comment[1][0] == ' ' && banner.comment[1][1] == 0,
          "NANDInitBanner fills 16-bit comments, empty becomes a space");
    check(writeSequence("/tmp/banner.bin", &banner, NAND_BANNER_SIZE(1), NAND_PERM_RGRP | NAND_PERM_WGRP | NAND_PERM_RUSR | NAND_PERM_WUSR,
                        &written) == NAND_RESULT_OK &&
              written == NAND_BANNER_SIZE(1),
          "banner staged in /tmp");
    check(NANDMove("/tmp/banner.bin", home) == NAND_RESULT_OK, "banner moved home");
    check(!fs::exists(root / "tmp" / "banner.bin") && fs::file_size(hostSave.parent_path() / "banner.bin") == NAND_BANNER_SIZE(1),
          "move relocates the file");
    check(NANDMove("/tmp/banner.bin", home) == NAND_RESULT_NOEXISTS, "moving a missing file");
    writeSequence("/tmp/banner.bin", &banner, 64, 0x3C, &written);
    check(NANDMove("/tmp/banner.bin", home) == NAND_RESULT_OK && fs::file_size(hostSave.parent_path() / "banner.bin") == 64,
          "move replaces an existing file");

    // Access rules.
    check(NANDCreate("/shared2/x.bin", 0x3C, 0) == NAND_RESULT_ACCESS, "public API refuses /shared2");
    check(NANDPrivateCreate("/shared2/x.bin", 0x3C, 0) == NAND_RESULT_OK, "private API may create under /shared2");
    check(NANDCreate("noread.bin", 0x20, 0) == NAND_RESULT_INVALID, "permission without owner read is invalid");
    check(NANDCreate("ro.bin", 0x10, 0) == NAND_RESULT_OK, "owner read-only file");
    check(NANDOpen("ro.bin", &info, NAND_ACCESS_WRITE) == NAND_RESULT_ACCESS, "write access to a read-only file is refused");
    check(NANDCreate("/title/00010000/evil.bin", 0x3C, 0) == NAND_RESULT_ACCESS, "system directories are not writable by the title");
    check(NANDCreate("../outside.bin", 0x3C, 0) == NAND_RESULT_ACCESS, "relative path out of home resolves into a system directory");
    check(NANDCreate("abcdefghijklm", 0x3C, 0) == NAND_RESULT_INVALID, "names over 12 characters are invalid");
    check(NANDCreate("/a/b/c/d/e/f/g/h/i", 0x3C, 0) == NAND_RESULT_INVALID, "paths deeper than 8 levels are invalid");
    check(NANDCreate("nodir/x.bin", 0x3C, 0) == NAND_RESULT_NOEXISTS, "missing parent directory");
    char longPath[80];
    std::memset(longPath, 'a', sizeof(longPath) - 1);
    longPath[0] = '/';
    longPath[79] = '\0';
    check(NANDCreate(longPath, 0x3C, 0) == NAND_RESULT_INVALID, "paths of 64 bytes or more are invalid");
    check(NANDPrivateCreateDir("sub", 0x3C, 0) == NAND_RESULT_OK && NANDCreate("sub/in.bin", 0x3C, 0) == NAND_RESULT_OK,
          "directory creation and nested file");
    check(NANDDelete("sub") == NAND_RESULT_OK && !fs::exists(hostSave.parent_path() / "sub"), "delete is recursive");
    check(NANDDelete("sub") == NAND_RESULT_NOEXISTS, "deleting a missing node");

    // Descriptor limit.
    NANDFileInfo many[16];
    int opened = 0;
    for (auto& f : many) {
        if (NANDOpen("banner.bin", &f, NAND_ACCESS_READ) == NAND_RESULT_OK) {
            ++opened;
        }
    }
    check(opened == 15, "at most 15 descriptors are open");
    check(NANDOpen("banner.bin", &info, NAND_ACCESS_READ) == NAND_RESULT_MAXFD, "16th open reports MAXFD");
    for (int i = 0; i < 15; ++i) {
        NANDClose(&many[i]);
    }

    // Capacity check used before saving.
    u32 answer = 0xFFFF;
    check(NANDCheck(4, 2, &answer) == NAND_RESULT_OK && answer == 0, "small request fits");
    check(NANDCheck(0x400, 0, &answer) == NAND_RESULT_OK && answer == NAND_CHECK_HOME_INSSPACE, "home block quota");
    check(NANDCheck(0, 0x21, &answer) == NAND_RESULT_OK && (answer & NAND_CHECK_HOME_INSINODE), "home inode quota");

    // Metadata is stored with the host file.
    char raw[12];
    check(::getxattr(hostSave.c_str(), "com.petari.nand", raw, sizeof(raw), 0, 0) == 12, "NAND metadata stored as an xattr");

    // Remount: saves persist, /tmp is cleared again.
    writeSequence("/tmp/left.bin", save.data(), 32, 0x3C, &written);
    PNAND::shutdown();
    check(NANDCreate("x", 0x3C, 0) == NAND_RESULT_FATAL_ERROR, "shutdown uninitialises the library");
    check(PNAND::mount(root, &error) && NANDInit() == NAND_RESULT_OK, "remount");
    check(readSequence("GameData.bin", back.data(), static_cast<u32>(back.size()), &length) == NAND_RESULT_OK && length == 0x4000 &&
              back[0] == 0xAA,
          "saved data persists across boots");
    check(!fs::exists(root / "tmp" / "left.bin"), "/tmp is cleared on the next boot");
}

// ---- Asynchronous NAND, as RVLFaceLib uses it for the Mii database ----

OSMessageQueue gNandDone;
OSMessage gNandDoneSlots[4];
struct Completion {
    s32 result;
    void* userData;
    bool interruptsDisabled;
};
Completion gLast;

void asyncCallback(s32 result, NANDCommandBlock* block) {
    gLast.result = result;
    gLast.userData = NANDGetUserData(block);
    gLast.interruptsDisabled = OSDisableInterrupts() == FALSE;
    OSSendMessage(&gNandDone, nullptr, OS_MESSAGE_NOBLOCK);
}

s32 await(s32 immediate) {
    check(immediate == NAND_RESULT_OK, "asynchronous request accepted");
    OSMessage msg;
    OSReceiveMessage(&gNandDone, &msg, OS_MESSAGE_BLOCK);
    check(gLast.interruptsDisabled, "NAND callback runs with interrupts disabled");
    return gLast.result;
}

void testAsync(const fs::path& root) {
    OSInitMessageQueue(&gNandDone, gNandDoneSlots, 4);
    static NANDCommandBlock block;
    static NANDFileInfo file;
    alignas(32) static u8 copyBuffer[0x4000];
    int tag = 0;
    NANDSetUserData(&block, &tag);
    const char* db = "/shared2/menu/FaceLib/RFL_DB.dat";

    // Boot without a database: the open is accepted; the callback reports it missing.
    check(await(NANDPrivateSafeOpenAsync(db, &file, NAND_ACCESS_READ, copyBuffer, sizeof(copyBuffer), asyncCallback, &block)) == NAND_RESULT_NOEXISTS,
          "missing file is reported to the callback");
    check(gLast.userData == &tag, "callback receives the command block with its user data");

    check(await(NANDPrivateCreateDirAsync("/shared2/menu", NAND_PERM_RWALL, 0, asyncCallback, &block)) == NAND_RESULT_OK, "create directory");
    check(await(NANDPrivateCreateDirAsync("/shared2/menu/FaceLib", NAND_PERM_RWALL, 0, asyncCallback, &block)) == NAND_RESULT_OK, "nested directory");
    check(await(NANDPrivateCreateAsync(db, NAND_PERM_RWALL, 0, asyncCallback, &block)) == NAND_RESULT_OK, "create database file");
    check(await(NANDPrivateCreateAsync(db, NAND_PERM_RWALL, 0, asyncCallback, &block)) == NAND_RESULT_EXISTS, "second create reports EXISTS");
    check(NANDPrivateCreateAsync(db, 0x20, 0, asyncCallback, &block) == NAND_RESULT_INVALID, "invalid permission is refused immediately");

    // Writing safe open: edits happen on a copy that replaces the file on close.
    check(await(NANDPrivateSafeOpenAsync(db, &file, NAND_ACCESS_RW, copyBuffer, sizeof(copyBuffer), asyncCallback, &block)) == NAND_RESULT_OK,
          "safe open for read/write");
    check(std::string(file.tmpPath).rfind("/tmp/sys/", 0) == 0 && fs::exists(PNAND::hostPath(file.tmpPath)), "working copy under /tmp/sys");
    std::vector<u8> data(100);
    for (int i = 0; i < 100; ++i) {
        data[i] = static_cast<u8>(i * 3);
    }
    check(await(NANDWriteAsync(&file, data.data(), 100, asyncCallback, &block)) == 100, "asynchronous write");
    check(fs::file_size(PNAND::hostPath(db)) == 0, "original untouched until close");
    check(await(NANDSeekAsync(&file, 0, 0 /* SEEK_SET */, asyncCallback, &block)) == 0, "asynchronous seek");
    std::vector<u8> back(100);
    check(await(NANDReadAsync(&file, back.data(), 100, asyncCallback, &block)) == 100 && back == data, "asynchronous read");
    u32 length = 0;
    check(await(NANDGetLengthAsync(&file, &length, asyncCallback, &block)) == NAND_RESULT_OK && length == 100, "asynchronous length");
    const std::string tmpDir = fs::path(PNAND::hostPath(file.tmpPath)).parent_path().string();
    check(await(NANDSafeCloseAsync(&file, asyncCallback, &block)) == NAND_RESULT_OK, "safe close commits");
    check(fs::file_size(PNAND::hostPath(db)) == 100 && !fs::exists(tmpDir), "copy replaced the original and the work directory is gone");
    check(NANDSafeCloseAsync(&file, asyncCallback, &block) == NAND_RESULT_INVALID, "closing twice is refused immediately");

    // Boot with a database.
    check(await(NANDPrivateSafeOpenAsync(db, &file, NAND_ACCESS_READ, copyBuffer, sizeof(copyBuffer), asyncCallback, &block)) == NAND_RESULT_OK,
          "safe open for reading");
    std::fill(back.begin(), back.end(), 0);
    check(await(NANDReadAsync(&file, back.data(), 100, asyncCallback, &block)) == 100 && back == data, "read the committed data");
    check(await(NANDSafeCloseAsync(&file, asyncCallback, &block)) == NAND_RESULT_OK, "close after reading");

    check(await(NANDPrivateDeleteAsync(db, asyncCallback, &block)) == NAND_RESULT_OK && !fs::exists(PNAND::hostPath(db)), "asynchronous delete");
    (void)root;
}

// The game performs NAND requests on its NAND manager OS thread.
struct Request {
    const u8* data;
    u32 size;
    s32 result;
    u32 written;
};

void* nandManager(void* param) {
    auto* request = static_cast<Request*>(param);
    request->result = writeSequence("thread.bin", request->data, request->size, 60, &request->written);
    return nullptr;
}

void testOnOsThread() {
    static OSThread thread;
    alignas(32) static u8 stack[0x8000];
    std::vector<u8> data(0x2000, 0x5C);
    Request request{data.data(), static_cast<u32>(data.size()), -1, 0};
    check(OSCreateThread(&thread, nandManager, &request, stack + sizeof(stack), sizeof(stack), 13, 0), "create NAND manager thread");
    OSResumeThread(&thread);
    OSJoinThread(&thread, nullptr);
    check(request.result == NAND_RESULT_OK && request.written == 0x2000, "NAND write sequence on an OS thread");
}

std::atomic<int> gFlushResult{-1};
void flushDone(u32 result) {
    gFlushResult = static_cast<int>(result);
}

void testSettings(const fs::path& dir) {
    PSC::reset();
    SCInit();
    check(SCCheckStatus() == SC_STATUS_OK, "SC ready");
    check(SCGetLanguage() == SC_LANG_ENGLISH && SCGetAspectRatio() == SC_ASPECT_RATIO_16x9 &&
              SCGetProgressiveMode() == SC_PROGRESSIVE_MODE_ON && SCGetSoundMode() == SC_SOUND_MODE_STEREO,
          "native defaults: English, 16:9, progressive, stereo");
    check(SCGetEuRgb60Mode() == SC_EURGB60_MODE_OFF && SCGetWpadSensorBarPosition() == 0 && SCGetBtDpdSensibility() == 3, "other defaults");

    std::string error;
    PSC::Settings s = PSC::current();
    s.language = 10;
    check(!PSC::set(s, &error) && error.find("language") != std::string::npos, "invalid language is rejected");
    s = PSC::current();
    s.displayOffsetH = 40;
    check(!PSC::set(s, &error) && error.find("offset") != std::string::npos, "invalid display offset is rejected");
    s = PSC::current();
    s.language = SC_LANG_FRENCH;
    s.aspectRatio = SC_ASPECT_RATIO_4x3;
    s.soundMode = SC_SOUND_MODE_SURROUND;
    check(PSC::set(s, &error), "valid settings are accepted");
    check(SCGetLanguage() == SC_LANG_FRENCH && SCGetAspectRatio() == SC_ASPECT_RATIO_4x3 && SCGetSoundMode() == SC_SOUND_MODE_SURROUND,
          "SC getters see new settings");
    check(!SCSetWpadSpeakerVolume(200) && SCGetWpadSpeakerVolume() == 0x58, "out-of-range speaker volume is refused");
    check(SCSetWpadSpeakerVolume(100) && SCGetWpadSpeakerVolume() == 100, "speaker volume setter");
    check(SCSetWpadMotorMode(0) && SCGetWpadMotorMode() == 0, "motor mode setter");

    SCFlushAsync(flushDone);
    check(gFlushResult == static_cast<int>(SC_STATUS_ERROR), "flush without a store reports an error");

    const fs::path store = dir / "settings.txt";
    check(PSC::setStore(store, &error) && !fs::exists(store), "store path configured; file created on flush");
    gFlushResult = -1;
    SCFlushAsync(flushDone);
    check(gFlushResult == static_cast<int>(SC_STATUS_OK) && readHost(store).find("speaker_volume=100") != std::string::npos,
          "flush persists settings");

    PSC::reset();
    check(SCGetWpadSpeakerVolume() == 0x58, "reset restores defaults");
    check(PSC::setStore(store, &error) && SCGetWpadSpeakerVolume() == 100 && SCGetLanguage() == SC_LANG_FRENCH, "store is loaded");

    std::ofstream(store) << "language=12\n";
    check(!PSC::setStore(store, &error) && error.find("language") != std::string::npos, "out-of-range stored value is rejected");
    SCInit();
    check(SCCheckStatus() == SC_STATUS_ERROR, "SCCheckStatus reports a bad settings file");
    std::ofstream(store) << "volume=3\n";
    check(!PSC::setStore(store, &error) && error.find("line 1") != std::string::npos, "unknown key is rejected");
    std::ofstream(store) << "speaker_volume=300\n";
    check(!PSC::setStore(store, &error), "values that would wrap are rejected");
    PSC::reset();
}

}  // namespace

int main() {
    __OSThreadInit();
    const fs::path disc = makeDisc();
    std::string error;
    check(PDVD::mount({disc}, &error), "mount synthetic disc");
    DVDInit();

    testWithoutRoot();
    const fs::path nandRoot = tempDir("nand_root");
    testSaveData(nandRoot);
    testOnOsThread();
    testAsync(nandRoot);
    PNAND::shutdown();
    testSettings(tempDir("sc"));

    PDVD::shutdown();
    fs::remove_all(nandRoot);
    fs::remove_all(disc);
    OSReport("platform NAND/SC tests passed (%d checks)\n", checks);
    return 0;
}

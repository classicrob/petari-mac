// Tests for the native DVD service using synthetic extracted discs in
// temporary directories. No retail data is used.

#include <revolution/dvd.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dvd_internal.hpp"
#include "petari/platform/dvd.hpp"

namespace fs = std::filesystem;
namespace PDVD = PetariNative::Platform::DVD;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

bool waitFor(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::string pattern(std::size_t size, unsigned seed) {
    std::string s(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        s[i] = static_cast<char>((i * 131 + seed * 17 + (i >> 8)) & 0xFF);
    }
    return s;
}

struct Node {
    std::string name;
    bool dir = false;
    std::string data;
    std::vector<Node> children;
};

Node file(std::string name, std::string data) {
    return {std::move(name), false, std::move(data), {}};
}
Node dir(std::string name, std::vector<Node> children) {
    return {std::move(name), true, {}, std::move(children)};
}

void writeHostTree(const fs::path& path, const Node& node) {
    for (const Node& child : node.children) {
        if (child.dir) {
            fs::create_directories(path / child.name);
            writeHostTree(path / child.name, child);
        } else {
            std::ofstream(path / child.name, std::ios::binary).write(child.data.data(), static_cast<std::streamsize>(child.data.size()));
        }
    }
}

void putU32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    out[at] = static_cast<std::uint8_t>(v >> 24);
    out[at + 1] = static_cast<std::uint8_t>(v >> 16);
    out[at + 2] = static_cast<std::uint8_t>(v >> 8);
    out[at + 3] = static_cast<std::uint8_t>(v);
}

// Serializes a Wii fst.bin in the given child order. File positions are word
// addresses starting at firstPosition, each file 0x8000-byte aligned.
struct FstWriter {
    struct Raw {
        bool dir;
        std::uint32_t nameOffset, a, b;
    };
    std::vector<Raw> entries;
    std::string strings;
    std::uint64_t cursor;
    std::vector<std::uint32_t> positions;  // disc byte offset of each file entry (0 for dirs)

    explicit FstWriter(std::uint64_t firstByte) : cursor(firstByte) {}

    void add(const Node& node, std::uint32_t parent) {
        for (const Node& child : node.children) {
            const std::uint32_t index = static_cast<std::uint32_t>(entries.size());
            const std::uint32_t nameOffset = static_cast<std::uint32_t>(strings.size());
            strings += child.name;
            strings += '\0';
            if (child.dir) {
                entries.push_back({true, nameOffset, parent, 0});
                positions.push_back(0);
                add(child, index);
                entries[index].b = static_cast<std::uint32_t>(entries.size());
            } else {
                entries.push_back({false, nameOffset, static_cast<std::uint32_t>(cursor >> 2), static_cast<std::uint32_t>(child.data.size())});
                positions.push_back(static_cast<std::uint32_t>(cursor));
                cursor += (child.data.size() + 0x7FFF) / 0x8000 * 0x8000 + 0x8000;
            }
        }
    }

    std::vector<std::uint8_t> build(const Node& root) {
        entries.push_back({true, 0, 0, 0});
        positions.push_back(0);
        add(root, 0);
        entries[0].b = static_cast<std::uint32_t>(entries.size());
        std::vector<std::uint8_t> out(entries.size() * 12);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Raw& e = entries[i];
            putU32(out, i * 12, e.nameOffset);
            out[i * 12] = e.dir ? 1 : 0;
            putU32(out, i * 12 + 4, e.a);
            putU32(out, i * 12 + 8, e.b);
        }
        out.insert(out.end(), strings.begin(), strings.end());
        return out;
    }
};

void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> bootBin() {
    std::vector<std::uint8_t> boot(0x440, 0);
    std::memcpy(&boot[0], "RMGK01", 6);
    boot[6] = 0;
    boot[7] = 1;
    putU32(boot, 0x18, 0x5D1C9EA3);
    return boot;
}

// Disc layout used by most tests, in Nintendo FST order (upper-cased names).
Node sampleDisc() {
    return dir("", {
        dir("AudioRes", {file("Seq.arc", pattern(5000, 1)), file("Wave.aw", pattern(0x80000 + 77, 2))}),
        file("opening.bnr", pattern(96, 3)),
        dir("StageData", {
            dir("AstroGalaxy", {file("AstroGalaxy.arc", pattern(40000, 4))}),
            file("empty.bin", ""),
            dir("HeavensDoorGalaxy", {file("HeavensDoorGalaxy.arc", pattern(333, 5))}),
        }),
    });
}

struct TempDir {
    fs::path path;
    TempDir() {
        const char* base = std::getenv("TMPDIR");
        path = fs::path(base ? base : "/tmp") / ("petari_dvd_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    static int& counter() {
        static int value = 0;
        return value;
    }
};

// Extracted disc with sys/fst.bin and sys/boot.bin. Returns the byte offsets
// of file entries from the generated FST.
std::vector<std::uint32_t> makeExtractedDisc(const fs::path& root, const Node& disc) {
    fs::create_directories(root / "files");
    fs::create_directories(root / "sys");
    writeHostTree(root / "files", disc);
    FstWriter writer(0x00440000);
    writeBytes(root / "sys" / "fst.bin", writer.build(disc));
    writeBytes(root / "sys" / "boot.bin", bootBin());
    return writer.positions;
}

void mountOrFail(const PDVD::MountOptions& options) {
    std::string error;
    if (!PDVD::mount(options, &error)) {
        std::fprintf(stderr, "mount error: %s\n", error.c_str());
        check(false, "mount succeeds");
    }
}

std::string readSync(DVDFileInfo& info, s32 length, s32 offset, s32* result = nullptr) {
    std::vector<char> buffer(static_cast<std::size_t>(length) + 64, '\x5A');
    const s32 got = DVDReadPrio(&info, buffer.data(), length, offset, 2);
    if (result) {
        *result = got;
    }
    check(buffer[static_cast<std::size_t>(length)] == '\x5A', "read does not write past the requested length");
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

// Runs fn in a child process and reports whether it aborted.
bool aborts(const std::function<void()>& fn) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        fn();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

void testFstMount() {
    TempDir tmp;
    const Node disc = sampleDisc();
    const std::vector<std::uint32_t> positions = makeExtractedDisc(tmp.path, disc);
    mountOrFail({tmp.path});
    DVDInit();

    const PDVD::MountInfo info = PDVD::mountInfo();
    check(info.source == PDVD::FstSource::DiscFst, "fst.bin is used when present");
    check(info.entryCount == 11 && info.fileCount == 6, "entry and file counts from fst.bin");
    check(info.hasDiskId, "boot.bin provides the disc ID");
    std::string error;
    check(!PDVD::mount({tmp.path}, &error) && !error.empty(), "remount while mounted is refused");

    // Entry numbers follow fst.bin order.
    check(DVDConvertPathToEntrynum("/AudioRes") == 1, "directory entry number");
    check(DVDConvertPathToEntrynum("/AudioRes/Wave.aw") == 3, "file entry number");
    check(DVDConvertPathToEntrynum("/StageData/HeavensDoorGalaxy/HeavensDoorGalaxy.arc") == 10, "nested file entry number");
    check(DVDConvertPathToEntrynum("/stagedata/astrogalaxy/ASTROGALAXY.ARC") == 7, "lookup is ASCII case-insensitive");
    check(DVDConvertPathToEntrynum("StageData/./AstroGalaxy/../empty.bin") == 8, "'.' and '..' components");
    check(DVDConvertPathToEntrynum("/StageData/") == 5, "trailing slash names a directory");
    check(DVDConvertPathToEntrynum("/opening.bnr/") == -1, "trailing slash does not match a file");
    check(DVDConvertPathToEntrynum("/StageData/AstroGalaxy.arc") == -1, "file in wrong directory is not found");
    check(DVDConvertPathToEntrynum("/") == 0 && DVDConvertPathToEntrynum("") == 0, "root path");
    check(DVDConvertPathToEntrynum("/opening") == -1, "prefix of a name does not match");
    check(PDVD::hostPathForEntry(4) == tmp.path / "files" / "opening.bnr", "entry maps to its host file");
    check(PDVD::hostPathForEntry(1).empty() && PDVD::hostPathForEntry(99).empty(), "directories and bad entries have no host file");

    char cwd[16];
    check(DVDGetCurrentDir(cwd, sizeof(cwd)) && std::strcmp(cwd, "/") == 0, "current directory is the root");

    DVDDiskID* id = DVDGetCurrentDiskID();
    check(std::memcmp(id->gameName, "RMGK", 4) == 0 && std::memcmp(id->company, "01", 2) == 0, "disc ID name and maker");
    check(id->gameVersion == 1 && id->rvlMagic == 0x5D1C9EA3 && id->gcMagic == 0, "disc ID fields are host-endian");
    DVDDiskID same = *id;
    check(DVDCompareDiskID(id, &same), "SDK disc ID comparison links");

    DVDFileInfo info2;
    check(!DVDOpen("/StageData", &info2), "DVDOpen refuses directories");
    check(!DVDOpen("/missing.arc", &info2), "DVDOpen of a missing file fails");
    check(!DVDFastOpen(-1, &info2) && !DVDFastOpen(11, &info2) && !DVDFastOpen(5, &info2), "DVDFastOpen validates the entry");

    DVDFileInfo seq;
    check(DVDOpen("/AudioRes/Seq.arc", &seq), "DVDOpen a file");
    check(seq.startAddr == positions[2] >> 2 && seq.length == 5000, "file info carries the disc word address and length");
    check(DVDGetCommandBlockStatus(&seq.cb) == DVD_STATE_END, "opened file is idle");
    s32 result = 0;
    check(readSync(seq, 5000, 0, &result) == disc.children[0].children[0].data && result == 5000, "whole-file synchronous read");
    check(readSync(seq, 928, 4096) == disc.children[0].children[0].data.substr(4096, 904) + std::string(24, '\0'),
          "read past end within 32 bytes returns data then zero padding");
    check(readSync(seq, 32, 128, &result) == disc.children[0].children[0].data.substr(128, 32) && result == 32, "offset read");
    check(seq.cb.state == DVD_STATE_END && seq.cb.transferredSize == 32, "sync read leaves the block finished");
    check(DVDClose(&seq), "DVDClose");

    DVDFileInfo wave;
    check(DVDFastOpen(3, &wave), "DVDFastOpen a file");
    PDVD::Testing::setChunkSize(0x1000);
    check(readSync(wave, 0x80000 + 96, 0, &result) == disc.children[0].children[1].data + std::string(19, '\0') && result == 0x80000 + 96,
          "multi-chunk read with 32-byte rounded length");
    PDVD::Testing::setChunkSize(0x80000);

    DVDFileInfo empty;
    check(DVDFastOpen(8, &empty) && empty.length == 0, "empty file opens");
    check(readSync(empty, 28, 0, &result) == std::string(28, '\0') && result == 28, "empty file reads as padding");
    check(readSync(empty, 0, 0, &result).empty() && result == 0, "zero-length read completes");

    // Directory enumeration returns immediate children in FST order.
    DVDDir d;
    DVDDirEntry entry;
    std::vector<std::string> names;
    check(DVDOpenDir("/StageData", &d), "DVDOpenDir");
    while (DVDReadDir(&d, &entry)) {
        names.push_back(std::string(entry.name) + (entry.isDir ? "/" : ""));
    }
    check(DVDCloseDir(&d), "DVDCloseDir");
    check((names == std::vector<std::string>{"AstroGalaxy/", "empty.bin", "HeavensDoorGalaxy/"}), "DVDReadDir lists children only");
    check(!DVDOpenDir("/opening.bnr", &d), "DVDOpenDir refuses files");
    check(!DVDOpenDir("/nope", &d), "DVDOpenDir of missing directory fails");

    // Absolute reads address the disc; the gap after a file reads as zero.
    std::vector<char> raw(0x40);
    DVDCommandBlock block;
    std::memset(&block, 0xCD, sizeof(block));  // stale state must not matter
    check(DVDReadAbsAsyncPrio(&block, raw.data(), 0x40, positions[4] >> 2, nullptr, 0), "DVDReadAbsAsyncPrio issues");
    check(waitFor([&] { return DVDGetCommandBlockStatus(&block) == DVD_STATE_END; }), "absolute read finishes");
    check(std::string(raw.data(), 0x40) == disc.children[1].data.substr(0, 0x40), "absolute read returns file bytes");

    // Concurrent synchronous readers.
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            DVDFileInfo f;
            if (!DVDOpen("/StageData/AstroGalaxy/AstroGalaxy.arc", &f)) {
                ok = false;
                return;
            }
            std::vector<char> buf(256);
            for (int i = 0; i < 100; ++i) {
                const s32 offset = ((i * 7 + t * 13) % 150) * 256;
                if (DVDReadPrio(&f, buf.data(), 256, offset, t) != 256 ||
                    std::memcmp(buf.data(), disc.children[2].children[0].children[0].data.data() + offset, 256) != 0) {
                    ok = false;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    check(ok, "concurrent synchronous reads return correct data");

    // Disc check succeeds while the tree is present and fails once it is gone.
    static std::atomic<int> diskResult{-100};
    DVDCommandBlock checkBlock;
    diskResult = -100;
    check(DVDCheckDiskAsync(&checkBlock, [](s32 r, DVDCommandBlock*) { diskResult = r; }), "DVDCheckDiskAsync issues");
    check(waitFor([] { return diskResult != -100; }) && diskResult == TRUE, "disc check succeeds");
    fs::rename(tmp.path / "files", tmp.path / "files.moved");
    diskResult = -100;
    DVDCheckDiskAsync(&checkBlock, [](s32 r, DVDCommandBlock*) { diskResult = r; });
    check(waitFor([] { return diskResult != -100; }) && diskResult == FALSE, "disc check fails when the files are gone");
    fs::rename(tmp.path / "files.moved", tmp.path / "files");

    PDVD::shutdown();
    check(!PDVD::isMounted() && DVDConvertPathToEntrynum("/opening.bnr") == -1, "shutdown unmounts");
}

struct Recorder {
    std::mutex lock;
    std::vector<std::pair<int, s32>> events;  // (tag, result)
};
Recorder* gRecorder;

void recordCallback(s32 result, DVDFileInfo* info) {
    // Per SDK ordering, the state is final before the callback runs.
    check(info->cb.state == (result >= 0 ? DVD_STATE_END : DVD_STATE_CANCELED), "state is final when the callback runs");
    std::lock_guard<std::mutex> guard(gRecorder->lock);
    gRecorder->events.emplace_back(static_cast<int>(reinterpret_cast<intptr_t>(info->cb.userData)), result);
}

void testAsyncQueue() {
    TempDir tmp;
    const Node disc = sampleDisc();
    makeExtractedDisc(tmp.path, disc);
    mountOrFail({tmp.path});
    DVDInit();
    Recorder recorder;
    gRecorder = &recorder;

    // Priority order: 0 first, FIFO within a priority.
    DVDFileInfo files[5];
    std::vector<char> buffers[5];
    const int prios[5] = {3, 1, 0, 1, 2};
    DVDPause();
    check(DVDGetDriveStatus() == DVD_STATE_PAUSING, "paused idle drive reports pausing");
    for (int i = 0; i < 5; ++i) {
        check(DVDOpen("/opening.bnr", &files[i]), "open for queue test");
        files[i].cb.userData = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        buffers[i].resize(96);
        check(DVDReadAsyncPrio(&files[i], buffers[i].data(), 96, 0, recordCallback, prios[i]), "DVDReadAsyncPrio issues");
        check(DVDGetCommandBlockStatus(&files[i].cb) == DVD_STATE_WAITING, "queued block is waiting");
    }
    DVDResume();
    check(waitFor([&] { std::lock_guard<std::mutex> g(recorder.lock); return recorder.events.size() == 5; }), "queued reads complete");
    std::vector<int> order;
    for (auto& [tag, result] : recorder.events) {
        order.push_back(tag);
        check(result == 96, "async callback receives the transferred size");
    }
    check((order == std::vector<int>{2, 1, 3, 4, 0}), "commands run in priority order, FIFO within a priority");
    for (auto& b : buffers) {
        check(std::string(b.data(), 96) == disc.children[1].data, "async read data");
    }

    // Cancel a waiting command: callback -3, cancel returns 0.
    recorder.events.clear();
    DVDPause();
    DVDFileInfo waiting;
    std::vector<char> buf(5000);
    check(DVDOpen("/AudioRes/Seq.arc", &waiting), "open for cancel test");
    waiting.cb.userData = reinterpret_cast<void*>(intptr_t(7));
    DVDReadAsyncPrio(&waiting, buf.data(), 5000, 0, recordCallback, 2);
    check(DVDCancel(&waiting.cb) == 0, "DVDCancel of a waiting command");
    check(DVDGetCommandBlockStatus(&waiting.cb) == DVD_STATE_CANCELED, "cancelled block state");
    check(recorder.events.size() == 1 && recorder.events[0] == std::make_pair(7, s32(-3)), "cancelled callback receives -3");
    check(DVDCancel(&waiting.cb) == 0, "cancelling an idle block succeeds without callbacks");
    check(recorder.events.size() == 1, "no extra callback for idle cancel");

    // DVDClose cancels a pending read.
    DVDReadAsyncPrio(&waiting, buf.data(), 5000, 0, recordCallback, 2);
    check(DVDClose(&waiting) && waiting.cb.state == DVD_STATE_CANCELED && recorder.events.size() == 2, "DVDClose cancels pending reads");
    DVDResume();

    // Cancel a busy read between chunks.
    PDVD::Testing::setChunkSize(0x1000);
    std::mutex hookLock;
    std::condition_variable hookCv;
    bool inChunk = false, release = false;
    PDVD::Testing::setChunkHook([&](DVDCommandBlock*) {
        std::unique_lock<std::mutex> g(hookLock);
        inChunk = true;
        hookCv.notify_all();
        hookCv.wait(g, [&] { return release; });
    });
    DVDFileInfo busy;
    std::vector<char> big(0x80000 + 96);
    check(DVDFastOpen(3, &busy), "open big file");
    busy.cb.userData = reinterpret_cast<void*>(intptr_t(9));
    recorder.events.clear();
    DVDReadAsyncPrio(&busy, big.data(), 0x80000 + 96, 0, recordCallback, 1);
    {
        std::unique_lock<std::mutex> g(hookLock);
        check(hookCv.wait_for(g, std::chrono::seconds(10), [&] { return inChunk; }), "read reaches the drive");
    }
    check(DVDGetCommandBlockStatus(&busy.cb) == DVD_STATE_BUSY && DVDGetDriveStatus() == DVD_STATE_BUSY, "busy read state");
    static std::atomic<int> cancelCallbacks{0};
    check(DVDCancelAsync(&busy.cb, [](s32 r, DVDCommandBlock*) { cancelCallbacks += (r == 0); }), "DVDCancelAsync of a busy read");
    check(!DVDCancelAsync(&busy.cb, nullptr), "second cancel of the same busy command is refused");
    check(DVDCancel(&busy.cb) == -1, "DVDCancel while another cancel is in progress returns -1 without waiting");
    {
        std::lock_guard<std::mutex> g(hookLock);
        release = true;
        hookCv.notify_all();
    }
    check(waitFor([&] { return DVDGetCommandBlockStatus(&busy.cb) == DVD_STATE_CANCELED; }), "busy read ends cancelled");
    check(busy.cb.transferredSize == 0x1000, "cancel takes effect after the current chunk");
    check(recorder.events.size() == 1 && recorder.events[0].second == -3 && cancelCallbacks == 1, "busy cancel callbacks");
    check(DVDGetDriveStatus() == DVD_STATE_END, "drive idle after cancel");

    // Synchronous DVDCancel of a busy read waits for the chunk to finish.
    inChunk = false;
    release = false;
    std::atomic<s32> readResult{1};
    std::thread reader([&] { readResult = DVDReadPrio(&busy, big.data(), 0x80000 + 96, 0, 2); });
    {
        std::unique_lock<std::mutex> g(hookLock);
        check(hookCv.wait_for(g, std::chrono::seconds(10), [&] { return inChunk; }), "sync read reaches the drive");
    }
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> g(hookLock);
        release = true;
        hookCv.notify_all();
    });
    check(DVDCancel(&busy.cb) == 0, "DVDCancel of a busy synchronous read");
    releaser.join();
    reader.join();
    check(readResult == -3, "cancelled DVDReadPrio returns -3");
    PDVD::Testing::setChunkHook({});
    PDVD::Testing::setChunkSize(0x80000);

    // A callback may issue the next read on the same block (streaming pattern).
    static DVDFileInfo* chained;
    static std::vector<char>* chainBuf;
    static std::atomic<int> chainCount{0};
    DVDFileInfo chain;
    std::vector<char> chainData(0x1000);
    chained = &chain;
    chainBuf = &chainData;
    chainCount = 0;
    check(DVDFastOpen(7, &chain), "open for chained reads");
    DVDReadAsyncPrio(&chain, chainData.data(), 0x1000, 0, [](s32 r, DVDFileInfo* f) {
        if (r == 0x1000 && ++chainCount < 5) {
            DVDReadAsyncPrio(chained, chainBuf->data(), 0x1000, chainCount * 0x1000, f->callback, 2);
        }
    }, 2);
    check(waitFor([] { return chainCount == 5; }) && waitFor([&] { return DVDGetCommandBlockStatus(&chain.cb) == DVD_STATE_END; }),
          "callbacks can chain reads");
    check(std::memcmp(chainData.data(), disc.children[2].children[0].children[0].data.data() + 0x4000, 0x1000) == 0, "chained read data");

    // Shutdown cancels queued commands.
    DVDPause();
    recorder.events.clear();
    DVDFileInfo pending;
    DVDOpen("/opening.bnr", &pending);
    pending.cb.userData = reinterpret_cast<void*>(intptr_t(11));
    DVDReadAsyncPrio(&pending, buf.data(), 96, 0, recordCallback, 0);
    PDVD::shutdown();
    check(recorder.events.size() == 1 && recorder.events[0] == std::make_pair(11, s32(-3)), "shutdown cancels queued commands");
    gRecorder = nullptr;
}

void testNoDiscThenMount() {
    TempDir tmp;
    const Node disc = sampleDisc();
    const std::vector<std::uint32_t> positions = makeExtractedDisc(tmp.path, disc);
    ::unsetenv(PDVD::kRootEnvironmentVariable);
    DVDInit();
    check(!PDVD::isMounted() && DVDConvertPathToEntrynum("/opening.bnr") == -1, "no disc: lookups fail");
    DVDFileInfo info;
    check(!DVDOpen("/opening.bnr", &info), "no disc: DVDOpen fails");
    DVDDiskID zero{};
    check(std::memcmp(DVDGetCurrentDiskID(), &zero, sizeof(zero)) == 0, "no disc: disc ID is zero");

    static std::atomic<int> diskResult{-100};
    DVDCommandBlock checkBlock;
    DVDCheckDiskAsync(&checkBlock, [](s32 r, DVDCommandBlock*) { diskResult = r; });
    check(diskResult == FALSE, "no disc: disc check fails immediately");

    // A read issued without a disc waits in NO_DISK and is cancellable.
    std::vector<char> raw(96);
    DVDCommandBlock block;
    DVDReadAbsAsyncPrio(&block, raw.data(), 96, positions[4] >> 2, nullptr, 2);
    check(waitFor([&] { return DVDGetCommandBlockStatus(&block) == DVD_STATE_NO_DISK; }), "no disc: command waits in NO_DISK");
    check(DVDGetDriveStatus() == DVD_STATE_NO_DISK, "no disc: drive status");
    check(DVDCancel(&block) == 0 && block.state == DVD_STATE_CANCELED, "NO_DISK command can be cancelled");

    // Mounting releases a waiting command.
    DVDReadAbsAsyncPrio(&block, raw.data(), 96, positions[4] >> 2, nullptr, 2);
    check(waitFor([&] { return DVDGetCommandBlockStatus(&block) == DVD_STATE_NO_DISK; }), "command waits for a disc");
    ::setenv(PDVD::kRootEnvironmentVariable, "/nonexistent", 1);  // ignored: DVDInit already ran
    mountOrFail({tmp.path});
    check(waitFor([&] { return DVDGetCommandBlockStatus(&block) == DVD_STATE_END; }), "mount releases the waiting command");
    check(std::string(raw.data(), 96) == disc.children[1].data && block.transferredSize == 96, "released command reads the disc");
    PDVD::shutdown();

    // DVDInit mounts from the environment.
    ::setenv(PDVD::kRootEnvironmentVariable, tmp.path.c_str(), 1);
    DVDInit();
    check(PDVD::isMounted() && DVDConvertPathToEntrynum("/opening.bnr") == 4, "DVDInit mounts PETARI_GAME_DIR");
    PDVD::shutdown();
    ::unsetenv(PDVD::kRootEnvironmentVariable);
}

void testMountValidationAndScan() {
    TempDir tmp;
    std::string error;
    check(!PDVD::mount({tmp.path}, &error) && error.find("files/") != std::string::npos, "mount requires files/");

    // Modified file size contradicts fst.bin.
    const Node disc = sampleDisc();
    makeExtractedDisc(tmp.path, disc);
    std::ofstream(tmp.path / "files" / "opening.bnr", std::ios::binary | std::ios::app) << "extra";
    error.clear();
    check(!PDVD::mount({tmp.path}, &error), "size mismatch rejects fst.bin mount");
    check(error.find("opening.bnr is 101 bytes; the disc FST lists 96") != std::string::npos, "size mismatch is reported");
    fs::remove(tmp.path / "files" / "StageData" / "empty.bin");
    error.clear();
    check(!PDVD::mount({tmp.path}, &error) && error.find("missing file") != std::string::npos, "missing file is reported");

    // Name escaping files/ is rejected.
    {
        TempDir bad;
        fs::create_directories(bad.path / "files");
        fs::create_directories(bad.path / "sys");
        writeBytes(bad.path / "sys" / "fst.bin", FstWriter(0x440000).build(dir("", {file("..", "x")})));
        error.clear();
        check(!PDVD::mount({bad.path}, &error) && error.find("invalid name") != std::string::npos, "'..' FST name is rejected");
        writeBytes(bad.path / "sys" / "fst.bin", {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9});
        error.clear();
        check(!PDVD::mount({bad.path}, &error) && error.find("does not fit") != std::string::npos, "truncated fst.bin is rejected");
    }

    // Directory scan of the modified tree, with host metadata and an extra file.
    std::ofstream(tmp.path / "files" / ".DS_Store") << "x";
    std::ofstream(tmp.path / "files" / "._opening.bnr") << "x";
    std::ofstream(tmp.path / "files" / "a_first.txt") << "abc";
    std::ofstream(tmp.path / "files" / "Zed.txt") << "z";
    std::ofstream(tmp.path / "files" / "b.txt") << "b";
    mountOrFail({tmp.path, true});
    DVDInit();
    const PDVD::MountInfo info = PDVD::mountInfo();
    check(info.source == PDVD::FstSource::DirectoryScan && info.hasDiskId, "ignoreDiscFst scans files/");
    DVDDir d;
    DVDDirEntry entry;
    std::vector<std::string> names;
    check(DVDOpenDir("/", &d), "open root");
    while (DVDReadDir(&d, &entry)) {
        names.push_back(entry.name);
    }
    check((names == std::vector<std::string>{"AudioRes", "a_first.txt", "b.txt", "opening.bnr", "StageData", "Zed.txt"}),
          "scan orders by upper-cased name and skips macOS metadata");
    DVDFileInfo f;
    s32 result = 0;
    check(DVDOpen("/opening.bnr", &f) && f.length == 101, "scanned file uses the host size");
    check(readSync(f, 101, 0, &result) == disc.children[1].data + "extra" && result == 101, "scanned file reads");
    DVDFileInfo arc;
    check(DVDOpen("/StageData/AstroGalaxy/AstroGalaxy.arc", &arc), "scanned nested file");
    check(readSync(arc, 40000, 0) == disc.children[2].children[0].children[0].data, "scanned nested file reads");
    check(arc.startAddr != f.startAddr && (arc.startAddr & ((0x8000 >> 2) - 1)) == 0, "synthetic positions are distinct and aligned");
    PDVD::shutdown();

    // wit layout: DATA/files, no sys/.
    TempDir wit;
    fs::create_directories(wit.path / "DATA" / "files");
    std::ofstream(wit.path / "DATA" / "files" / "a.bin") << "hello";
    mountOrFail({wit.path});
    check(!PDVD::mountInfo().hasDiskId && DVDConvertPathToEntrynum("/A.BIN") == 1, "DATA/files layout without sys/");
    PDVD::shutdown();
}

// Disc-file mods: replacement (same, smaller, larger), case-insensitive paths, new files and
// directories, conflicts, and byte-identical behavior without an overlay.
void testOverlay() {
    TempDir tmp, mods;
    const Node disc = sampleDisc();
    makeExtractedDisc(tmp.path, disc);
    const std::string bigger = pattern(70000, 9), smaller = pattern(10, 8), same = pattern(96, 7), added = pattern(123, 6);
    writeBytes(mods.path / "bigger.arc", std::vector<std::uint8_t>(bigger.begin(), bigger.end()));
    writeBytes(mods.path / "smaller.bin", std::vector<std::uint8_t>(smaller.begin(), smaller.end()));
    writeBytes(mods.path / "same.bnr", std::vector<std::uint8_t>(same.begin(), same.end()));
    writeBytes(mods.path / "added.arc", std::vector<std::uint8_t>(added.begin(), added.end()));

    // No overlay: identical tables (entry count, positions) to the plain mount.
    mountOrFail({tmp.path});
    DVDInit();
    const std::uint32_t plainEntries = PDVD::mountInfo().entryCount;
    DVDFileInfo plain;
    check(DVDOpen("/StageData/AstroGalaxy/AstroGalaxy.arc", &plain), "plain open");
    const u32 plainStart = plain.startAddr;
    check(PDVD::mountInfo().overlayReplaced == 0 && PDVD::mountInfo().overlayAdded == 0, "no overlay: nothing reported");
    PDVD::shutdown();

    PDVD::MountOptions options{tmp.path};
    options.overlay = {
        {"stagedata/ASTROGALAXY/astrogalaxy.ARC", mods.path / "bigger.arc", "ModA"},  // other case, grows
        {"opening.bnr", mods.path / "same.bnr", "ModA"},                              // same size
        {"AudioRes/Seq.arc", mods.path / "smaller.bin", "ModB"},                      // shrinks
        {"StageData/NewGalaxy/NewGalaxy.arc", mods.path / "added.arc", "ModB"},       // new dir + file
        {"StageData/AstroGalaxy/Extra.bin", mods.path / "added.arc", "ModB"},         // new file, existing dir
        {"StageData/empty.bin/inside.bin", mods.path / "added.arc", "ModB"},          // dir where a file is: skipped
        {"AudioRes", mods.path / "added.arc", "ModB"},                                // file where a dir is: skipped
    };
    mountOrFail(options);
    DVDInit();
    const PDVD::MountInfo info = PDVD::mountInfo();
    check(info.overlayReplaced == 3 && info.overlayAdded == 2, "overlay counts replaced and added files");
    check(info.entryCount == plainEntries + 3, "new file + new dir + new file add three entries");
    DVDFileInfo f;
    s32 result = 0;
    check(DVDOpen("/StageData/AstroGalaxy/AstroGalaxy.arc", &f) && f.length == bigger.size(), "grown file reports the mod size");
    check(readSync(f, static_cast<s32>(bigger.size()), 0, &result) == bigger && result == static_cast<s32>(bigger.size()),
          "grown file reads the mod bytes");
    check(f.startAddr != plainStart, "a grown file moves to the end of the disc");
    check(DVDOpen("/opening.bnr", &f) && f.length == 96 && readSync(f, 96, 0) == same, "same-size replacement reads the mod bytes");
    check(DVDOpen("/AudioRes/Seq.arc", &f) && f.length == 10 && readSync(f, 10, 0) == smaller, "smaller replacement reads the mod bytes");
    check(DVDOpen("/AudioRes/Wave.aw", &f) && readSync(f, 64, 0) == disc.children[0].children[1].data.substr(0, 64),
          "untouched disc files still read the disc");
    check(DVDOpen("/STAGEDATA/newgalaxy/NEWGALAXY.ARC", &f) && f.length == 123 && readSync(f, 123, 0) == added,
          "an added file is found case-insensitively and reads");
    check(DVDOpen("/StageData/AstroGalaxy/Extra.bin", &f) && readSync(f, 123, 0) == added, "an added file in an existing directory");
    check(!DVDOpen("/StageData/empty.bin/inside.bin", &f), "a path through a disc file is skipped");
    check(DVDOpen("/AudioRes/Seq.arc", &f), "a file path equal to a disc directory is skipped");
    // Absolute (disc offset) reads see the mod bytes too.
    DVDCommandBlock block;
    std::vector<char> buffer(32, 0);
    check(DVDOpen("/opening.bnr", &f), "reopen");
    check(DVDReadAbsAsyncPrio(&block, buffer.data(), 32, f.startAddr, nullptr, 2), "absolute read issues");
    waitFor([&] { return DVDGetCommandBlockStatus(&block) == DVD_STATE_END; });
    check(std::string(buffer.data(), 32) == same.substr(0, 32), "absolute disc reads map through the overlay");
    PDVD::shutdown();
}

void testFatalError() {
    TempDir tmp;
    const Node disc = sampleDisc();
    makeExtractedDisc(tmp.path, disc);
    mountOrFail({tmp.path});
    DVDInit();
    DVDFileInfo f;
    check(DVDOpen("/StageData/HeavensDoorGalaxy/HeavensDoorGalaxy.arc", &f), "open before truncation");
    fs::resize_file(tmp.path / "files" / "StageData" / "HeavensDoorGalaxy" / "HeavensDoorGalaxy.arc", 10);
    std::vector<char> buf(352);
    check(DVDReadPrio(&f, buf.data(), 352, 0, 2) == -1, "host read failure returns -1");
    check(f.cb.state == DVD_STATE_FATAL_ERROR && DVDGetDriveStatus() == DVD_STATE_FATAL_ERROR, "host read failure is a fatal drive error");
    DVDFileInfo other;
    check(DVDOpen("/opening.bnr", &other) && DVDReadPrio(&other, buf.data(), 96, 0, 2) == -1, "later commands fail after a fatal error");
    static std::atomic<int> diskResult{-100};
    DVDCommandBlock checkBlock;
    DVDCheckDiskAsync(&checkBlock, [](s32 r, DVDCommandBlock*) { diskResult = r; });
    check(diskResult == FALSE, "disc check reports the fatal error");
    PDVD::shutdown();
}

void testMisuseAborts() {
    TempDir tmp;
    makeExtractedDisc(tmp.path, sampleDisc());
    mountOrFail({tmp.path});
    DVDInit();
    DVDFileInfo f;
    DVDOpen("/opening.bnr", &f);
    char buf[256];
    check(aborts([&] { DVDReadPrio(&f, buf, 32, 100, 2); }), "read starting past the file aborts");
    check(aborts([&] { DVDReadPrio(&f, buf, 64, 64, 2); }), "read ending 32+ bytes past the file aborts");
    check(aborts([&] { DVDReadPrio(&f, buf, 32, 2, 2); }), "unaligned offset aborts");
    check(aborts([&] { DVDReadPrio(&f, buf, 32, 0, 4); }), "invalid priority aborts");
    PDVD::shutdown();

    // DVDReadPrio from a DVD callback would deadlock the drive.
    check(aborts([&] {
        mountOrFail({tmp.path});
        DVDInit();
        static DVDFileInfo inner;
        static char innerBuf[96];
        DVDOpen("/opening.bnr", &inner);
        DVDFileInfo outer;
        DVDOpen("/opening.bnr", &outer);
        static char outerBuf[96];
        DVDReadAsyncPrio(&outer, outerBuf, 96, 0, [](s32, DVDFileInfo*) { DVDReadPrio(&inner, innerBuf, 96, 0, 2); }, 2);
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }), "synchronous read from a callback aborts");
}

}  // namespace

int main() {
    testFstMount();
    testAsyncQueue();
    testNoDiscThenMount();
    testMountValidationAndScan();
    testOverlay();
    testFatalError();
    testMisuseAborts();
    std::printf("platform DVD tests passed (%d checks)\n", checks);
    return 0;
}

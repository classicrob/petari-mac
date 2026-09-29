// Native implementation of the Revolution SDK DVD API over an extracted disc.
//
// Replaces src/RVL_SDK/dvd/{dvd,dvdfs,dvdqueue,dvd_broadway,dvderror,dvdFatal,
// dvdDeviceError}.c on the host. dvdidutils.c is portable and is compiled
// unchanged alongside this file.
//
// Drive model (matches dvd.c where the host can):
// - One drive thread executes commands from four FIFO queues, priority 0
//   first. A command block's state goes 2 (WAITING) -> 1 (BUSY) -> 0 (END),
//   10 (CANCELED), or -1 (FATAL_ERROR). While no disc is mounted the executing
//   command sits in 4 (NO_DISK).
// - Issuing, cancelling, and completing commands allocate nothing; queues
//   link through DVDCommandBlock::next/prev as dvdqueue.c does. Host-side
//   allocations (mount tables, drive thread state) run under
//   PetariNative::HostAllocationScope so they never land in game heaps.
// - Reads transfer in 0x80000-byte chunks; transferredSize advances after each
//   chunk. A cancel of a busy read takes effect after the current chunk, as
//   DVDLowBreak does on hardware.
// - Drive state is protected by the OS interrupt lock (OSDisableInterrupts),
//   shared with the native OS layer. The drive thread is an interrupt source:
//   completion callbacks run on it with interrupts disabled, so no thread can
//   observe a finished state before its callback has returned, and OS calls
//   made by callbacks (OSSendMessage, OSWakeupThread) wake game threads as DI
//   interrupts do. Callbacks may issue or cancel commands but must not block;
//   synchronous DVD calls from a callback abort.
// - DVDReadPrio and DVDCancel block like the SDK: OS threads sleep on the DVD
//   thread queue (giving up the CPU to other OS threads); plain host threads
//   (tools, tests) wait on a condition variable.
// - A host I/O failure puts the drive in the fatal error state, like an
//   unrecoverable disc error: that command and all later ones fail with -1 and
//   DVDGetDriveStatus() returns -1.

#include <revolution/dvd.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "dvd_fst.hpp"
#include "dvd_internal.hpp"
#include "os_internal.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/dvd.hpp"

namespace fs = std::filesystem;

namespace PetariNative::Platform::DVD {
namespace {

constexpr u32 kCommandRead = 1;
constexpr u32 kCommandCheckDisk = 36;
constexpr int kQueueCount = 4;
constexpr std::size_t kMaxOpenFiles = 32;

[[noreturn]] void panic(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    std::fputs("DVD panic: ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::abort();
}

void storeState(DVDCommandBlock* block, s32 state) {
    __atomic_store_n(&block->state, state, __ATOMIC_RELEASE);
}

// Host file descriptors for disc extents. Only the drive thread uses it.
class OpenFiles {
public:
    ~OpenFiles() { clear(); }

    int get(u32 entry, const fs::path& path) {
        auto found = mFds.find(entry);
        if (found != mFds.end()) {
            mOrder.splice(mOrder.begin(), mOrder, found->second.second);
            return found->second.first;
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return -1;
        }
        if (mFds.size() >= kMaxOpenFiles) {
            const u32 victim = mOrder.back();
            ::close(mFds[victim].first);
            mFds.erase(victim);
            mOrder.pop_back();
        }
        mOrder.push_front(entry);
        mFds.emplace(entry, std::make_pair(fd, mOrder.begin()));
        return fd;
    }

    void clear() {
        for (auto& [entry, value] : mFds) {
            ::close(value.first);
        }
        mFds.clear();
        mOrder.clear();
    }

private:
    std::list<u32> mOrder;
    std::unordered_map<u32, std::pair<int, std::list<u32>::iterator>> mFds;
};

// FIFO linked through DVDCommandBlock::next/prev, as in dvdqueue.c, so
// issuing and cancelling commands allocates nothing on game threads.
// Membership is established by walking the list: a block's next/prev are
// only meaningful while it is queued.
class WaitingQueue {
public:
    bool empty() const { return mHead == nullptr; }

    bool contains(const DVDCommandBlock* block) const {
        for (const DVDCommandBlock* it = mHead; it != nullptr; it = it->next) {
            if (it == block) {
                return true;
            }
        }
        return false;
    }

    void pushBack(DVDCommandBlock* block) {
        block->next = nullptr;
        block->prev = mTail;
        if (mTail) {
            mTail->next = block;
        } else {
            mHead = block;
        }
        mTail = block;
    }

    DVDCommandBlock* popFront() {
        DVDCommandBlock* block = mHead;
        if (block) {
            unlink(block);
        }
        return block;
    }

    bool remove(DVDCommandBlock* block) {
        if (!contains(block)) {
            return false;
        }
        unlink(block);
        return true;
    }

private:
    void unlink(DVDCommandBlock* block) {
        (block->prev ? block->prev->next : mHead) = block->next;
        (block->next ? block->next->prev : mTail) = block->prev;
        block->next = nullptr;
        block->prev = nullptr;
    }

    DVDCommandBlock* mHead = nullptr;
    DVDCommandBlock* mTail = nullptr;
};

struct Drive {
    // Wakes the drive thread and host-thread waiters; OS-thread waiters sleep
    // on waiters. Both are signalled after every completion.
    std::condition_variable changed;
    OSThreadQueue waiters{nullptr, nullptr};

    bool initialized = false;
    bool stopRequested = false;
    std::thread thread;
    std::thread::id threadId;

    WaitingQueue queues[kQueueCount];
    DVDCommandBlock* executing = nullptr;
    bool cancelRequested = false;
    DVDCBCallback cancelCallback = nullptr;
    bool paused = false;
    bool fatal = false;
    BOOL autoInvalidation = TRUE;

    // Published once per mount; immutable while mounted, so path lookups run
    // without the lock.
    std::atomic<Fst*> fst{nullptr};
    MountInfo info;
    u32 currentDirectory = 0;

    std::function<void(DVDCommandBlock*)> chunkHook;
    u32 chunkSize = 0x80000;
};

// Intentionally never destroyed: the drive thread may outlive static
// destruction when the game exits without calling shutdown().
Drive& drive() {
    static Drive* instance = new Drive;
    return *instance;
}

// Stands in for the disc ID at physical address 0 on the console.
DVDDiskID gDiskId;

namespace OS = PetariNative::Platform::OS;
using InterruptGuard = OS::InterruptGuard;

void signalChange(Drive& d) {
    d.changed.notify_all();
    OSWakeupThread(&d.waiters);
}

bool isQueued(Drive& d, const DVDCommandBlock* block) {
    for (const auto& queue : d.queues) {
        if (queue.contains(block)) {
            return true;
        }
    }
    return false;
}

// Highest priority first.
DVDCommandBlock* popWaiting(Drive& d) {
    for (auto& queue : d.queues) {
        if (DVDCommandBlock* block = queue.popFront()) {
            return block;
        }
    }
    return nullptr;
}

bool isInFlight(Drive& d, const DVDCommandBlock* block) {
    return d.executing == block || isQueued(d, block);
}

bool anyQueued(Drive& d) {
    for (const auto& queue : d.queues) {
        if (!queue.empty()) {
            return true;
        }
    }
    return false;
}

void requireNotDriveThread(Drive& d, const char* function) {
    if (std::this_thread::get_id() == d.threadId) {
        panic("%s() called from a DVD callback; it would wait on the drive that is running the callback", function);
    }
}

// Interrupts disabled. Blocks until block is neither queued nor executing.
void waitUntilIdle(Drive& d, const DVDCommandBlock* block) {
    if (OS::boundThread() != nullptr) {
        while (isInFlight(d, block)) {
            OSSleepThread(&d.waiters);
        }
    } else {
        OS::hostWait(d.changed, [&] { return !isInFlight(d, block); });
    }
}

BOOL issueCommand(s32 prio, DVDCommandBlock* block) {
    Drive& d = drive();
    InterruptGuard guard;
    if (!d.initialized) {
        panic("DVD command issued before DVDInit()");
    }
    if (prio < 0 || prio >= kQueueCount) {
        panic("invalid DVD priority %d (expected 0-3)", prio);
    }
    if (isInFlight(d, block)) {
        panic("DVD command block %p reissued while its previous command is still pending", static_cast<void*>(block));
    }
    storeState(block, DVD_STATE_WAITING);
    d.queues[prio].pushBack(block);
    d.changed.notify_all();
    return TRUE;
}

// Reads disc bytes [offset, offset + length) into dst. Areas between files
// read as zero. Called on the drive thread without the lock.
bool readDisc(const Fst& fst, OpenFiles& files, u8* dst, u64 offset, u64 length) {
    if (offset + length > fst.discEnd()) {
        std::fprintf(stderr, "DVD: read of 0x%llx bytes at disc offset 0x%llx is past the end of the disc\n",
                     static_cast<unsigned long long>(length), static_cast<unsigned long long>(offset));
        return false;
    }
    const auto& extents = fst.extents();
    while (length > 0) {
        auto next = std::upper_bound(extents.begin(), extents.end(), offset,
                                     [](u64 value, const Fst::Extent& e) { return value < e.start; });
        if (next != extents.begin()) {
            const Fst::Extent& e = *(next - 1);
            if (offset < e.start + e.length) {
                const u64 n = std::min(length, e.start + e.length - offset);
                const int fd = files.get(e.entry, fst.hostPath(e.entry));
                if (fd < 0) {
                    std::fprintf(stderr, "DVD: cannot open %s: %s\n", fst.hostPath(e.entry).c_str(), std::strerror(errno));
                    return false;
                }
                u64 done = 0;
                while (done < n) {
                    const ssize_t got = ::pread(fd, dst + done, static_cast<size_t>(n - done), static_cast<off_t>(offset - e.start + done));
                    if (got < 0 && errno == EINTR) {
                        continue;
                    }
                    if (got <= 0) {
                        std::fprintf(stderr, "DVD: read of %s failed: %s\n", fst.hostPath(e.entry).c_str(),
                                     got == 0 ? "file is shorter than when it was mounted" : std::strerror(errno));
                        return false;
                    }
                    done += static_cast<u64>(got);
                }
                dst += n;
                offset += n;
                length -= n;
                continue;
            }
        }
        const u64 gapEnd = next == extents.end() ? offset + length : std::min(offset + length, next->start);
        std::memset(dst, 0, static_cast<size_t>(gapEnd - offset));
        dst += gapEnd - offset;
        length -= gapEnd - offset;
        offset = gapEnd;
    }
    return true;
}

// Finishes the executing command. Lock held; callbacks run under it.
void complete(Drive& d, DVDCommandBlock* block, s32 state, s32 result) {
    const bool wasCanceling = d.cancelRequested;
    const DVDCBCallback cancelCallback = d.cancelCallback;
    d.executing = nullptr;
    d.cancelRequested = false;
    d.cancelCallback = nullptr;
    storeState(block, state);
    if (block->callback) {
        block->callback(result, block);
    }
    if (wasCanceling && cancelCallback) {
        cancelCallback(0, block);
    }
    signalChange(d);
}

void runDrive() {
    // Drive-thread bookkeeping (open files, test hooks) must not come from
    // whichever JKR heap is current on the game side.
    PetariNative::HostAllocationScope hostAllocations;
    Drive& d = drive();
    OpenFiles files;
    OSDisableInterrupts();
    while (true) {
        OS::hostWait(d.changed, [&] { return d.stopRequested || (!d.paused && anyQueued(d)); });
        if (d.stopRequested) {
            break;
        }
        DVDCommandBlock* block = popWaiting(d);
        d.executing = block;

        if (d.fatal) {
            complete(d, block, DVD_STATE_FATAL_ERROR, -1);
            continue;
        }
        if (d.fst.load() == nullptr) {
            storeState(block, DVD_STATE_NO_DISK);
            d.changed.notify_all();
            OS::hostWait(d.changed, [&] { return d.stopRequested || d.cancelRequested || d.fst.load() != nullptr; });
            if (d.stopRequested || d.cancelRequested) {
                complete(d, block, DVD_STATE_CANCELED, -3);
                continue;
            }
        }
        const Fst& fst = *d.fst.load();
        storeState(block, DVD_STATE_BUSY);
        d.changed.notify_all();

        if (block->command == kCommandCheckDisk) {
            std::error_code ec;
            const BOOL present = fs::is_directory(d.info.filesDirectory, ec) ? TRUE : FALSE;
            block->offset = static_cast<u32>(present);
            complete(d, block, DVD_STATE_END, present);
            continue;
        }

        // kCommandRead
        bool failed = false;
        while (block->transferredSize < block->length) {
            const u32 n = std::min(d.chunkSize, block->length - block->transferredSize);
            block->currTransferSize = n;
            const u64 discOffset = (u64(block->offset) << 2) + block->transferredSize;
            u8* dst = static_cast<u8*>(block->addr) + block->transferredSize;
            OSEnableInterrupts();
            const bool ok = readDisc(fst, files, dst, discOffset, n);
            OSDisableInterrupts();
            if (!ok) {
                failed = true;
                break;
            }
            block->transferredSize += n;
            if (d.chunkHook) {
                auto hook = d.chunkHook;
                OSEnableInterrupts();
                hook(block);
                OSDisableInterrupts();
            }
            if (d.cancelRequested || d.stopRequested) {
                break;
            }
        }
        if (failed) {
            d.fatal = true;
            complete(d, block, DVD_STATE_FATAL_ERROR, -1);
        } else if (d.cancelRequested || d.stopRequested) {
            complete(d, block, DVD_STATE_CANCELED, -3);
        } else {
            complete(d, block, DVD_STATE_END, static_cast<s32>(block->transferredSize));
        }
    }
    // Pending cancels of the command interrupted by stop were completed above.
    OSEnableInterrupts();
    files.clear();
}

bool loadFile(const fs::path& path, std::vector<u8>& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error) {
            *error = "cannot read " + path.string();
        }
        return false;
    }
    return true;
}

void checkReadRange(const DVDFileInfo* fileInfo, s32 length, s32 offset, const char* function) {
    if (!(0 <= offset && static_cast<u32>(offset) <= fileInfo->length)) {
        panic("%s(): specified area is out of the file (offset %d, file length %u)", function, offset, fileInfo->length);
    }
    if (length < 0 || !(static_cast<u64>(offset) + static_cast<u64>(length) < u64(fileInfo->length) + 32)) {
        panic("%s(): specified area is out of the file (offset %d, length %d, file length %u)", function, offset, length, fileInfo->length);
    }
    // The drive addresses 4-byte words; the SDK silently drops the low bits.
    if (offset & 3) {
        panic("%s(): offset %d is not a multiple of 4", function, offset);
    }
}

void cbForReadAsync(s32 result, DVDCommandBlock* block) {
    // cb is the first member of DVDFileInfo.
    DVDFileInfo* fileInfo = reinterpret_cast<DVDFileInfo*>(block);
    if (fileInfo->callback) {
        fileInfo->callback(result, fileInfo);
    }
}

void startDriveLocked(Drive& d) {
    d.stopRequested = false;
    d.thread = std::thread(runDrive);
    d.threadId = d.thread.get_id();
}

}  // namespace

bool mount(const MountOptions& options, std::string* error) {
    // The FST lives for the whole mount, independent of game heaps.
    PetariNative::HostAllocationScope hostAllocations;
    fs::path base;
    std::error_code ec;
    for (const fs::path& candidate : {options.root, options.root / "DATA"}) {
        if (fs::is_directory(candidate / "files", ec)) {
            base = candidate;
            break;
        }
    }
    if (base.empty()) {
        if (error) {
            *error = options.root.string() + " does not contain files/ or DATA/files/ from an extracted disc";
        }
        return false;
    }

    auto fst = std::make_unique<Fst>();
    MountInfo info;
    info.filesDirectory = base / "files";
    const fs::path fstPath = base / "sys" / "fst.bin";
    if (!options.ignoreDiscFst && fs::is_regular_file(fstPath, ec)) {
        std::vector<u8> image;
        if (!loadFile(fstPath, image, error) || !Fst::fromDiscImage(image, info.filesDirectory, *fst, error)) {
            return false;
        }
        info.source = FstSource::DiscFst;
    } else {
        if (!Fst::fromDirectory(info.filesDirectory, *fst, error)) {
            return false;
        }
        info.source = FstSource::DirectoryScan;
    }
    info.entryCount = fst->entryCount();
    info.fileCount = fst->fileCount();

    DVDDiskID id{};
    const fs::path bootPath = base / "sys" / "boot.bin";
    if (fs::is_regular_file(bootPath, ec)) {
        std::vector<u8> boot;
        if (!loadFile(bootPath, boot, error)) {
            return false;
        }
        if (boot.size() < sizeof(DVDDiskID)) {
            if (error) {
                *error = bootPath.string() + " is shorter than a disc ID";
            }
            return false;
        }
        std::memcpy(id.gameName, &boot[0], 4);
        std::memcpy(id.company, &boot[4], 2);
        id.diskNumber = boot[6];
        id.gameVersion = boot[7];
        id.streaming = boot[8];
        id.streamingBufSize = boot[9];
        std::memcpy(id.padding, &boot[10], sizeof(id.padding));
        id.rvlMagic = readU32BE(&boot[0x18]);
        id.gcMagic = readU32BE(&boot[0x1C]);
        info.hasDiskId = true;
    }

    Drive& d = drive();
    InterruptGuard guard;
    if (d.fst.load() != nullptr) {
        if (error) {
            *error = "a disc is already mounted";
        }
        return false;
    }
    gDiskId = id;
    d.info = std::move(info);
    d.currentDirectory = 0;
    d.fst.store(fst.release());
    d.changed.notify_all();
    return true;
}

bool isMounted() {
    return drive().fst.load() != nullptr;
}

MountInfo mountInfo() {
    Drive& d = drive();
    InterruptGuard guard;
    return d.fst.load() ? d.info : MountInfo{};
}

fs::path hostPathForEntry(s32 entryNum) {
    const Fst* fst = drive().fst.load();
    if (fst == nullptr || entryNum < 0 || static_cast<u32>(entryNum) >= fst->entryCount()) {
        return {};
    }
    return fst->hostPath(static_cast<u32>(entryNum));
}

void shutdown() {
    PetariNative::HostAllocationScope hostAllocations;
    Drive& d = drive();
    requireNotDriveThread(d, "PetariNative::Platform::DVD::shutdown");
    if (OS::interruptsDisabled()) {
        panic("PetariNative::Platform::DVD::shutdown() needs interrupts enabled to stop the drive thread");
    }
    InterruptGuard guard;
    if (d.thread.joinable()) {
        d.stopRequested = true;
        d.changed.notify_all();
        std::thread thread = std::move(d.thread);
        OSEnableInterrupts();
        thread.join();
        OSDisableInterrupts();
    }
    while (DVDCommandBlock* block = popWaiting(d)) {
        storeState(block, DVD_STATE_CANCELED);
        if (block->callback) {
            block->callback(-3, block);
        }
    }
    delete d.fst.exchange(nullptr);
    d.info = MountInfo{};
    gDiskId = DVDDiskID{};
    d.initialized = false;
    d.stopRequested = false;
    d.threadId = std::thread::id();
    d.executing = nullptr;
    d.cancelRequested = false;
    d.cancelCallback = nullptr;
    d.paused = false;
    d.fatal = false;
    d.autoInvalidation = TRUE;
    d.currentDirectory = 0;
    d.changed.notify_all();
}

namespace Testing {

void setChunkHook(std::function<void(DVDCommandBlock*)> hook) {
    PetariNative::HostAllocationScope hostAllocations;
    Drive& d = drive();
    InterruptGuard guard;
    d.chunkHook = std::move(hook);
}

void setChunkSize(std::uint32_t bytes) {
    if (bytes == 0 || (bytes & 31) != 0) {
        panic("DVD chunk size %u must be a non-zero multiple of 32", bytes);
    }
    Drive& d = drive();
    InterruptGuard guard;
    d.chunkSize = bytes;
}

}  // namespace Testing
}  // namespace PetariNative::Platform::DVD

using namespace PetariNative::Platform::DVD;

extern "C" {

void DVDInit(void) {
    // Starts the drive thread and may mount PETARI_GAME_DIR.
    PetariNative::HostAllocationScope hostAllocations;
    Drive& d = drive();
    InterruptGuard guard;
    if (d.initialized) {
        return;
    }
    d.initialized = true;
    if (d.fst.load() == nullptr) {
        if (const char* root = std::getenv(kRootEnvironmentVariable); root != nullptr && *root != '\0') {
            std::string error;
            if (!mount({root}, &error)) {
                std::fprintf(stderr, "DVD: cannot mount %s=%s: %s\n", kRootEnvironmentVariable, root, error.c_str());
            }
        }
    }
    startDriveLocked(d);
}

s32 DVDConvertPathToEntrynum(const char* pathPtr) {
    Drive& d = drive();
    const Fst* fst = d.fst.load();
    if (fst == nullptr) {
        return -1;
    }
    return fst->convertPathToEntrynum(pathPtr, d.currentDirectory);
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    const Fst* fst = drive().fst.load();
    if (fst == nullptr || entrynum < 0 || static_cast<u32>(entrynum) >= fst->entryCount() || fst->entry(entrynum).isDir) {
        return FALSE;
    }
    fileInfo->startAddr = fst->entry(entrynum).parentOrPosition;
    fileInfo->length = fst->entry(entrynum).nextOrLength;
    fileInfo->callback = nullptr;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDGetCurrentDir(char* path, u32 maxlen) {
    Drive& d = drive();
    const Fst* fst = d.fst.load();
    if (fst == nullptr) {
        if (maxlen > 0) {
            path[0] = '\0';
        }
        return FALSE;
    }
    return fst->convertEntrynumToPath(d.currentDirectory, path, maxlen) ? TRUE : FALSE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    const s32 entry = DVDConvertPathToEntrynum(fileName);
    if (entry < 0) {
        char currentDir[128];
        if (drive().fst.load() == nullptr) {
            OSReport("Warning: DVDOpen(): file '%s' was not found: no disc is mounted.\n", fileName);
        } else {
            DVDGetCurrentDir(currentDir, sizeof(currentDir));
            OSReport("Warning: DVDOpen(): file '%s' was not found under %s.\n", fileName, currentDir);
        }
        return FALSE;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    DVDCancel(&fileInfo->cb);
    return TRUE;
}

BOOL DVDOpenDir(const char* dirName, DVDDir* dir) {
    const s32 entry = DVDConvertPathToEntrynum(dirName);
    const Fst* fst = drive().fst.load();
    if (entry < 0) {
        char currentDir[128];
        DVDGetCurrentDir(currentDir, sizeof(currentDir));
        OSReport("Warning: DVDOpenDir(): file '%s' was not found under %s.\n", dirName,
                     fst ? currentDir : "(no disc mounted)");
        return FALSE;
    }
    if (!fst->entry(entry).isDir) {
        return FALSE;
    }
    dir->entryNum = static_cast<u32>(entry);
    dir->location = static_cast<u32>(entry) + 1;
    dir->next = fst->entry(entry).nextOrLength;
    return TRUE;
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirent) {
    Fst* fst = drive().fst.load();
    const u32 loc = dir->location;
    if (fst == nullptr || loc <= dir->entryNum || dir->next <= loc || loc >= fst->entryCount()) {
        return FALSE;
    }
    const FstEntry& e = fst->entry(loc);
    dirent->entryNum = loc;
    dirent->isDir = e.isDir ? TRUE : FALSE;
    dirent->name = fst->name(loc);
    dir->location = e.isDir ? e.nextOrLength : loc + 1;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir*) {
    // Directory handles hold no resources; the SDK's DVDCloseDir also only returns TRUE.
    return TRUE;
}

BOOL DVDReadAbsAsyncPrio(DVDCommandBlock* block, void* addr, s32 length, u32 offset, DVDCBCallback callback, s32 prio) {
    block->command = kCommandRead;
    block->addr = addr;
    block->length = static_cast<u32>(length);
    block->offset = offset;
    block->transferredSize = 0;
    block->currTransferSize = 0;
    block->callback = callback;
    return issueCommand(prio, block);
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio) {
    checkReadRange(fileInfo, length, offset, "DVDReadAsync");
    fileInfo->callback = callback;
    DVDReadAbsAsyncPrio(&fileInfo->cb, addr, length, fileInfo->startAddr + static_cast<u32>(offset >> 2), cbForReadAsync, prio);
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    checkReadRange(fileInfo, length, offset, "DVDRead");
    Drive& d = drive();
    requireNotDriveThread(d, "DVDReadPrio");
    DVDCommandBlock* block = &fileInfo->cb;
    InterruptGuard guard;
    if (DVDReadAbsAsyncPrio(block, addr, length, fileInfo->startAddr + static_cast<u32>(offset >> 2), nullptr, prio) == FALSE) {
        return -1;
    }
    waitUntilIdle(d, block);
    switch (block->state) {
    case DVD_STATE_END:
        return static_cast<s32>(block->transferredSize);
    case DVD_STATE_CANCELED:
        return -3;
    default:
        return -1;
    }
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    Drive& d = drive();
    InterruptGuard guard;
    return block->state;
}

s32 DVDGetDriveStatus(void) {
    Drive& d = drive();
    InterruptGuard guard;
    if (d.fatal) {
        return DVD_STATE_FATAL_ERROR;
    }
    if (d.paused && d.executing == nullptr) {
        return DVD_STATE_PAUSING;
    }
    if (d.executing != nullptr) {
        return d.executing->state;
    }
    return DVD_STATE_END;
}

BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    Drive& d = drive();
    InterruptGuard guard;
    if (d.executing == block) {
        if (d.cancelRequested) {
            return FALSE;
        }
        d.cancelRequested = true;
        d.cancelCallback = callback;
        d.changed.notify_all();
        return TRUE;
    }
    for (auto& queue : d.queues) {
        if (queue.remove(block)) {
            storeState(block, DVD_STATE_CANCELED);
            if (block->callback) {
                block->callback(-3, block);
            }
            if (callback) {
                callback(0, block);
            }
            signalChange(d);
            return TRUE;
        }
    }
    // Not pending: nothing to cancel.
    if (callback) {
        callback(0, block);
    }
    return TRUE;
}

s32 DVDCancel(DVDCommandBlock* block) {
    Drive& d = drive();
    InterruptGuard guard;
    if (DVDCancelAsync(block, nullptr) == FALSE) {
        return -1;
    }
    if (d.executing == block) {
        requireNotDriveThread(d, "DVDCancel");
    }
    waitUntilIdle(d, block);
    return 0;
}

BOOL DVDCancelAllAsync(DVDCBCallback callback) {
    Drive& d = drive();
    InterruptGuard guard;
    d.paused = true;
    while (DVDCommandBlock* block = popWaiting(d)) {
        storeState(block, DVD_STATE_CANCELED);
        if (block->callback) {
            block->callback(-3, block);
        }
    }
    BOOL result = TRUE;
    if (d.executing != nullptr) {
        result = DVDCancelAsync(d.executing, callback);
    } else if (callback) {
        callback(0, nullptr);
    }
    // Like the SDK, this resumes the drive even if it was paused before.
    d.paused = false;
    d.changed.notify_all();
    return result;
}

BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    Drive& d = drive();
    InterruptGuard guard;
    s32 state;
    if (d.fatal) {
        state = DVD_STATE_FATAL_ERROR;
    } else if (d.paused) {
        state = DVD_STATE_PAUSING;
    } else if (d.executing != nullptr) {
        state = d.executing->state;
    } else {
        state = d.fst.load() ? DVD_STATE_END : DVD_STATE_NO_DISK;
    }

    switch (state) {
    case DVD_STATE_BUSY:
    case DVD_STATE_WAITING:
    case DVD_STATE_IGNORED:
    case DVD_STATE_CANCELED:
        storeState(block, DVD_STATE_END);
        if (callback) {
            callback(TRUE, block);
        }
        return TRUE;
    case DVD_STATE_END:
    case DVD_STATE_PAUSING:
        block->command = kCommandCheckDisk;
        block->callback = callback;
        return issueCommand(2, block);
    default:
        storeState(block, DVD_STATE_END);
        if (callback) {
            callback(FALSE, block);
        }
        return TRUE;
    }
}

DVDDiskID* DVDGetCurrentDiskID(void) {
    return &gDiskId;
}

BOOL DVDSetAutoInvalidation(BOOL autoInval) {
    // Host memory is cache coherent with file reads, so there is nothing to
    // invalidate; the flag is kept so callers see the SDK's return value.
    Drive& d = drive();
    InterruptGuard guard;
    const BOOL prev = d.autoInvalidation;
    d.autoInvalidation = autoInval;
    return prev;
}

void DVDPause(void) {
    Drive& d = drive();
    InterruptGuard guard;
    d.paused = true;
}

void DVDResume(void) {
    Drive& d = drive();
    InterruptGuard guard;
    d.paused = false;
    d.changed.notify_all();
}

}  // extern "C"

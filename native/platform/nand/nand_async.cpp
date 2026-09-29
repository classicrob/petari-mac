// Asynchronous NAND API (the subset RVLFaceLib uses), ported from the SDK's
// nand.c and NANDOpenClose.c onto the host ISFS.
//
// As on the console, an asynchronous call validates its arguments and returns
// at once: NAND_RESULT_OK when the request was queued, or an immediate error
// (library not initialised, private path from the public API, invalid access
// type or permission, file not opened by safe open). The file-system result
// arrives later through the callback, which runs on the NAND request thread
// (the IOS/IPC side) with interrupts disabled, receiving the command block
// passed in; a missing file is therefore reported to the callback as
// NAND_RESULT_NOEXISTS, not by the call. Requests complete in submission
// order.
//
// Safe open/close keep the SDK's copy-on-write scheme: writing opens copy the
// file into /tmp/sys/<unique>/, and safe close moves the copy over the
// original, so an interrupted write never damages it.

#include <revolution/nand.h>
#include <revolution/os.h>

#include <pthread.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

#include "isfs_host.hpp"
#include "nand_internal.hpp"
#include "petari/host_allocation.hpp"

namespace Fs = PetariNative::Platform::NAND::Fs;

namespace {

struct Worker {
    std::mutex lock;
    std::condition_variable changed;
    std::deque<std::function<void()>> queue;
    bool running = false;
    bool stop = false;
    pthread_t thread{};
    std::uint32_t uniqueCounter = 0;
};

Worker& worker() {
    static Worker* instance = new Worker;
    return *instance;
}

void* workerMain(void*) {
    PetariNative::HostAllocationScope hostAllocations;
    Worker& w = worker();
    std::unique_lock<std::mutex> lock(w.lock);
    while (true) {
        w.changed.wait(lock, [&] { return w.stop || !w.queue.empty(); });
        if (w.stop) {
            return nullptr;
        }
        std::function<void()> job = std::move(w.queue.front());
        w.queue.pop_front();
        lock.unlock();
        job();
        {
            // Destroy the job (and its captures) under the host allocator.
            std::function<void()> discard = std::move(job);
        }
        lock.lock();
    }
}

s32 submit(std::function<void()> job) {
    PetariNative::HostAllocationScope hostAllocations;
    Worker& w = worker();
    std::lock_guard<std::mutex> guard(w.lock);
    if (!w.running) {
        w.stop = false;
        if (pthread_create(&w.thread, nullptr, workerMain, nullptr) != 0) {
            return NAND_RESULT_ALLOC_FAILED;
        }
        w.running = true;
    }
    w.queue.push_back(std::move(job));
    w.changed.notify_one();
    return NAND_RESULT_OK;
}

// Completion, as an IPC interrupt delivers it.
void complete(NANDCommandBlock* block, s32 result) {
    BOOL enabled = OSDisableInterrupts();
    reinterpret_cast<NANDCallback>(block->callback)(result, block);
    OSRestoreInterrupts(enabled);
}

void completeIsfs(NANDCommandBlock* block, ISFSError result) {
    complete(block, nandConvertErrorCode(result));
}

s32 prepare(NANDCommandBlock* block, NANDCallback cb) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    block->callback = reinterpret_cast<void*>(cb);
    return NAND_RESULT_OK;
}

void splitPerm(u8 perm, u32* owner, u32* group, u32* others) {
    *owner = ((perm & 0x10) ? 1 : 0) | ((perm & 0x20) ? 2 : 0);
    *group = ((perm & 0x04) ? 1 : 0) | ((perm & 0x08) ? 2 : 0);
    *others = ((perm & 0x01) ? 1 : 0) | ((perm & 0x02) ? 2 : 0);
}

// Writing safe open: /tmp/sys, attributes, original opened for reading, a
// unique directory, the copy created with the original's attributes and
// opened with the requested access, the data copied, the copy rewound.
ISFSError safeOpenForWrite(NANDFileInfo* info, NANDCommandBlock* block) {
    ISFSError err = Fs::createDir("/tmp/sys", 0, 3, 3, 3);
    if (err != ISFS_ERROR_OK && err != ISFS_ERROR_EXISTS) {
        return err;
    }
    info->stage = 1;
    Fs::Attr attr;
    if ((err = Fs::getAttr(info->origPath, &attr)) != ISFS_ERROR_OK) {
        return err;
    }
    const s32 origFd = Fs::open(info->origPath, 1);
    if (origFd < 0) {
        return origFd;
    }
    info->origFd = origFd;
    info->stage = 2;

    {
        std::lock_guard<std::mutex> guard(worker().lock);
        block->uniqNo = worker().uniqueCounter++;
    }
    char tmpDir[64];
    std::snprintf(tmpDir, sizeof(tmpDir), "/tmp/sys/%08x", block->uniqNo);
    if ((err = Fs::createDir(tmpDir, 0, 3, 0, 0)) != ISFS_ERROR_OK) {
        return err;
    }
    char name[64];
    nandGetRelativeName(name, info->origPath);
    info->stage = 3;
    std::snprintf(info->tmpPath, sizeof(info->tmpPath), "%s/%s", tmpDir, name);
    if ((err = Fs::createFile(info->tmpPath, attr.attr, attr.ownerAccess, attr.groupAccess, attr.othersAccess)) != ISFS_ERROR_OK) {
        return err;
    }
    info->stage = 4;
    const s32 fd = Fs::open(info->tmpPath, info->accType);
    if (fd < 0) {
        return fd;
    }
    info->fileDescriptor = fd;
    info->stage = 5;

    auto* buffer = static_cast<u8*>(block->copyBuf);
    while (true) {
        const s32 got = Fs::read(origFd, buffer, block->bufLength);
        if (got < 0) {
            return got;
        }
        if (got == 0) {
            break;
        }
        const s32 put = Fs::write(fd, buffer, static_cast<u32>(got));
        if (put < 0) {
            return put;
        }
    }
    const s32 pos = Fs::seek(fd, 0, 0);
    if (pos < 0) {
        return pos;
    }
    info->mark = 3;
    return ISFS_ERROR_OK;
}

ISFSError safeCloseAfterWrite(NANDFileInfo* info) {
    ISFSError err;
    if ((err = Fs::close(info->fileDescriptor)) != ISFS_ERROR_OK) {
        return err;
    }
    info->stage = 6;
    if ((err = Fs::close(info->origFd)) != ISFS_ERROR_OK) {
        return err;
    }
    info->stage = 7;
    if ((err = Fs::rename(info->tmpPath, info->origPath)) != ISFS_ERROR_OK) {
        return err;
    }
    info->stage = 8;
    char tmpDir[64] = "";
    nandGetParentDirectory(tmpDir, info->tmpPath);
    if ((err = Fs::remove(tmpDir)) != ISFS_ERROR_OK) {
        return err;
    }
    info->stage = 9;
    info->mark = 4;
    return ISFS_ERROR_OK;
}

}  // namespace

namespace PetariNative::Platform::NAND {

void stopAsync() {
    Worker& w = worker();
    bool join;
    {
        std::lock_guard<std::mutex> guard(w.lock);
        join = w.running;
        w.stop = true;
    }
    w.changed.notify_all();
    if (join) {
        pthread_join(w.thread, nullptr);
    }
    PetariNative::HostAllocationScope hostAllocations;
    std::lock_guard<std::mutex> guard(w.lock);
    w.queue.clear();
    w.running = false;
    w.stop = false;
}

}  // namespace PetariNative::Platform::NAND

namespace PNAND = PetariNative::Platform::NAND;

extern "C" {

s32 NANDReadAsync(NANDFileInfo* info, void* buf, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    const s32 fd = info->fileDescriptor;
    return submit([=] { complete(block, nandConvertErrorCode(Fs::read(fd, static_cast<u8*>(buf), length))); });
}

s32 NANDWriteAsync(NANDFileInfo* info, const void* buf, u32 length, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    const s32 fd = info->fileDescriptor;
    return submit([=] { complete(block, nandConvertErrorCode(Fs::write(fd, static_cast<const u8*>(buf), length))); });
}

s32 NANDSeekAsync(NANDFileInfo* info, s32 offset, s32 whence, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    const s32 fd = info->fileDescriptor;
    const u32 w = (whence >= 0 && whence <= 2) ? static_cast<u32>(whence) : 0xFFFFFFFF;
    return submit([=] { complete(block, nandConvertErrorCode(Fs::seek(fd, offset, w))); });
}

s32 NANDGetLengthAsync(NANDFileInfo* info, u32* length, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    block->length = length;
    block->pos = nullptr;
    const s32 fd = info->fileDescriptor;
    return submit([=] {
        ISFSFileStats stats;
        const ISFSError err = Fs::fileStats(fd, &stats);
        if (err == ISFS_ERROR_OK && block->length) {
            *block->length = stats.size;
        }
        completeIsfs(block, err);
    });
}

s32 NANDPrivateCreateAsync(const char* path, u8 perm, u8 attr, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    if (!(perm & 0x10)) {
        return NAND_RESULT_INVALID;
    }
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    std::string target(absPath);
    return submit([=] { completeIsfs(block, PNAND::createNode(target.c_str(), perm, attr, true, false)); });
}

s32 NANDPrivateCreateDirAsync(const char* path, u8 perm, u8 attr, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    if (!(perm & 0x10)) {
        return NAND_RESULT_INVALID;
    }
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    std::string target(absPath);
    return submit([=] { completeIsfs(block, PNAND::createNode(target.c_str(), perm, attr, true, true)); });
}

s32 NANDPrivateDeleteAsync(const char* path, NANDCallback cb, NANDCommandBlock* block) {
    if (s32 r = prepare(block, cb); r != NAND_RESULT_OK) {
        return r;
    }
    char absPath[64] = "";
    nandGenerateAbsPath(absPath, path);
    std::string target(absPath);
    return submit([=] { completeIsfs(block, PNAND::deleteNode(target.c_str(), true)); });
}

s32 NANDPrivateSafeOpenAsync(const char* path, NANDFileInfo* info, const u8 accType, void* buf, const u32 length, NANDCallback cb,
                             NANDCommandBlock* block) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    info->accType = accType;
    info->stage = 0;
    block->simpleFlag = FALSE;
    nandGenerateAbsPath(info->origPath, path);
    block->fileInfo = info;
    block->callback = reinterpret_cast<void*>(cb);
    if (accType == NAND_ACCESS_READ) {
        return submit([=] {
            const s32 fd = Fs::open(info->origPath, 1);
            if (fd >= 0) {
                info->fileDescriptor = fd;
                info->stage = 2;
                info->mark = 3;
                complete(block, NAND_RESULT_OK);
            } else {
                completeIsfs(block, fd);
            }
        });
    }
    if (accType == NAND_ACCESS_WRITE || accType == NAND_ACCESS_RW) {
        if (buf == nullptr || length == 0) {
            return NAND_RESULT_INVALID;
        }
        block->state = 0;
        block->copyBuf = buf;
        block->bufLength = length;
        return submit([=] { completeIsfs(block, safeOpenForWrite(info, block)); });
    }
    return NAND_RESULT_INVALID;
}

s32 NANDSafeCloseAsync(NANDFileInfo* info, NANDCallback cb, NANDCommandBlock* block) {
    if (!nandIsInitialized()) {
        return NAND_RESULT_FATAL_ERROR;
    }
    if (info->mark != 3) {
        return NAND_RESULT_INVALID;
    }
    block->simpleFlag = FALSE;
    block->fileInfo = info;
    block->callback = reinterpret_cast<void*>(cb);
    if (info->accType == NAND_ACCESS_READ) {
        return submit([=] {
            const ISFSError err = Fs::close(info->fileDescriptor);
            if (err == ISFS_ERROR_OK) {
                info->stage = 7;
                info->mark = 4;
            }
            completeIsfs(block, err);
        });
    }
    if (info->accType == NAND_ACCESS_WRITE || info->accType == NAND_ACCESS_RW) {
        block->state = 10;
        return submit([=] { completeIsfs(block, safeCloseAfterWrite(info)); });
    }
    return NAND_RESULT_INVALID;
}

}  // extern "C"

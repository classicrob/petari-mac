// Tests for the native OS layer: interrupts, the uniprocessor scheduler,
// mutexes (SDK OSMutex.c), message queues (SDK OSMessage.c), alarms, and time.

#include <revolution/dvd.h>
#include <revolution/os.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "dvd_internal.hpp"
#include "os_internal.hpp"
#include "os_sdk_private.h"
#include "petari/platform/dvd.hpp"
#include "petari/platform/os_host.hpp"

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

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

// Game-style thread storage: OSThread plus a game-owned stack buffer.
struct TestThread {
    OSThread thread;
    alignas(32) u8 stack[0x4000];
    bool create(void* (*func)(void*), void* param, OSPriority prio, u16 attr = 0) {
        return OSCreateThread(&thread, func, param, stack + sizeof(stack), sizeof(stack), prio, attr);
    }
};

std::vector<std::string> gLog;
void log(const char* event) {
    // Only one OS thread runs at a time, so the log needs no lock.
    gLog.emplace_back(event);
}

void testInterrupts() {
    check(OSDisableInterrupts() == TRUE, "disable from enabled returns TRUE");
    check(OSDisableInterrupts() == FALSE, "disable while disabled returns FALSE");
    check(OSRestoreInterrupts(FALSE) == FALSE, "restore FALSE keeps interrupts disabled");
    check(OSRestoreInterrupts(TRUE) == FALSE, "restore TRUE enables and reports previous disabled");
    check(OSEnableInterrupts() == TRUE, "enable while enabled returns TRUE");
    check(OSRestoreInterrupts(FALSE) == TRUE, "restore FALSE from enabled disables");
    check(OSEnableInterrupts() == FALSE, "enable from disabled returns FALSE");

    // Interrupts exclude other host threads (interrupt handlers).
    std::atomic<int> stage{0};
    BOOL level = OSDisableInterrupts();
    std::thread handler([&] {
        stage = 1;
        BOOL e = OSDisableInterrupts();
        stage = 2;
        OSRestoreInterrupts(e);
    });
    while (stage.load() != 1) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(stage.load() == 1, "interrupt handler waits while interrupts are disabled");
    OSRestoreInterrupts(level);
    handler.join();
    check(stage.load() == 2, "interrupt handler runs after interrupts are restored");
}

void* recordHigh(void* param) {
    log(static_cast<const char*>(param));
    return reinterpret_cast<void*>(intptr_t(42));
}

void testPriorityPreemption() {
    gLog.clear();
    OSThread* main = OSGetCurrentThread();
    check(main != nullptr && OSGetThreadPriority(main) == 16, "default thread has priority 16");

    TestThread high, low;
    check(high.create(recordHigh, const_cast<char*>("high"), 8), "create high-priority thread");
    check(low.create(recordHigh, const_cast<char*>("low"), 24), "create low-priority thread");
    check(OSIsThreadSuspended(&high.thread) && high.thread.state == OS_THREAD_STATE_READY, "new thread is suspended and ready");
    check(high.thread.stackEnd == reinterpret_cast<u32*>(high.stack) && *high.thread.stackEnd == 0xDEADBABE, "stack end marker");

    log("before-resume");
    check(OSResumeThread(&low.thread) == 1, "resume returns previous suspend count");
    log("after-low-resume");
    OSResumeThread(&high.thread);
    log("after-high-resume");
    void* value = nullptr;
    check(OSJoinThread(&high.thread, &value) && value == reinterpret_cast<void*>(intptr_t(42)), "join returns the exit value");
    check(OSJoinThread(&low.thread, nullptr), "join low thread");
    const std::vector<std::string> expected{"before-resume", "after-low-resume", "high", "after-high-resume", "low"};
    check(gLog == expected, "higher priority preempts on resume; lower priority waits until the current thread blocks");
    check(high.thread.state == 0 && OSIsThreadTerminated(&high.thread), "joined thread is freed");
}

// Several equal-priority threads interleave only at OS calls.
std::atomic<int> gActive{0};
int gCounter = 0;
void* exclusiveWorker(void*) {
    for (int i = 0; i < 2000; ++i) {
        check(gActive.fetch_add(1) == 0, "only one OS thread runs at a time");
        int value = gCounter;  // intentionally unsynchronised: the baton serialises threads
        value += 1;
        gCounter = value;
        gActive.fetch_sub(1);
        if (i % 3 == 0) {
            OSYieldThread();
        }
    }
    return nullptr;
}

void testUniprocessor() {
    gCounter = 0;
    TestThread workers[4];
    for (auto& w : workers) {
        check(w.create(exclusiveWorker, nullptr, 20), "create worker");
        OSResumeThread(&w.thread);
    }
    for (auto& w : workers) {
        check(OSJoinThread(&w.thread, nullptr), "join worker");
    }
    check(gCounter == 8000, "unsynchronised counter is exact under the uniprocessor scheduler");
}

OSMessageQueue gQueue;
OSMessage gQueueSlots[2];
std::vector<intptr_t> gReceived;

void* consumer(void*) {
    for (int i = 0; i < 6; ++i) {
        OSMessage msg;
        OSReceiveMessage(&gQueue, &msg, OS_MESSAGE_BLOCK);
        gReceived.push_back(reinterpret_cast<intptr_t>(msg));
    }
    return nullptr;
}

void testMessages() {
    OSInitMessageQueue(&gQueue, gQueueSlots, 2);
    OSMessage msg;
    check(!OSReceiveMessage(&gQueue, &msg, OS_MESSAGE_NOBLOCK), "non-blocking receive on empty queue fails");
    check(OSSendMessage(&gQueue, reinterpret_cast<OSMessage>(1), OS_MESSAGE_NOBLOCK), "send 1");
    check(OSJamMessage(&gQueue, reinterpret_cast<OSMessage>(0), OS_MESSAGE_NOBLOCK), "jam 0 at the front");
    check(!OSSendMessage(&gQueue, reinterpret_cast<OSMessage>(9), OS_MESSAGE_NOBLOCK), "non-blocking send on full queue fails");

    gReceived.clear();
    TestThread consumerThread;
    consumerThread.create(consumer, nullptr, 20);
    OSResumeThread(&consumerThread.thread);
    // Blocking sends: the queue is full, so the main thread sleeps and the
    // lower-priority consumer runs.
    for (intptr_t i = 2; i < 6; ++i) {
        check(OSSendMessage(&gQueue, reinterpret_cast<OSMessage>(i), OS_MESSAGE_BLOCK), "blocking send");
    }
    OSJoinThread(&consumerThread.thread, nullptr);
    check((gReceived == std::vector<intptr_t>{0, 1, 2, 3, 4, 5}), "messages arrive in order with jammed message first");
}

OSMutex gMutex;
std::vector<std::string> gMutexLog;
OSMessageQueue gGate;
OSMessage gGateSlot[1];
OSMessageQueue gLocked;
OSMessage gLockedSlot[1];

void* lowOwner(void*) {
    OSLockMutex(&gMutex);
    gMutexLog.push_back("low-locked");
    OSSendMessage(&gLocked, nullptr, OS_MESSAGE_BLOCK);
    OSMessage msg;
    OSReceiveMessage(&gGate, &msg, OS_MESSAGE_BLOCK);  // let main start the high thread
    gMutexLog.push_back("low-priority-" + std::to_string(OSGetCurrentThread()->priority));
    OSUnlockMutex(&gMutex);
    gMutexLog.push_back("low-unlocked-priority-" + std::to_string(OSGetCurrentThread()->priority));
    return nullptr;
}

void* highWaiter(void*) {
    gMutexLog.push_back("high-waiting");
    OSLockMutex(&gMutex);
    gMutexLog.push_back("high-locked");
    OSUnlockMutex(&gMutex);
    return nullptr;
}

void testMutex() {
    OSInitMutex(&gMutex);
    OSLockMutex(&gMutex);
    OSLockMutex(&gMutex);
    check(gMutex.count == 2 && gMutex.thread == OSGetCurrentThread(), "mutex is recursive for its owner");
    check(OSTryLockMutex(&gMutex) && gMutex.count == 3, "try-lock by owner succeeds");
    OSUnlockMutex(&gMutex);
    OSUnlockMutex(&gMutex);
    OSUnlockMutex(&gMutex);
    check(gMutex.thread == nullptr && gMutex.count == 0, "mutex released");

    OSInitMessageQueue(&gGate, gGateSlot, 1);
    OSInitMessageQueue(&gLocked, gLockedSlot, 1);
    gMutexLog.clear();
    TestThread low, high;
    low.create(lowOwner, nullptr, 20);
    high.create(highWaiter, nullptr, 4);
    OSResumeThread(&low.thread);
    OSMessage locked;
    OSReceiveMessage(&gLocked, &locked, OS_MESSAGE_BLOCK);  // low runs, locks, and reports
    OSYieldThread();  // no equal-or-higher thread is ready: main continues while low waits on the gate
    check(gMutex.thread == &low.thread, "low thread owns the mutex");
    check(!OSTryLockMutex(&gMutex), "try-lock of a mutex owned elsewhere fails");
    OSResumeThread(&high.thread);  // high blocks on the mutex and promotes low
    check(low.thread.priority == 4 && low.thread.base == 20, "owner inherits the waiter's priority");
    OSSendMessage(&gGate, nullptr, OS_MESSAGE_BLOCK);  // low (now priority 4) preempts main
    OSJoinThread(&high.thread, nullptr);
    OSJoinThread(&low.thread, nullptr);
    const std::vector<std::string> expected{"low-locked", "high-waiting", "low-priority-4", "high-locked", "low-unlocked-priority-20"};
    check(gMutexLog == expected, "priority inheritance: unlocking hands off to the waiter immediately");
}

std::atomic<int> gSpins{0};
void* spinner(void* param) {
    auto* stop = static_cast<volatile bool*>(param);
    while (!*stop) {
        gSpins.fetch_add(1);
        OSYieldThread();
    }
    return nullptr;
}

void* exitFromNested(void*) {
    struct Helper {
        static void deep() { OSExitThread(reinterpret_cast<void*>(intptr_t(7))); }
    };
    Helper::deep();
    log("not reached");
    return nullptr;
}

void* waitsForever(void*) {
    OSThreadQueue queue;
    OSInitThreadQueue(&queue);
    OSSleepThread(&queue);
    log("woke unexpectedly");
    return nullptr;
}

void testLifecycle() {
    gLog.clear();
    TestThread exiter;
    exiter.create(exitFromNested, nullptr, 10);
    OSResumeThread(&exiter.thread);
    void* value = nullptr;
    check(OSJoinThread(&exiter.thread, &value) && value == reinterpret_cast<void*>(intptr_t(7)), "OSExitThread from a nested call");
    check(gLog.empty(), "code after OSExitThread does not run");

    TestThread sleeper;
    sleeper.create(waitsForever, nullptr, 10);
    OSResumeThread(&sleeper.thread);
    check(sleeper.thread.state == OS_THREAD_STATE_WAITING, "thread sleeps on its queue");
    OSCancelThread(&sleeper.thread);
    check(sleeper.thread.state == OS_THREAD_STATE_MORIBUND, "cancelled joinable thread is moribund");
    check(OSJoinThread(&sleeper.thread, &value) && value == reinterpret_cast<void*>(-1), "join cancelled thread");
    check(gLog.empty(), "cancelled thread never resumes");

    TestThread detached;
    detached.create(recordHigh, const_cast<char*>("detached"), 10, 1);
    OSResumeThread(&detached.thread);
    check(detached.thread.state == 0 && !OSJoinThread(&detached.thread, nullptr), "detached thread frees itself and cannot be joined");

    TestThread never;
    never.create(recordHigh, const_cast<char*>("never"), 10);
    OSCancelThread(&never.thread);
    check(OSJoinThread(&never.thread, nullptr) && gLog.size() == 1, "cancelling a never-resumed thread");

    // Suspension counts nest.
    volatile bool stop = false;
    TestThread spin;
    spin.create(spinner, const_cast<bool*>(&stop), 16);
    check(OSResumeThread(&spin.thread) == 1, "resume new thread");
    OSYieldThread();
    const int afterYield = gSpins.load();
    check(afterYield > 0, "equal-priority thread runs when main yields");
    check(OSSuspendThread(&spin.thread) == 0 && OSSuspendThread(&spin.thread) == 1, "suspend counts nest");
    OSYieldThread();
    check(gSpins.load() == afterYield, "suspended thread does not run");
    check(OSResumeThread(&spin.thread) == 2 && OSIsThreadSuspended(&spin.thread), "one resume leaves it suspended");
    OSResumeThread(&spin.thread);
    stop = true;
    OSJoinThread(&spin.thread, nullptr);

    // Priority changes take effect immediately.
    TestThread bumped;
    gLog.clear();
    bumped.create(recordHigh, const_cast<char*>("bumped"), 30);
    OSResumeThread(&bumped.thread);
    check(gLog.empty(), "priority 30 thread waits");
    check(OSSetThreadPriority(&bumped.thread, 2) && OSGetThreadPriority(&bumped.thread) == 2, "raise priority");
    check(gLog.size() == 1, "raised thread preempts immediately");
    OSJoinThread(&bumped.thread, nullptr);
    check(!OSSetThreadPriority(&bumped.thread, 32), "priority range is checked");
    OSThread bad;
    check(!OSCreateThread(&bad, recordHigh, nullptr, nullptr, 0, -1, 0), "invalid priority is rejected");
}

void testScheduler() {
    gLog.clear();
    TestThread high;
    high.create(recordHigh, const_cast<char*>("high"), 2);
    check(OSDisableScheduler() == 0, "disable scheduler");
    OSResumeThread(&high.thread);
    check(gLog.empty(), "no switch while the scheduler is disabled");
    check(OSEnableScheduler() == 1, "enable scheduler");
    OSYieldThread();
    check(gLog.size() == 1, "switch happens at the next scheduling point");
    OSJoinThread(&high.thread, nullptr);
}

std::atomic<int> gAlarmFires{0};
OSMessageQueue gAlarmQueue;
OSMessage gAlarmSlots[8];
void alarmHandler(OSAlarm* alarm, OSContext* context) {
    check(context != nullptr, "alarm handler receives a context");
    gAlarmFires.fetch_add(1);
    OSSendMessage(&gAlarmQueue, OSGetAlarmUserData(alarm), OS_MESSAGE_NOBLOCK);
}

volatile bool gHighRan = false;
void* alarmWaiter(void*) {
    OSMessage msg;
    OSReceiveMessage(&gAlarmQueue, &msg, OS_MESSAGE_BLOCK);
    gHighRan = true;
    return msg;
}

void testAlarmsAndSleep() {
    OSInitMessageQueue(&gAlarmQueue, gAlarmSlots, 8);

    // One-shot alarm wakes an idle CPU: main blocks, the timer interrupt wakes it.
    OSAlarm alarm;
    OSCreateAlarm(&alarm);
    OSSetAlarmUserData(&alarm, reinterpret_cast<void*>(intptr_t(5)));
    const OSTime start = OSGetTime();
    OSSetAlarm(&alarm, OSMillisecondsToTicks(30), alarmHandler);
    OSMessage msg = nullptr;
    OSReceiveMessage(&gAlarmQueue, &msg, OS_MESSAGE_BLOCK);
    const OSTime elapsed = OSGetTime() - start;
    check(msg == reinterpret_cast<void*>(intptr_t(5)), "alarm handler message");
    check(elapsed >= OSMillisecondsToTicks(30) && elapsed < OSMillisecondsToTicks(1000), "alarm fires after its delay");

    // Cancelled alarm never fires.
    gAlarmFires = 0;
    OSSetAlarm(&alarm, OSMillisecondsToTicks(20), alarmHandler);
    OSCancelAlarm(&alarm);
    OSSleepTicks(OSMillisecondsToTicks(40));
    check(gAlarmFires.load() == 0, "cancelled alarm does not fire");

    // Periodic alarm.
    OSSetPeriodicAlarm(&alarm, OSGetTime(), OSMillisecondsToTicks(10), alarmHandler);
    for (int i = 0; i < 3; ++i) {
        OSReceiveMessage(&gAlarmQueue, &msg, OS_MESSAGE_BLOCK);
    }
    OSCancelAlarm(&alarm);
    check(gAlarmFires.load() >= 3, "periodic alarm repeats");
    while (OSReceiveMessage(&gAlarmQueue, &msg, OS_MESSAGE_NOBLOCK)) {
    }

    // OSSleepTicks lets lower-priority threads run.
    volatile bool stop = false;
    gSpins = 0;
    TestThread spin;
    spin.create(spinner, const_cast<bool*>(&stop), 28);
    OSResumeThread(&spin.thread);
    const OSTime sleepStart = OSGetTime();
    OSSleepTicks(OSMillisecondsToTicks(20));
    check(OSGetTime() - sleepStart >= OSMillisecondsToTicks(20), "OSSleepTicks sleeps at least the requested time");
    // Host thread start-up latency is outside the emulated timeline, so allow
    // several sleeps for the new thread's first run.
    for (int i = 0; i < 100 && gSpins.load() == 0; ++i) {
        OSSleepTicks(OSMillisecondsToTicks(20));
    }
    check(gSpins.load() > 0, "lower-priority thread runs while the main thread sleeps");

    // An interrupt that readies a higher-priority thread preempts a running
    // lower-priority thread at its next interrupt-state change.
    gHighRan = false;
    TestThread waiter;
    waiter.create(alarmWaiter, nullptr, 4);
    OSResumeThread(&waiter.thread);  // runs and blocks on the queue
    OSSetAlarm(&alarm, OSMillisecondsToTicks(10), alarmHandler);
    bool sawPreemption = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        BOOL e = OSDisableInterrupts();
        OSRestoreInterrupts(e);
        if (gHighRan) {
            sawPreemption = true;
            break;
        }
    }
    check(sawPreemption, "alarm-woken high-priority thread preempts a busy lower-priority thread");
    OSJoinThread(&waiter.thread, nullptr);
    stop = true;
    OSJoinThread(&spin.thread, nullptr);
}

void testTime() {
    const OSTime a = OSGetTime();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const OSTime b = OSGetTime();
    check(b - a >= OSMillisecondsToTicks(5) && b - a < OSMillisecondsToTicks(500), "OSGetTime advances at 60.75 MHz");
    check(static_cast<OSTick>(b) == static_cast<OSTick>(b), "OSGetTick");

    OSCalendarTime cal;
    OSTicksToCalendarTime(OSGetTime(), &cal);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    check(cal.year == local.tm_year + 1900 && cal.mon == local.tm_mon && cal.mday == local.tm_mday, "calendar date is local today");
    check(cal.hour == local.tm_hour, "calendar hour is local time");

    OSTicksToCalendarTime(0, &cal);
    check(cal.year == 2000 && cal.mon == 0 && cal.mday == 1 && cal.hour == 0 && cal.wday == 6, "tick 0 is Saturday 2000-01-01");
    OSTicksToCalendarTime(OSSecondsToTicks(s64(86400) * 366 + 3661) + OSMillisecondsToTicks(5), &cal);
    check(cal.year == 2001 && cal.yday == 0 && cal.hour == 1 && cal.min == 1 && cal.sec == 1 && cal.msec == 5, "calendar conversion after a leap year");
}

// ---- DVD on the OS scheduler ----

namespace fs = std::filesystem;
namespace PDVD = PetariNative::Platform::DVD;

std::string gDiscData;
OSMessageQueue gDvdQueue;
OSMessage gDvdSlots[4];

void dvdDone(s32 result, DVDFileInfo* info) {
    // JKRDvdFile::doneProcess pattern: a DI-interrupt callback posts a message.
    OSSendMessage(&gDvdQueue, reinterpret_cast<OSMessage>(static_cast<intptr_t>(result)), OS_MESSAGE_NOBLOCK);
    (void)info;
}

struct LoaderArgs {
    DVDFileInfo file;
    std::vector<char> buffer;
    s32 result = 0;
};

void* loader(void* param) {
    auto* args = static_cast<LoaderArgs*>(param);
    args->result = DVDReadPrio(&args->file, args->buffer.data(), static_cast<s32>(args->buffer.size()), 0, 2);
    return nullptr;
}

void testDvdOnOsThreads() {
    const char* base = std::getenv("TMPDIR");
    const fs::path root = fs::path(base ? base : "/tmp") / ("petari_os_dvd_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "files" / "Data");
    gDiscData.resize(0x40000);
    for (std::size_t i = 0; i < gDiscData.size(); ++i) {
        gDiscData[i] = static_cast<char>((i * 7 + (i >> 9)) & 0xFF);
    }
    std::ofstream(root / "files" / "Data" / "big.bin", std::ios::binary).write(gDiscData.data(), static_cast<std::streamsize>(gDiscData.size()));
    std::string error;
    check(PDVD::mount({root}, &error), "mount synthetic disc");
    DVDInit();
    OSInitMessageQueue(&gDvdQueue, gDvdSlots, 4);

    // Async read: the main thread blocks in OSReceiveMessage (CPU idle) and the
    // drive's completion callback wakes it.
    DVDFileInfo file;
    check(DVDOpen("/Data/big.bin", &file), "open on OS thread");
    std::vector<char> buffer(0x40000);
    check(DVDReadAsyncPrio(&file, buffer.data(), 0x40000, 0, dvdDone, 2), "async read");
    OSMessage msg;
    OSReceiveMessage(&gDvdQueue, &msg, OS_MESSAGE_BLOCK);
    check(reinterpret_cast<intptr_t>(msg) == 0x40000 && std::memcmp(buffer.data(), gDiscData.data(), buffer.size()) == 0,
          "DVD callback message wakes the waiting OS thread with the data read");

    // A loader thread blocked in DVDReadPrio gives the CPU to other threads.
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
    LoaderArgs args;
    check(DVDOpen("/Data/big.bin", &args.file), "open for loader");
    args.buffer.resize(0x40000);
    TestThread loaderThread;
    loaderThread.create(loader, &args, 8);  // higher priority than main
    OSResumeThread(&loaderThread.thread);   // runs at once, issues the read, sleeps
    check(loaderThread.thread.state == OS_THREAD_STATE_WAITING, "loader sleeps in DVDReadPrio");
    check(OSGetCurrentThread() != &loaderThread.thread, "main runs while the higher-priority loader waits for the disc");
    {
        std::unique_lock<std::mutex> g(hookLock);
        hookCv.wait(g, [&] { return inChunk; });
        release = true;
        hookCv.notify_all();
    }
    OSJoinThread(&loaderThread.thread, nullptr);
    check(args.result == 0x40000 && std::memcmp(args.buffer.data(), gDiscData.data(), args.buffer.size()) == 0, "loader read result");

    // DVDCancel from an OS thread waits (sleeping) for the busy chunk to end.
    inChunk = false;
    release = false;
    check(DVDReadAsyncPrio(&file, buffer.data(), 0x40000, 0, dvdDone, 2), "read to cancel");
    {
        std::unique_lock<std::mutex> g(hookLock);
        hookCv.wait(g, [&] { return inChunk; });
    }
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> g(hookLock);
        release = true;
        hookCv.notify_all();
    });
    check(DVDCancel(&file.cb) == 0 && file.cb.state == DVD_STATE_CANCELED, "DVDCancel of a busy read from an OS thread");
    releaser.join();
    OSReceiveMessage(&gDvdQueue, &msg, OS_MESSAGE_BLOCK);
    check(reinterpret_cast<intptr_t>(msg) == -3, "cancelled read callback reports -3");
    PDVD::Testing::setChunkHook({});
    PDVD::Testing::setChunkSize(0x80000);

    PDVD::shutdown();
    fs::remove_all(root);
}

// ---- Cache operations ----

std::atomic<bool> gPublished{false};

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PETARI_TSAN 1
#endif
#endif

void testCache() {
    // Producer/consumer across host threads with the Wii DMA protocol: write,
    // DCFlushRange, publish; observe, DCInvalidateRange, read. The flag is
    // relaxed, so only the fences in the cache calls order the data.
    // ThreadSanitizer does not model standalone fences and would report this
    // correct pattern as a race, so it is skipped there.
#ifndef PETARI_TSAN
    for (int round = 0; round < 200; ++round) {
        static u32 shared[256];
        gPublished.store(false, std::memory_order_relaxed);
        std::thread producer([round] {
            for (u32 i = 0; i < 256; ++i) {
                shared[i] = i * 3 + round;
            }
            DCFlushRange(shared, sizeof(shared));
            gPublished.store(true, std::memory_order_relaxed);
        });
        while (!gPublished.load(std::memory_order_relaxed)) {
        }
        DCInvalidateRange(shared, sizeof(shared));
        bool ok = true;
        for (u32 i = 0; i < 256; ++i) {
            ok = ok && shared[i] == i * 3 + static_cast<u32>(round);
        }
        producer.join();
        check(ok, "flush/invalidate pairing makes producer data visible");
    }
#endif

    alignas(32) static u8 block[128];
    std::memset(block, 0xFF, sizeof(block));
    DCZeroRange(block + 40, 10);  // inside block 1 (32..63)
    check(block[31] == 0xFF && block[32] == 0 && block[63] == 0 && block[64] == 0xFF, "DCZeroRange zeroes whole 32-byte blocks");
    std::memset(block, 0xFF, sizeof(block));
    DCZeroRange(block + 60, 8);  // straddles blocks 1 and 2
    check(block[31] == 0xFF && block[32] == 0 && block[95] == 0 && block[96] == 0xFF, "DCZeroRange covers straddled blocks");
    DCStoreRange(block, 32);
    DCFlushRangeNoSync(block, 32);
    DCStoreRangeNoSync(block, 32);
    check(block[0] == 0xFF, "flush and store leave data unchanged");

    static u8 code[64];
    ICInvalidateRange(code, sizeof(code));
    ICFlashInvalidate();

    alignas(32) static u8 lockedCache[LC_MAX_DMA_BYTES * 2 + 64];
    alignas(32) static u8 mainMemory[sizeof(lockedCache)];
    for (std::size_t i = 0; i < sizeof(lockedCache); ++i) {
        lockedCache[i] = static_cast<u8>(i * 7);
    }
    check(aborts([] { LCStoreData(mainMemory, lockedCache, 32); }), "locked cache DMA needs LCEnable");
    LCEnable();
    std::memset(mainMemory, 0, sizeof(mainMemory));
    check(LCStoreData(mainMemory, lockedCache, 0x2000 + 20) == 3, "LCStoreData reports its DMA transactions");
    LCQueueWait(0);
    check(std::memcmp(mainMemory, lockedCache, 0x2000 + 32) == 0 && mainMemory[0x2000 + 32] == 0,
          "LCStoreData copies whole 32-byte blocks");
    check(LCStoreData(mainMemory, lockedCache, 0x800) == 1, "one transaction for up to 4 KiB");
    LCDisable();
}

void testMisuse() {
    check(aborts([] {
              std::thread host([] {
                  OSThreadQueue q;
                  OSInitThreadQueue(&q);
                  OSSleepThread(&q);
              });
              host.join();
          }),
          "blocking from a non-OS host thread aborts");
    check(aborts([] {
              OSDisableScheduler();
              OSThreadQueue q;
              OSInitThreadQueue(&q);
              OSSleepThread(&q);
          }),
          "blocking with the scheduler disabled aborts");
    check(aborts([] { OSPanic("file.cpp", 12, "boom %d", 3); }), "OSPanic aborts");
}

// petari/platform/os_host.hpp: the main thread does host work (as the app's
// frame seam does) while lower-priority OS threads run, then takes the CPU
// back.
std::atomic<int> gHostBlockSpins{0};
std::atomic<bool> gHostBlockStop{false};
void* hostBlockSpinner(void*) {
    while (!gHostBlockStop) {
        gHostBlockSpins++;
        OSYieldThread();
    }
    return nullptr;
}

std::atomic<bool> gWokenDuringHostWork{false};
void* hostBlockWaiter(void* queue) {
    OSReceiveMessage(static_cast<OSMessageQueue*>(queue), nullptr, OS_MESSAGE_BLOCK);
    gWokenDuringHostWork = true;
    return nullptr;
}

// A thread that was dispatched (given the baton) but whose host thread has
// not woken yet has executed nothing, so a higher-priority thread readied
// before it wakes must run first, as on the Wii. Deterministic: an interrupt
// (host thread holding the interrupt lock) readies the low thread, which is
// dispatched to the idle CPU but cannot run yet, then the high one.
std::atomic<int> gDispatchOrder{0};
std::atomic<int> gLowRanAt{0}, gHighRanAt{0};
void* dispatchLow(void* queue) {
    OSReceiveMessage(static_cast<OSMessageQueue*>(queue), nullptr, OS_MESSAGE_BLOCK);
    gLowRanAt = ++gDispatchOrder;
    return nullptr;
}
void* dispatchHigh(void* queue) {
    OSReceiveMessage(static_cast<OSMessageQueue*>(queue), nullptr, OS_MESSAGE_BLOCK);
    gHighRanAt = ++gDispatchOrder;
    return nullptr;
}

void testDispatchBeforeWake() {
    static OSMessageQueue lowQueue, highQueue;
    static OSMessage lowSlot, highSlot;
    OSInitMessageQueue(&lowQueue, &lowSlot, 1);
    OSInitMessageQueue(&highQueue, &highSlot, 1);
    static OSThread low, high;
    alignas(32) static u8 lowStack[0x4000], highStack[0x4000];
    for (int round = 0; round < 20; ++round) {
        gDispatchOrder = 0;
        gLowRanAt = gHighRanAt = 0;
        OSCreateThread(&low, dispatchLow, &lowQueue, lowStack + sizeof(lowStack), sizeof(lowStack), 20, 0);
        OSCreateThread(&high, dispatchHigh, &highQueue, highStack + sizeof(highStack), sizeof(highStack), 2, 0);
        OSResumeThread(&low);
        OSResumeThread(&high);  // both run and block on their queues
        petari_os_begin_host_blocking();
        // The CPU goes to the low thread, which runs and blocks on its queue;
        // under host load that takes a while. Wait until the CPU is idle, or
        // the "interrupt" below would find the low thread still running.
        for (int i = 0; i < 3000 && !(OSGetCurrentThread() == nullptr && low.state == OS_THREAD_STATE_WAITING); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(OSGetCurrentThread() == nullptr, "the CPU is idle with both threads blocked");
        static OSThread* afterLow;
        static OSThread* afterHigh;
        std::thread interrupt([] {
            BOOL enabled = OSDisableInterrupts();
            OSSendMessage(&lowQueue, nullptr, OS_MESSAGE_NOBLOCK);   // dispatches low to the idle CPU
            afterLow = OSGetCurrentThread();
            OSSendMessage(&highQueue, nullptr, OS_MESSAGE_NOBLOCK);  // before low's host thread can run
            afterHigh = OSGetCurrentThread();                        // still under the interrupt lock
            OSRestoreInterrupts(enabled);
        });
        interrupt.join();
        check(afterLow == &low, "the idle CPU is dispatched to the first ready thread");
        // The dispatch decision, not just the order game code observes: the
        // high thread must get the baton now, not after the low thread's host
        // thread wakes up only to be preempted (5-35 ms in the app).
        check(afterHigh == &high, "the high-priority thread takes the baton from a dispatched thread that has not woken");
        for (int i = 0; i < 2000 && gDispatchOrder.load() < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        petari_os_end_host_blocking();
        OSJoinThread(&low, nullptr);
        OSJoinThread(&high, nullptr);
        check(gHighRanAt.load() == 1 && gLowRanAt.load() == 2, "a dispatched thread that has not woken yet yields to a higher-priority one");
    }
}

// Host QoS inheritance: while a higher-priority OS thread waits on a running
// holder, the holder has a QoS override; it is detached at the handoff and
// ended after the interrupt mutex is released. Exact lifecycle counts, no
// timing assertions.
namespace OSI = PetariNative::Platform::OS;
OSI::HolderOverrideStats overrideStats() {
    BOOL enabled = OSDisableInterrupts();
    const OSI::HolderOverrideStats st = OSI::holderOverrideStats();
    OSRestoreInterrupts(enabled);
    return st;
}
OSMessageQueue gQosQueue;
OSMessage gQosSlot;
OSThread gQosWaiter;
alignas(32) u8 gQosStack[0x4000];
std::atomic<bool> gQosWaiterRan{false};
void* qosWaiter(void*) {
    OSReceiveMessage(&gQosQueue, nullptr, OS_MESSAGE_BLOCK);
    gQosWaiterRan = true;
    return nullptr;
}
__attribute__((noinline)) void spinHostCode(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    volatile unsigned n = 0;
    while (std::chrono::steady_clock::now() < end) {
        n = n + 1;
    }
}

// One episode: a priority-2 thread readied by an interrupt while this
// (priority-16) thread spins in host code holding the baton. Returns the
// stats seen inside the interrupt, just after the send. The caller then hands
// over and joins the waiter.
OSI::HolderOverrideStats qosEpisode() {
    OSInitMessageQueue(&gQosQueue, &gQosSlot, 1);
    gQosWaiterRan = false;
    OSCreateThread(&gQosWaiter, qosWaiter, nullptr, gQosStack + sizeof(gQosStack), sizeof(gQosStack), 2, 0);
    OSResumeThread(&gQosWaiter);  // runs and blocks on the queue
    static OSI::HolderOverrideStats during;
    std::thread interrupt([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        BOOL enabled = OSDisableInterrupts();
        OSSendMessage(&gQosQueue, nullptr, OS_MESSAGE_NOBLOCK);
        during = OSI::holderOverrideStats();  // still under the interrupt lock
        OSRestoreInterrupts(enabled);
    });
    spinHostCode(15);  // holds the baton: no OS call
    interrupt.join();
    return during;
}

void testHolderQosOverride() {
    OSThread* self = OSGetCurrentThread();
    const OSI::HolderOverrideStats before = overrideStats();
    const OSI::HolderOverrideStats during = qosEpisode();
    check(during.active && during.target == self && during.started == before.started + 1,
          "a running holder gets a QoS override while a higher-priority thread waits on it");
    OSYieldThread();  // the handoff (an interrupt-state change)
    OSJoinThread(&gQosWaiter, nullptr);
    const OSI::HolderOverrideStats after = overrideStats();
    check(!after.active && after.detached == after.started, "detached at the handoff");
    check(after.ended == after.started && after.deferred == 0, "and ended once the interrupt mutex was released");

    // Handing over by petari_os_begin_host_blocking also detaches and ends it.
    const OSI::HolderOverrideStats duringHost = qosEpisode();
    check(duringHost.active && duringHost.target == self, "override active before a host-blocking handoff");
    petari_os_begin_host_blocking();
    for (int i = 0; i < 2000 && !gQosWaiterRan.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    petari_os_end_host_blocking();
    OSJoinThread(&gQosWaiter, nullptr);
    const OSI::HolderOverrideStats afterHost = overrideStats();
    check(gQosWaiterRan.load() && !afterHost.active && afterHost.detached == afterHost.started && afterHost.ended == afterHost.started &&
              afterHost.deferred == 0,
          "host-blocking handoff: detached and ended");

    // The dependency ends without a handoff: the waiter is suspended while the
    // holder still runs. The override is detached at once (still under the
    // interrupt lock) and ended when that lock is released.
    {
        static OSI::HolderOverrideStats withAWaiter, afterSuspend;
        OSInitMessageQueue(&gQosQueue, &gQosSlot, 1);
        gQosWaiterRan = false;
        OSCreateThread(&gQosWaiter, qosWaiter, nullptr, gQosStack + sizeof(gQosStack), sizeof(gQosStack), 2, 0);
        OSResumeThread(&gQosWaiter);  // blocks on the queue
        std::thread interrupt([] {
            BOOL enabled = OSDisableInterrupts();
            OSSendMessage(&gQosQueue, nullptr, OS_MESSAGE_NOBLOCK);
            withAWaiter = OSI::holderOverrideStats();
            OSSuspendThread(&gQosWaiter);  // no strictly higher thread waits now
            afterSuspend = OSI::holderOverrideStats();
            OSRestoreInterrupts(enabled);
        });
        interrupt.join();  // this thread keeps the baton: no OS call since
        check(withAWaiter.active && withAWaiter.target == self, "override while the waiter is ready");
        check(!afterSuspend.active && afterSuspend.detached == withAWaiter.detached + 1,
              "suspending the waiter detaches the override without a handoff");
        const OSI::HolderOverrideStats released = overrideStats();
        check(released.ended == released.started && released.deferred == 0, "and it is ended once the lock is released");
        check(!gQosWaiterRan.load(), "the suspended waiter did not run");
        OSResumeThread(&gQosWaiter);  // higher priority: runs now
        OSJoinThread(&gQosWaiter, nullptr);
    }

    // The dispatch-before-wake path hands the baton over without an override.
    const std::uint64_t startedBefore = overrideStats().started;
    testDispatchBeforeWake();
    check(overrideStats().started == startedBefore, "a dispatched thread that has not woken gets no override");
}

void testHostBlocking() {
    static OSThread spinner;
    alignas(32) static u8 stack[0x4000];
    gHostBlockStop = false;
    OSCreateThread(&spinner, hostBlockSpinner, nullptr, stack + sizeof(stack), sizeof(stack), 24, 0);
    OSResumeThread(&spinner);
    OSThread* self = OSGetCurrentThread();
    gHostBlockSpins = 0;

    petari_os_begin_host_blocking();
    for (int i = 0; i < 3000 && gHostBlockSpins.load() == 0; ++i) {  // e.g. SDL_WaitEvent
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const int during = gHostBlockSpins.load();
    petari_os_end_host_blocking();
    check(during > 0, "ready OS threads run while the main thread does host work");
    check(OSGetCurrentThread() == self && self->state == OS_THREAD_STATE_RUNNING, "end_host_blocking returns holding the CPU");
    const int after = gHostBlockSpins.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(gHostBlockSpins.load() == after, "the lower-priority thread stops once the CPU is back");

    // An interrupt wakes a higher-priority thread during host work: it runs
    // then, not when the main thread returns.
    static OSMessageQueue queue;
    static OSMessage slot;
    OSInitMessageQueue(&queue, &slot, 1);
    static OSThread waiter;
    alignas(32) static u8 waiterStack[0x4000];
    gWokenDuringHostWork = false;
    OSCreateThread(&waiter, hostBlockWaiter, &queue, waiterStack + sizeof(waiterStack), sizeof(waiterStack), 10, 0);
    OSResumeThread(&waiter);  // runs and blocks on the queue
    petari_os_begin_host_blocking();
    std::thread interrupt([] { OSSendMessage(&queue, nullptr, OS_MESSAGE_NOBLOCK); });
    interrupt.join();
    bool ran = false;
    for (int i = 0; i < 2000 && !(ran = gWokenDuringHostWork.load()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    petari_os_end_host_blocking();
    check(ran, "a thread woken by an interrupt runs during host work");
    OSJoinThread(&waiter, nullptr);

    gHostBlockStop = true;
    OSJoinThread(&spinner, nullptr);

    check(aborts([] {
              petari_os_begin_host_blocking();
              OSThreadQueue q;
              OSInitThreadQueue(&q);
              OSSleepThread(&q);
          }),
          "blocking OS calls during host work abort");
    check(aborts([] {
              petari_os_begin_host_blocking();
              petari_os_begin_host_blocking();
          }),
          "nested host blocking aborts");
    check(aborts([] {
              OSDisableInterrupts();
              petari_os_begin_host_blocking();
          }),
          "host blocking with interrupts disabled aborts");
    check(aborts([] { petari_os_end_host_blocking(); }), "end without begin aborts");
}

// petari_os_try_begin_host_blocking: for waits that may happen anywhere (the
// renderer's FIFO lock). It releases the CPU only where begin_host_blocking
// would be legal, and otherwise leaves the caller untouched.
void testTryHostBlocking() {
    static OSThread spinner;
    alignas(32) static u8 stack[0x4000];
    gHostBlockStop = false;
    gHostBlockSpins = 0;
    OSCreateThread(&spinner, hostBlockSpinner, nullptr, stack + sizeof(stack), sizeof(stack), 24, 0);
    OSResumeThread(&spinner);
    OSThread* self = OSGetCurrentThread();

    check(petari_os_try_begin_host_blocking() == 1, "an OS thread holding the CPU can release it");
    for (int i = 0; i < 3000 && gHostBlockSpins.load() == 0; ++i) {  // a contended host lock
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(gHostBlockSpins.load() > 0, "other OS threads run while it waits");
    check(petari_os_try_begin_host_blocking() == 0, "nested use does nothing");
    petari_os_end_host_blocking();
    check(OSGetCurrentThread() == self, "the CPU is back after end_host_blocking");

    BOOL enabled = OSDisableInterrupts();
    check(petari_os_try_begin_host_blocking() == 0, "not with interrupts disabled (a GX BP write)");
    OSRestoreInterrupts(enabled);
    OSDisableScheduler();
    check(petari_os_try_begin_host_blocking() == 0, "not with the scheduler disabled");
    OSEnableScheduler();
    int fromHost = -1;
    std::thread host([&] { fromHost = petari_os_try_begin_host_blocking(); });
    host.join();
    check(fromHost == 0, "not from a host thread (interrupt context)");
    check(OSGetCurrentThread() == self && self->state == OS_THREAD_STATE_RUNNING, "refusals leave the caller running");

    gHostBlockStop = true;
    OSJoinThread(&spinner, nullptr);
}

// The baton monitor: an OS thread holding the CPU that blocks in a host lock
// (what write_data_grow did behind the FIFO processor) is reported with its
// site; the same wait with the CPU released, or a short one, is not.
std::mutex gHostLock;

__attribute__((noinline)) void waitForHostLockHoldingCpu() {
    std::lock_guard<std::mutex> guard(gHostLock);
    asm volatile("" ::: "memory");
}

bool hasSite(const PetariNative::Platform::OS::BatonBlockStats& stats, const char* part) {
    for (const std::string& site : stats.sites) {
        if (site.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Holds gHostLock on a host thread for `ms` while `wait` runs on this OS thread.
template <class Wait>
void contend(int ms, Wait wait) {
    std::atomic<bool> held{false};
    std::thread host([&] {
        std::lock_guard<std::mutex> guard(gHostLock);
        held = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    });
    while (!held.load()) {
        std::this_thread::yield();
    }
    wait();
    host.join();
}

PetariNative::Platform::OS::BatonBlockStats settledBlockStats() {
    // The monitor finishes a stretch at its next poll (every 2 ms). Sleeping
    // with the CPU would itself be a host wait while holding it.
    petari_os_begin_host_blocking();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    petari_os_end_host_blocking();
    return PetariNative::Platform::OS::batonBlockStats();
}

void testBatonMonitor() {
    namespace OSI = PetariNative::Platform::OS;
    OSI::setBatonBlockThresholdMs(20.0);
    const auto before = settledBlockStats();

    contend(250, [] { waitForHostLockHoldingCpu(); });
    const auto blocked = settledBlockStats();
    check(blocked.stretches >= before.stretches + 1, "a host wait while holding the CPU is counted");
    check(blocked.maxMs >= 100.0, "with its length");
    check(hasSite(blocked, "waitForHostLockHoldingCpu"), "and its site in the executable");
    check(hasSite(blocked, "[waiting in __psynch_mutexwait"), "and the host primitive it waits in");

    const auto beforeReleased = settledBlockStats();
    contend(250, [] {
        petari_os_begin_host_blocking();
        waitForHostLockHoldingCpu();
        petari_os_end_host_blocking();
    });
    check(settledBlockStats().stretches == beforeReleased.stretches, "the same wait with the CPU released is not counted");

    // Below the threshold (1 s here, so host load cannot stretch a 5 ms wait past it).
    OSI::setBatonBlockThresholdMs(1000.0);
    const auto beforeShort = settledBlockStats();
    contend(5, [] { waitForHostLockHoldingCpu(); });
    check(settledBlockStats().stretches == beforeShort.stretches, "a wait shorter than the threshold is not counted");
    OSI::setBatonBlockThresholdMs(20.0);
}

// Tests fork (aborts()) while the monitor may be writing through stdio,
// holding that FILE's lock; a child that then uses the FILE must not hang.
// The monitor writes a line to one file at every poll (a test mode, so the
// test does not depend on how many stretches it happens to detect), and each
// child writes to the same file right after the fork.
void testMonitorForkSafety() {
    namespace OSI = PetariNative::Platform::OS;
    std::FILE* chatter = std::tmpfile();
    OSI::setBatonMonitorChatter(chatter);
    const std::uint64_t pollsBefore = OSI::batonMonitorPolls();
    int hung = 0;
    for (int i = 0; i < 150 && hung == 0; ++i) {
        std::fflush(nullptr);
        const pid_t pid = ::fork();
        if (pid == 0) {
            std::fprintf(chatter, "child %d\n", i);
            std::fflush(chatter);
            ::_exit(0);
        }
        int status = 0;
        pid_t done = 0;
        for (int waited = 0; waited < 5000 && (done = ::waitpid(pid, &status, WNOHANG)) == 0; ++waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (done == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            ++hung;
        }
        if (i % 10 == 0) {
            // Let the monitor poll between batches, whatever the host load.
            for (int w = 0; w < 1000 && OSI::batonMonitorPolls() == pollsBefore; ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    const std::uint64_t polls = OSI::batonMonitorPolls() - pollsBefore;
    OSI::setBatonMonitorChatter(nullptr);
    std::fclose(chatter);
    std::printf("monitor fork safety: 150 forks while the monitor polled %llu times\n", static_cast<unsigned long long>(polls));
    check(polls > 0, "the monitor was writing while the test forked");
    check(hung == 0, "children forked while the monitor writes through stdio never hang");
}

}  // namespace

// ---- Forced preemption of busy-waits (observatory Good Egg hang) ----
// GameScene::init waits for scenario wave data with an empty loop. The ARAM
// stream thread that finishes the load is made ready by a DVD interrupt while
// the loop's lower-priority thread holds the CPU. With no OS call in the loop
// the pending preemption was never delivered: the load, and the game, hung.
// The functions below are at file scope with game-style names, as the
// decompiled game's are (the monitor preempts only inside game code).
std::atomic<bool> gWaveDataLoaded{false};
std::atomic<bool> gBusyWaitDone{false};
std::atomic<bool> gSpinning{false};  // the loop runs (its host thread is executing it)
OSMessageQueue gLoaderQueue;
OSMessage gLoaderSlot;

void* GameStyleBusyWait(void*) {
    gSpinning = true;
    while (!gWaveDataLoaded.load(std::memory_order_relaxed)) {
    }
    gBusyWaitDone = true;
    return nullptr;
}

void* GameStyleLoader(void*) {
    OSMessage msg;
    OSReceiveMessage(&gLoaderQueue, &msg, OS_MESSAGE_BLOCK);  // DVD completion
    gWaveDataLoaded = true;
    return nullptr;
}

namespace PetariNative::PreemptionTest {
std::atomic<bool> gRelease{false};
// Host code spinning: never preempted (it could hold host locks).
void* HostSpin(void*) {
    gSpinning = true;
    while (!gRelease.load(std::memory_order_relaxed)) {
    }
    return nullptr;
}
}  // namespace PetariNative::PreemptionTest

// Waits (with the CPU released) until `done`, or fails after `ms`: a hung
// thread cannot be joined.
template <class Done>
bool waitReleased(Done done, int ms) {
    for (int i = 0; i < ms && !done(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

void testForcedPreemption() {
    namespace OSI = PetariNative::Platform::OS;
    const auto before = OSI::batonBlockStats();
    OSInitMessageQueue(&gLoaderQueue, &gLoaderSlot, 1);
    static OSThread loader, poller;
    alignas(32) static u8 loaderStack[0x4000], pollerStack[0x4000];
    OSCreateThread(&loader, GameStyleLoader, nullptr, loaderStack + sizeof(loaderStack), sizeof(loaderStack), 8, 0);
    OSResumeThread(&loader);  // runs at once and waits for its "DVD" message
    OSCreateThread(&poller, GameStyleBusyWait, nullptr, pollerStack + sizeof(pollerStack), sizeof(pollerStack), 17, 0);
    OSResumeThread(&poller);
    gSpinning = false;
    petari_os_begin_host_blocking();  // the poller (priority 17) takes the CPU and spins
    check(waitReleased([] { return gSpinning.load(); }, 3000), "the poller is spinning");
    std::thread dvdInterrupt([] { OSSendMessage(&gLoaderQueue, nullptr, OS_MESSAGE_NOBLOCK); });
    dvdInterrupt.join();
    if (!waitReleased([] { return gBusyWaitDone.load(); }, 3000)) {
        std::fprintf(stderr, "FAIL: a busy-wait holding the CPU was never preempted for the ready loader (hang)\n");
        std::fflush(stderr);
        ::_exit(1);
    }
    petari_os_end_host_blocking();
    OSJoinThread(&loader, nullptr);
    OSJoinThread(&poller, nullptr);
    const auto after = OSI::batonBlockStats();
    check(after.forcedPreemptions == before.forcedPreemptions + 1, "the busy-wait was preempted once, at a safe point");

    // The same spin in host code is left alone (and reported).
    static OSThread spinner;
    alignas(32) static u8 spinnerStack[0x4000];
    OSInitMessageQueue(&gLoaderQueue, &gLoaderSlot, 1);
    gWaveDataLoaded = false;
    OSCreateThread(&loader, GameStyleLoader, nullptr, loaderStack + sizeof(loaderStack), sizeof(loaderStack), 8, 0);
    OSResumeThread(&loader);
    OSCreateThread(&spinner, PetariNative::PreemptionTest::HostSpin, nullptr, spinnerStack + sizeof(spinnerStack),
                   sizeof(spinnerStack), 17, 0);
    gSpinning = false;
    OSResumeThread(&spinner);
    petari_os_begin_host_blocking();
    check(waitReleased([] { return gSpinning.load(); }, 3000), "the host spinner is spinning");
    std::thread interrupt([] { OSSendMessage(&gLoaderQueue, nullptr, OS_MESSAGE_NOBLOCK); });
    interrupt.join();
    // Until the monitor has looked at the spin at least once (a refusal).
    const std::uint64_t unsafeBefore = after.unsafePreemptions;
    waitReleased([&] { return OSI::batonBlockStats().unsafePreemptions > unsafeBefore; }, 3000);
    const bool loaderRanEarly = gWaveDataLoaded.load();
    const auto refused = OSI::batonBlockStats();
    PetariNative::PreemptionTest::gRelease = true;  // the spinner returns; its exit schedules the loader
    const bool finished = waitReleased([] { return gWaveDataLoaded.load(); }, 3000);
    petari_os_end_host_blocking();
    check(finished, "the loader runs once the host spin ends");
    OSJoinThread(&loader, nullptr);
    OSJoinThread(&spinner, nullptr);
    check(!loaderRanEarly && refused.forcedPreemptions == after.forcedPreemptions, "host code is never preempted");
    check(refused.unsafePreemptions > after.unsafePreemptions, "and the refusal is counted");
}

// ---- Handoff latency to a higher-priority thread (HeavensDoor choppy music) ----
// JAudio2's audio thread (priority 2) is made ready by the DSP/AI interrupts
// about 8 times per 17.5 ms audio block. While a lower-priority game thread
// runs collision code without OS calls, each handoff waits for the baton
// monitor's forced preemption; at 4 ms each the DSP missed about half its
// blocks. On the Wii the interrupt preempts at once.
constexpr int kHandoffs = 40;
std::atomic<bool> gCollisionDone{false};
std::atomic<bool> gCollisionSpinning{false};
std::atomic<int> gHandoffsReceived{0};
std::atomic<std::int64_t> gHandoffSentNs{0};
double gHandoffMs[kHandoffs];
OSMessageQueue gDspQueue;
OSMessage gDspSlot;

std::int64_t steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void* GameStyleCollisionCheck(void*) {
    volatile float strike = 0;
    gCollisionSpinning = true;
    while (!gCollisionDone.load(std::memory_order_relaxed)) {
        strike = strike + 1.0f;
    }
    return nullptr;
}

void* GameStyleAudioThread(void*) {
    for (int i = 0; i < kHandoffs; ++i) {
        OSMessage msg;
        OSReceiveMessage(&gDspQueue, &msg, OS_MESSAGE_BLOCK);
        gHandoffMs[i] = static_cast<double>(steadyNs() - gHandoffSentNs.load()) / 1e6;
        gHandoffsReceived.store(i + 1);
    }
    return nullptr;
}

void testPriorityHandoffLatency() {
    namespace OSI = PetariNative::Platform::OS;
    const auto before = OSI::batonBlockStats();
    OSInitMessageQueue(&gDspQueue, &gDspSlot, 1);
    static OSThread audio, game;
    alignas(32) static u8 audioStack[0x4000], gameStack[0x4000];
    OSCreateThread(&audio, GameStyleAudioThread, nullptr, audioStack + sizeof(audioStack), sizeof(audioStack), 2, 0);
    OSResumeThread(&audio);  // runs at once and waits for its first DSP message
    OSCreateThread(&game, GameStyleCollisionCheck, nullptr, gameStack + sizeof(gameStack), sizeof(gameStack), 16, 0);
    OSResumeThread(&game);
    petari_os_begin_host_blocking();  // the game thread takes the CPU and computes
    check(waitReleased([] { return gCollisionSpinning.load(); }, 3000), "the game thread computes without OS calls");
    std::thread dsp([] {
        for (int i = 0; i < kHandoffs; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));  // the game thread runs meanwhile
            gHandoffSentNs.store(steadyNs());
            OSSendMessage(&gDspQueue, nullptr, OS_MESSAGE_NOBLOCK);
            for (int w = 0; w < 3000 && gHandoffsReceived.load() <= i; ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    });
    dsp.join();
    const bool all = gHandoffsReceived.load() == kHandoffs;
    gCollisionDone = true;
    petari_os_end_host_blocking();
    OSJoinThread(&audio, nullptr);
    OSJoinThread(&game, nullptr);
    check(all, "every DSP message reached the audio thread");
    std::vector<double> ms(gHandoffMs, gHandoffMs + kHandoffs);
    std::sort(ms.begin(), ms.end());
    const double median = ms[kHandoffs / 2], p90 = ms[kHandoffs * 9 / 10];
    const auto after = OSI::batonBlockStats();
    std::printf("priority handoff: %d handoffs from a computing priority-16 thread to priority 2: median %.2f ms, 90th %.2f ms, "
                "longest %.2f ms; %llu forced preemptions\n",
                kHandoffs, median, p90, ms.back(), static_cast<unsigned long long>(after.forcedPreemptions - before.forcedPreemptions));
    check(after.forcedPreemptions == before.forcedPreemptions + kHandoffs, "each handoff was a forced preemption (no OS calls)");
    // 8 handoffs per 17.5 ms DSP block need well under 2 ms each (was over 4 ms).
    check(median <= 1.5, "a higher-priority thread made ready by an interrupt runs within 1.5 ms (median)");
}

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    __OSThreadInit();
    testInterrupts();
    testMisuse();
    testPriorityPreemption();
    testUniprocessor();
    testMessages();
    testMutex();
    testLifecycle();
    testScheduler();
    testAlarmsAndSleep();
    testTime();
    testDvdOnOsThreads();
    testCache();
    testHostBlocking();
    testTryHostBlocking();
    testBatonMonitor();
    testMonitorForkSafety();
    testForcedPreemption();
    testPriorityHandoffLatency();
    testDispatchBeforeWake();
    testHolderQosOverride();
    OSReport("platform OS tests passed (%d checks)\n", checks);
    return 0;
}

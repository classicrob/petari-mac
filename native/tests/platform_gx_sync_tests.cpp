// Tests for GX synchronisation with a fake command processor standing in for
// the renderer: it consumes a synthetic FIFO stream, honours the breakpoint,
// and reports tokens, draw done, breakpoint hits, and progress in stream order.

#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXManage.h>
#include <revolution/gx/GXPerf.h>
#include <revolution/os.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "petari/platform/diagnostics.hpp"
#include "petari/platform/gx_sync.hpp"

extern "C" void __OSThreadInit(void);

namespace GXS = PetariNative::Platform::GXSync;

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

// ---- Fake renderer: stream of commands, processor thread ----

enum class Cmd { Draw, Token, DrawDone };
struct Command {
    Cmd kind;
    std::uint16_t token;
    std::uint64_t start;  // stream position
    std::uint32_t size;
};

class FakeRenderer {
public:
    void start() {
        mStop = false;
        mThread = std::thread([this] { run(); });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> g(mLock);
            mStop = true;
        }
        mChanged.notify_all();
        mThread.join();
    }
    // GX wrappers' writes (game side).
    std::uint64_t write(Cmd kind, std::uint16_t token = 0, std::uint32_t size = 32) {
        std::lock_guard<std::mutex> g(mLock);
        mStream.push_back({kind, token, mWritten, size});
        mWritten += size;
        mChanged.notify_all();
        return mWritten;
    }
    std::uint64_t writePosition() {
        std::lock_guard<std::mutex> g(mLock);
        return mWritten;
    }
    void setDelay(std::chrono::microseconds d) { mDelay = d; }
    void wakeForBreakpointChange() { mChanged.notify_all(); }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mLock);
        bool reportedHalt = false;
        while (true) {
            mChanged.wait_for(lock, std::chrono::milliseconds(2), [&] { return mStop || !mStream.empty(); });
            if (mStop) {
                return;
            }
            if (mStream.empty()) {
                continue;
            }
            const Command c = mStream.front();
            const std::uint64_t bp = GXS::breakpointPosition();
            if (c.start >= bp) {
                // Halted at the breakpoint (complete command boundary).
                if (!reportedHalt) {
                    reportedHalt = true;
                    lock.unlock();
                    GXS::reportProgress(c.start, mWritten);
                    GXS::reportBreakpointReached(c.start);
                    lock.lock();
                }
                // As the real processor (patch_aurora_sync.py worker_main): wait
                // for the breakpoint to move without holding the stream lock, so
                // game threads keep writing. (wait_for with the stream predicate
                // returns at once while commands are pending, without unlocking.)
                mChanged.wait_for(lock, std::chrono::milliseconds(2),
                                  [&] { return mStop || GXS::breakpointPosition() != bp; });
                continue;
            }
            reportedHalt = false;
            mStream.pop_front();
            const std::uint64_t written = mWritten;
            lock.unlock();
            if (mDelay.count()) {
                std::this_thread::sleep_for(mDelay);
            }
            const std::uint64_t end = c.start + c.size;
            if (c.kind == Cmd::Token) {
                GXS::reportToken(c.token, end);
            } else if (c.kind == Cmd::DrawDone) {
                GXS::reportDrawDone(end);
            }
            GXS::reportProgress(end, written);
            lock.lock();
        }
    }

    std::mutex mLock;
    std::condition_variable mChanged;
    std::deque<Command> mStream;
    std::uint64_t mWritten = 0;
    bool mStop = false;
    std::chrono::microseconds mDelay{0};
    std::thread mThread;
};

FakeRenderer gRenderer;

// Thin wrappers as root's GX layer provides them.
void GXSetDrawSyncTest(std::uint16_t token) {
    gRenderer.write(Cmd::Token, token, 8);
}
void GXSetDrawDoneTest() {
    gRenderer.write(Cmd::DrawDone, 0, 8);
}
void GXDrawDoneTest() {
    const std::uint64_t n = GXS::noteDrawDoneIssued();
    GXSetDrawDoneTest();
    GXS::waitDrawDone(n);
}
void* GXGetWritePointerTest() {
    return GXS::positionToPointer(gRenderer.writePosition());
}

// ---- Tests ----

std::mutex gLogLock;
std::vector<std::string> gLog;
std::atomic<bool> gCallbackInInterruptContext{true};

void drawSyncCallback(u16 token) {
    gCallbackInInterruptContext = gCallbackInInterruptContext && OSDisableInterrupts() == FALSE;
    std::lock_guard<std::mutex> g(gLogLock);
    gLog.push_back("token" + std::to_string(token));
}
void drawDoneCallback() {
    gCallbackInInterruptContext = gCallbackInInterruptContext && OSDisableInterrupts() == FALSE;
    std::lock_guard<std::mutex> g(gLogLock);
    gLog.push_back("done");
}
std::atomic<int> gBreakpointHits{0};
void breakpointCallback() {
    gBreakpointHits++;
}

void testOrderingAndDrawDone() {
    GXS::setDrawSyncCallback(drawSyncCallback);
    GXS::setDrawDoneCallback(drawDoneCallback);
    for (u16 t = 1; t <= 5; ++t) {
        gRenderer.write(Cmd::Draw, 0, 256);
        GXSetDrawSyncTest(t);
    }
    GXDrawDoneTest();
    check((gLog == std::vector<std::string>{"token1", "token2", "token3", "token4", "token5", "done"}),
          "tokens then draw done, in stream order, before GXDrawDone returns");
    check(gCallbackInInterruptContext, "GP interrupts run with interrupts disabled");
    check(GXS::lastToken() == 5, "GXReadDrawSync returns the last processed token");
    const GXS::GPStatus st = GXS::gpStatus();
    check(st.readIdle && st.commandIdle && !st.breakpoint, "GP idle after draw done");
}

std::atomic<int> gSpins{0};
std::atomic<bool> gStopSpin{false};
void* spinner(void*) {
    while (!gStopSpin) {
        gSpins++;
        OSYieldThread();
    }
    return nullptr;
}

void testWaitYieldsBaton() {
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, spinner, nullptr, stack + sizeof(stack), sizeof(stack), 24, 0);
    OSResumeThread(&thread);
    gRenderer.setDelay(std::chrono::microseconds(2000));
    gSpins = 0;
    for (int i = 0; i < 10; ++i) {
        gRenderer.write(Cmd::Draw, 0, 64);
    }
    GXDrawDoneTest();  // ~22 ms of "GPU" work
    gRenderer.setDelay(std::chrono::microseconds(0));
    check(gSpins.load() > 0, "a lower-priority game thread runs while GXDrawDone waits");
    gStopSpin = true;
    OSJoinThread(&thread, nullptr);
}

void testBreakpoint() {
    GXS::setBreakpointCallback(breakpointCallback);
    gLog.clear();
    // Frame 1 then a breakpoint where frame 2 begins, as DrawSyncManager places it.
    gRenderer.write(Cmd::Draw, 0, 128);
    GXSetDrawSyncTest(10);
    void* frame2 = GXGetWritePointerTest();
    check(reinterpret_cast<uintptr_t>(frame2) >= 0x80000000u, "FIFO pointers are above the token range");
    GXS::setBreakpoint(GXS::pointerToPosition(frame2));
    gRenderer.write(Cmd::Draw, 0, 128);
    GXSetDrawSyncTest(11);
    // Wait for the processor to halt.
    for (int i = 0; i < 2000 && gBreakpointHits.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(gBreakpointHits.load() == 1, "breakpoint callback when the GP reaches the breakpoint");
    GXS::GPStatus st = GXS::gpStatus();
    check(st.breakpoint && !st.readIdle, "GP status: halted at the breakpoint with commands pending");
    {
        std::lock_guard<std::mutex> g(gLogLock);
        check((gLog == std::vector<std::string>{"token10"}), "nothing past the breakpoint is processed");
    }
    u32 a, b, c, clocks1, clocks2;
    GXReadXfRasMetric(&a, &b, &c, &clocks1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    GXReadXfRasMetric(&a, &b, &c, &clocks2);
    check(clocks1 == clocks2 && a == 0 && b == 0 && c == 0, "halted GP makes no progress; unmeasured counters are 0");
    // Game threads keep writing while the processor is halted: the real FIFO
    // processor waits for a wake without holding the buffer lock (patch_aurora_sync.py).
    // A writer blocked here would hold the OS CPU that the thread moving the
    // breakpoint needs (head-check hang under load, 2026-10-01).
    std::atomic<bool> wrote{false};
    std::thread writer([&wrote] {
        gRenderer.write(Cmd::Draw, 0, 64);
        wrote = true;
    });
    for (int i = 0; i < 2000 && !wrote.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(wrote.load(), "a write while the processor is halted at the breakpoint does not block");
    writer.join();
    GXS::clearBreakpoint();
    gRenderer.wakeForBreakpointChange();
    GXDrawDoneTest();
    check(gLog.back() == "done" && gLog[1] == "token11", "processing resumes after the breakpoint is cleared");
    GXReadXfRasMetric(&a, &b, &c, &clocks2);
    check(clocks2 != clocks1, "progress is visible in the metric");
    check(!GXS::gpStatus().breakpoint, "breakpoint flag cleared");
}

void testNoDropsAndInterruptsDisabled() {
    gLog.clear();
    std::atomic<int> count{0};
    static std::atomic<int>* counter;
    counter = &count;
    GXS::setDrawSyncCallback([](u16) { (*counter)++; });
    // A game thread keeps interrupts disabled while the GP floods interrupts:
    // the processor must keep going (it never takes the OS lock).
    BOOL e = OSDisableInterrupts();
    for (u16 t = 1; t <= 10000; ++t) {
        GXSetDrawSyncTest(t);
    }
    const std::uint64_t end = gRenderer.writePosition();
    for (int i = 0; i < 5000 && GXS::processedPosition() < end; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(GXS::processedPosition() == end, "the processor is not blocked by a game thread with interrupts disabled");
    check(count.load() == 0, "no GP interrupt is delivered while interrupts are disabled");
    OSRestoreInterrupts(e);
    GXDrawDoneTest();
    check(count.load() == 10000, "10000 queued interrupts all delivered, none dropped");
    GXS::setDrawSyncCallback(drawSyncCallback);
}

void testMisuse() {
    check(aborts([] { GXS::pointerToPosition(reinterpret_cast<void*>(0x1234)); }), "a non-FIFO pointer is refused");
    check(aborts([] {
              GXS::reportToken(1, 100);
              GXS::reportToken(2, 50);
          }),
          "out-of-order reports abort");
    check(aborts([] {
              std::thread host([] {
                  OSDisableInterrupts();
                  GXS::waitDrawDone(1u << 30);
              });
              host.join();
          }),
          "GXDrawDone from interrupt context aborts");
}

// DrawSyncManager's protocol (Game/System/DrawSyncManager.cpp): write pointers
// and tokens through one message queue, breakpoint on the second pending frame.
OSMessageQueue gDsmQueue;
OSMessage gDsmSlots[20];
std::deque<void*> gDsmFifo;
std::atomic<int> gDsmTokens{0};
std::atomic<std::uint64_t> gDsmMaxLead{0};

void* drawSyncManagerThread(void*) {
    while (true) {
        OSMessage msg;
        OSReceiveMessage(&gDsmQueue, &msg, OS_MESSAGE_BLOCK);
        const uintptr_t value = reinterpret_cast<uintptr_t>(msg);
        if (value >= 0x80000000) {
            gDsmFifo.push_back(msg);
            if (gDsmFifo.size() == 2) {
                GXS::setBreakpoint(GXS::pointerToPosition(msg));
            }
        } else if (value < 0x10000) {
            gDsmFifo.pop_front();
            if (gDsmFifo.size() == 1) {
                GXS::clearBreakpoint();
            } else if (gDsmFifo.size() >= 2) {
                GXS::setBreakpoint(GXS::pointerToPosition(gDsmFifo[1]));
            }
            gRenderer.wakeForBreakpointChange();
        } else {
            return nullptr;
        }
    }
}

void dsmTokenCallback(u16 token) {
    gDsmTokens++;
    OSSendMessage(&gDsmQueue, reinterpret_cast<OSMessage>(static_cast<uintptr_t>(token)), OS_MESSAGE_NOBLOCK);
}

void testDrawSyncManagerProtocol() {
    OSInitMessageQueue(&gDsmQueue, gDsmSlots, 20);
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, drawSyncManagerThread, nullptr, stack + sizeof(stack), sizeof(stack), 15, 0);
    OSResumeThread(&thread);
    GXS::setDrawSyncCallback(dsmTokenCallback);
    gRenderer.setDelay(std::chrono::microseconds(300));
    for (u16 frame = 1; frame <= 30; ++frame) {
        for (int d = 0; d < 4; ++d) {
            gRenderer.write(Cmd::Draw, 0, 512);
        }
        // pushBreakPoint + GXSetDrawSync, as StarPointerPeekZ does.
        OSSendMessage(&gDsmQueue, GXGetWritePointerTest(), OS_MESSAGE_BLOCK);
        GXSetDrawSyncTest(frame);
        // The GP may never run past the second pending peek point.
        const std::uint64_t bp = GXS::breakpointPosition();
        if (bp != GXS::kNoBreakpoint) {
            check(GXS::processedPosition() <= bp, "GP stays behind the DrawSyncManager breakpoint");
        }
    }
    GXDrawDoneTest();
    gRenderer.setDelay(std::chrono::microseconds(0));
    for (int i = 0; i < 2000 && gDsmTokens.load() < 30; ++i) {
        OSSleepTicks(OSMillisecondsToTicks(1));
    }
    check(gDsmTokens.load() == 30, "every frame's token reached DrawSyncManager");
    OSSendMessage(&gDsmQueue, reinterpret_cast<OSMessage>(0x10000), OS_MESSAGE_BLOCK);
    OSJoinThread(&thread, nullptr);
    check(gDsmFifo.size() <= 1, "DrawSyncManager's FIFO drained");
    GXS::setDrawSyncCallback(drawSyncCallback);
}

// Ticket lifetime at the platform API.
std::mutex gLifetimeLock;
std::vector<std::uint64_t> gLifetimeTickets;  // written on the processor thread
void lifetimeHook(std::uint64_t ticket, std::uint16_t, std::uint64_t, void*) {
    std::lock_guard<std::mutex> g(gLifetimeLock);
    gLifetimeTickets.push_back(ticket);
}
std::vector<std::uint64_t> lifetimeTickets(std::size_t atLeast) {
    for (int i = 0; i < 2000; ++i) {
        {
            std::lock_guard<std::mutex> g(gLifetimeLock);
            if (gLifetimeTickets.size() >= atLeast) {
                return gLifetimeTickets;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::lock_guard<std::mutex> g(gLifetimeLock);
    return gLifetimeTickets;
}

void testSnapshotLifetime() {
    // Completion from inside the hook (a capture that is ready at once).
    GXS::setSnapshotHook([](std::uint64_t ticket, std::uint16_t, std::uint64_t, void*) { GXS::snapshotReady(ticket); }, nullptr);
    gLog.clear();
    GXSetDrawSyncTest(40);
    GXDrawDoneTest();
    check((gLog == std::vector<std::string>{"token40", "done"}), "a ticket completed inside the hook is delivered");

    // Shutdown discards held events; a late completion is ignored, and
    // tickets are never reused afterwards.
    GXS::setSnapshotHook(lifetimeHook, nullptr);
    GXSetDrawSyncTest(41);
    const std::vector<std::uint64_t> first = lifetimeTickets(1);
    check(first.size() == 1, "one ticket for the token");
    const std::uint64_t held = first[0];
    // Forked children must not inherit a lock held by the processor thread.
    gRenderer.stop();
    check(aborts([] { GXS::snapshotReady(GXS::deliveredTicket() + 1000); }), "completing a ticket never issued aborts");
    check(aborts([held] {
              GXS::snapshotReady(held);
              GXS::snapshotReady(held);
          }),
          "completing a queued ticket twice aborts");
    GXS::shutdown();
    GXS::snapshotReady(held);  // after shutdown: ignored
    gRenderer.start();
    GXS::setDrawSyncCallback(drawSyncCallback);
    GXS::setDrawDoneCallback(drawDoneCallback);
    GXS::setSnapshotHook(lifetimeHook, nullptr);
    gLog.clear();
    GXSetDrawSyncTest(42);
    const std::vector<std::uint64_t> second = lifetimeTickets(2);
    check(second.size() == 2 && second[1] > held, "tickets are not reused after shutdown");
    GXS::snapshotReady(second[1]);
    GXDrawDoneTest();
    check((gLog == std::vector<std::string>{"token42", "done"}), "delivery works after a shutdown with held tickets");
    GXS::setSnapshotHook(nullptr, nullptr);
}

// Draw done does not wait for a token-time capture (a GPU round trip); the
// frame boundary does (GameSystem::frameLoop): the next frame's game code runs
// only after every token callback of the previous frame, as on the console,
// where they all precede GXDrawDone. Other game threads run meanwhile.
std::atomic<int> gBarrierSpins{0};
std::atomic<bool> gStopBarrierSpin{false};
void* barrierSpinner(void*) {
    while (!gStopBarrierSpin) {
        gBarrierSpins++;
        OSYieldThread();
    }
    return nullptr;
}

void testTokensBeforeNextFrame() {
    GXS::setDrawSyncCallback(drawSyncCallback);
    GXS::setDrawDoneCallback(drawDoneCallback);
    {
        std::lock_guard<std::mutex> g(gLifetimeLock);
        gLifetimeTickets.clear();
    }
    GXS::setSnapshotHook(lifetimeHook, nullptr);
    {
        std::lock_guard<std::mutex> g(gLogLock);
        gLog.clear();
    }
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, barrierSpinner, nullptr, stack + sizeof(stack), sizeof(stack), 24, 0);
    OSResumeThread(&thread);

    // Frame N: two peek tokens (star pointer, lens flare), then the frame's draw done.
    gRenderer.write(Cmd::Draw, 0, 256);
    GXSetDrawSyncTest(60);
    gRenderer.write(Cmd::Draw, 0, 256);
    GXSetDrawSyncTest(61);
    GXDrawDoneTest();
    const std::vector<std::uint64_t> tickets = lifetimeTickets(2);
    check(tickets.size() == 2, "a ticket per token");
    {
        std::lock_guard<std::mutex> g(gLogLock);
        check((gLog == std::vector<std::string>{"done"}), "GXDrawDone returns while the frame's captures are in flight");
    }
    // The captures complete later, out of order, from a renderer thread.
    std::thread renderer([&tickets] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        GXS::snapshotReady(tickets[1]);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        GXS::snapshotReady(tickets[0]);
    });
    gBarrierSpins = 0;
    GXS::waitTokensDelivered();  // frame N+1 starts after this
    {
        std::lock_guard<std::mutex> g(gLogLock);
        gLog.push_back("next frame");
    }
    renderer.join();
    {
        std::lock_guard<std::mutex> g(gLogLock);
        check((gLog == std::vector<std::string>{"done", "token60", "token61", "next frame"}),
              "every token callback of a frame precedes the next frame's game code, in token order");
    }
    check(gBarrierSpins.load() > 0, "other game threads run during the frame-boundary wait");
    const auto start = std::chrono::steady_clock::now();
    GXS::waitTokensDelivered();
    check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5), "nothing outstanding: no wait");
    gStopBarrierSpin = true;
    OSJoinThread(&thread, nullptr);
    GXS::setSnapshotHook(nullptr, nullptr);
}

// ---- Hang check (the game's GX abort alarm, MainLoopFramework.cpp) ----
// Driven directly, without the fake renderer (after a shutdown resets the
// positions): the test plays the processor, and a host thread plays the alarm
// with a simulated clock advancing 0.5 s per expiry, as the game re-arms it.

constexpr std::uint64_t kHalfSecond = 500000000;

struct AlarmResult {
    int checks = 0;
    int aborts = 0;
    int progress = 0;
    GXS::WaitState last = GXS::WaitState::Progress;
    bool sawBusy = false;
};

// Runs alarm expiries on a host thread until `stop`, or until the first abort
// verdict when `recover` is set (then runs the handler's recovery: breakpoint
// cleared, frame aborted, a new draw done written).
AlarmResult runAlarm(std::atomic<bool>& stop, bool recover, std::atomic<std::uint64_t>* recoveryDrawDone) {
    AlarmResult result;
    GXS::WaitCheck wait;
    std::uint64_t now = 1000000000;
    GXS::checkWait(wait, now);  // at OSSetAlarm
    while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        now += kHalfSecond;
        BOOL enabled = OSDisableInterrupts();  // alarm handlers run with interrupts disabled
        const GXS::WaitVerdict verdict = GXS::checkWait(wait, now);
        ++result.checks;
        result.last = verdict.state;
        result.sawBusy = result.sawBusy || verdict.state == GXS::WaitState::Busy;
        result.progress += verdict.state == GXS::WaitState::Progress;
        if (verdict.abort) {
            ++result.aborts;
            if (recover) {
                GXS::clearBreakpoint();
                GXS::abortFrame();
                recoveryDrawDone->store(GXS::noteDrawDoneIssued());
                OSRestoreInterrupts(enabled);
                return result;
            }
        }
        OSRestoreInterrupts(enabled);
    }
    return result;
}

// Observatory run 6: GXDrawDone waited ~11 s while one command batch compiled
// about 30 pipelines back to back. The processed position cannot move inside
// a batch, and between two compiles no pipeline wait is visible, so the Wii's
// check aborted a good frame. A busy processor is never a reason to abort,
// even with every game thread asleep.
void testCompileBurstIsNotAHang() {
    const std::uint64_t n = GXS::noteDrawDoneIssued();
    GXS::processLimit(0, 4096);  // the processor starts a batch ending in the draw done
    std::atomic<bool> stop{false};
    AlarmResult alarm;
    std::thread alarmThread([&] { alarm = runAlarm(stop, false, nullptr); });
    std::thread processor([&] {
        // ~40 expiries (20 simulated seconds) inside the batch, then its end.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        GXS::reportDrawDone(4096);
        GXS::reportProgress(4096, 4096);
    });
    GXS::waitDrawDone(n);  // sleeps: no OS thread holds or wants the CPU meanwhile
    stop = true;
    processor.join();
    alarmThread.join();
    check(alarm.checks >= 10, "the alarm expired repeatedly during the batch");
    check(alarm.sawBusy, "a batch in progress is classified as busy");
    check(alarm.aborts == 0, "no abort while the processor is inside a batch (pipeline compile burst)");
}

// A processor halted at the FIFO breakpoint makes no progress; that is a
// hang only if no game thread can run to move the breakpoint.
// Returns the outstanding draw done.
std::uint64_t testHaltedWhileAThreadCanRunIsNotAborted() {
    const std::uint64_t base = GXS::processedPosition();
    GXS::setBreakpoint(base);
    const std::uint64_t n = GXS::noteDrawDoneIssued();
    GXS::processLimit(base, base + 1024);  // halted: limit == processed
    GXS::WaitCheck wait;
    std::uint64_t now = 1000000000;
    GXS::checkWait(wait, now);
    GXS::WaitVerdict verdict{};
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));  // the breakpoint-hit event is delivered meanwhile
        now += kHalfSecond;
        verdict = GXS::checkWait(wait, now);  // this OS thread holds the CPU
        check(!verdict.abort, "no abort while a game thread holds the CPU");
    }
    check(verdict.state == GXS::WaitState::Halted, "halted at the breakpoint");
    check(verdict.stalledNs >= 19 * 1000000000ull, "the stall time accumulates");
    return n;
}

// ...and when nothing can move it, the alarm aborts and the waiter wakes:
// the draw done discarded by the abort is credited to the recovery draw done.
void testHaltedWithNothingRunnableAborts(std::uint64_t n) {
    const std::uint64_t written = GXS::processedPosition() + 1024;  // the previous test's halted batch
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> recovery{0};
    AlarmResult alarm;
    std::thread alarmThread([&] { alarm = runAlarm(stop, true, &recovery); });
    std::thread processor([&] {
        while (recovery.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // The processor applies the abort (skips the discarded stream), then
        // processes the recovery draw done written after it.
        GXS::abortApplied(written, written);
        GXS::reportDrawDone(written + 8);
        GXS::reportProgress(written + 8, written + 8);
    });
    GXS::waitDrawDone(n);
    stop = true;
    alarmThread.join();
    processor.join();
    check(alarm.aborts == 1, "a halted processor with nothing runnable is aborted");
    check(alarm.checks >= 2 && alarm.checks <= 10, "after at least a second of no progress");
    check(recovery.load() == n + 1, "the recovery draw done follows the one lost to the abort");
    GXS::waitDrawDone(recovery.load());  // delivered: returns at once
    check(!GXS::gpStatus().breakpoint, "the abort cleared the breakpoint");
}

// The hang report names every OS thread, the CPU holder, what each waits
// on and where its host thread is, and still reports when the interrupt lock
// is held by a stuck thread.
OSMessageQueue gBlockedQueue;
OSMessage gBlockedSlots[1];
void* blockedThread(void*) {
    OSMessage msg;
    OSReceiveMessage(&gBlockedQueue, &msg, OS_MESSAGE_BLOCK);
    return nullptr;
}

std::string readReport(const std::function<void(std::FILE*)>& write) {
    std::FILE* file = std::tmpfile();
    write(file);
    std::fflush(file);
    std::rewind(file);
    std::string text;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, n);
    }
    std::fclose(file);
    return text;
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

void testHangReport() {
    OSInitMessageQueue(&gBlockedQueue, gBlockedSlots, 1);
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, blockedThread, nullptr, stack + sizeof(stack), sizeof(stack), 10, 0);
    OSResumeThread(&thread);  // it runs at once (higher priority) and blocks
    const std::string os = readReport([](std::FILE* f) { PetariNative::Platform::Diagnostics::dumpOS(f); });
    check(contains(os, "[holds the CPU]: running, priority 16"), "the report marks the CPU holder");
    check(contains(os, "waiting, priority 10 (base 10), on queue"), "a blocked thread and its queue are listed");
    check(contains(os, "blockedThread"), "threads are named by their entry function");
    check(contains(os, "host WAITING (blocked)"), "host run states are sampled");
    check(contains(os, "[hang]     at "), "host stacks are sampled");
    check(contains(os, "pending alarms"), "pending alarms are listed");
    const std::string gx = readReport([](std::FILE* f) { GXS::dumpState(f); });
    check(contains(gx, "GX sync: processed") && contains(gx, "draw done: issued"), "GX sync state is reported");

    // A thread that never releases the interrupt lock: the report still comes.
    std::string stuck;
    BOOL enabled = OSDisableInterrupts();
    std::thread reporter([&] { stuck = readReport([](std::FILE* f) { PetariNative::Platform::Diagnostics::dumpOS(f); }); });
    reporter.join();
    OSRestoreInterrupts(enabled);
    check(contains(stuck, "stayed unavailable for 2 s (holder: OS thread"), "a held interrupt lock is named, not waited for");

    OSSendMessage(&gBlockedQueue, nullptr, OS_MESSAGE_BLOCK);
    OSJoinThread(&thread, nullptr);
}

}  // namespace

int main() {
    __OSThreadInit();
    gRenderer.start();
    testOrderingAndDrawDone();
    testWaitYieldsBaton();
    testBreakpoint();
    testNoDropsAndInterruptsDisabled();
    testDrawSyncManagerProtocol();
    testSnapshotLifetime();
    testTokensBeforeNextFrame();
    gRenderer.stop();
    GXS::shutdown();
    testCompileBurstIsNotAHang();
    testHaltedWithNothingRunnableAborts(testHaltedWhileAThreadCanRunIsNotAborted());
    testHangReport();
    GXS::shutdown();
    testMisuse();
    OSReport("platform GX sync tests passed (%d checks)\n", checks);
    return 0;
}

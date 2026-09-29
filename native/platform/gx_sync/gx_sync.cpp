// GP interrupts and GX synchronisation. See petari/platform/gx_sync.hpp and
// native/platform/GX_SYNC_PLAN.md.
//
// Reports from the renderer's command-processor thread go into an unbounded
// queue (never dropped) under a private mutex; that thread never touches the
// OS interrupt lock, so a game thread holding interrupts disabled can never
// stall the processor. One GP interrupt thread delivers events in report order
// with the OS interrupt lock held, as PE token / PE finish interrupts are
// delivered on the console. A token event with a snapshot ticket holds the
// queue until the renderer completes that ticket; the processor keeps going.
// Draw-done waits count delivered draw-done interrupts: OS threads sleep on an
// OSThreadQueue (yielding the CPU baton), host threads on a condition
// variable. The GP interrupt thread also wakes OS threads waiting for
// processing progress, outside the event order.

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include <revolution/gx/GXPerf.h>
#include <revolution/os.h>

#include "os_internal.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/diagnostics.hpp"
#include "petari/platform/gx_sync.hpp"

namespace OS = PetariNative::Platform::OS;

namespace PetariNative::Platform::GXSync {
namespace {

constexpr std::uint64_t kPointerBase = std::uint64_t(1) << 32;  // opaque FIFO pointers start at 4 GiB

enum class Kind { Token, DrawDone, Breakpoint };

struct Event {
    Kind kind;
    std::uint16_t token;
    std::uint64_t position;
    std::uint64_t ticket = 0;      // Token: snapshot ticket, 0 when none
    bool ready = true;             // Token: snapshot complete
    std::uint64_t lostDrawDones = 0;  // DrawDone: draw-done BPs discarded by an abort before this one
};

struct State {
    std::mutex lock;  // everything below up to the atomics
    std::condition_variable changed;
    std::deque<Event> queue;
    std::uint64_t lastReported = 0;
    std::uint64_t processed = 0;
    std::uint64_t written = 0;
    bool breakpointReached = false;
    bool running = false;
    bool stop = false;
    pthread_t thread{};

    SnapshotHook snapshotHook = nullptr;
    void* snapshotUser = nullptr;
    FlushHook flushHook = nullptr;
    void* flushUser = nullptr;
    std::uint64_t nextTicket = 1;      // never reset: tickets are unique for the process
    std::uint64_t flushRequested = 0;  // highest ticket passed to the flush hook

    std::uint64_t drawDonesReported = 0;
    std::uint64_t abortIssued = 0;      // draw dones issued at the last GXAbortFrame
    bool abortPending = false;
    std::uint64_t lostDrawDones = 0;    // credited to the next draw-done event

    int progressWaiters = 0;            // OS threads asleep in waitProcessed
    bool progressWake = false;
    int drawDoneWaiters = 0;

    std::atomic<std::uint64_t> breakpoint{kNoBreakpoint};
    std::atomic<std::uint16_t> lastToken{0};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<GXDrawSyncCallback> drawSync{nullptr};
    std::atomic<GXDrawDoneCallback> drawDone{nullptr};
    std::atomic<GXBreakPtCallback> breakpointCallback{nullptr};

    std::atomic<std::uint64_t> drawDoneIssued{0};
    std::atomic<std::uint64_t> drawDoneDelivered{0};
    OSThreadQueue drawDoneWaitQueue{nullptr, nullptr};  // under the OS interrupt lock
    OSThreadQueue progressWaitQueue{nullptr, nullptr};  // under the OS interrupt lock
    std::mutex hostWaitLock;
    std::condition_variable hostWait;
};

State& state() {
    static State* instance = [] {
        PetariNative::HostAllocationScope hostAllocations;  // first use may be on a game thread
        return new State;
    }();
    return *instance;
}

thread_local std::uint64_t tDeliveringTicket = 0;

void notifyHostWaiters(State& s) {
    std::lock_guard<std::mutex> guard(s.hostWaitLock);
    s.hostWait.notify_all();
}

void deliver(State& s, const Event& e) {
    BOOL enabled = OSDisableInterrupts();
    switch (e.kind) {
    case Kind::Token:
        tDeliveringTicket = e.ticket;
        if (GXDrawSyncCallback cb = s.drawSync.load(std::memory_order_acquire)) {
            cb(e.token);
        }
        tDeliveringTicket = 0;
        if (e.ticket != 0) {
            s.delivered.store(e.ticket, std::memory_order_release);
        }
        break;
    case Kind::DrawDone:
        if (GXDrawDoneCallback cb = s.drawDone.load(std::memory_order_acquire)) {
            cb();
        }
        s.drawDoneDelivered.fetch_add(1 + e.lostDrawDones, std::memory_order_acq_rel);
        OSWakeupThread(&s.drawDoneWaitQueue);
        break;
    case Kind::Breakpoint:
        if (GXBreakPtCallback cb = s.breakpointCallback.load(std::memory_order_acquire)) {
            cb();
        }
        break;
    }
    OSRestoreInterrupts(enabled);
    if (e.kind == Kind::DrawDone) {
        notifyHostWaiters(s);
    }
}

void* interruptMain(void*) {
    PetariNative::HostAllocationScope hostAllocations;
    State& s = state();
    std::unique_lock<std::mutex> lock(s.lock);
    while (true) {
        s.changed.wait(lock, [&] { return s.stop || s.progressWake || (!s.queue.empty() && s.queue.front().ready); });
        if (s.progressWake) {
            // Not a GP interrupt: wakes waitProcessed sleepers, never held
            // back by an incomplete snapshot.
            s.progressWake = false;
            s.progressWaiters = 0;
            lock.unlock();
            BOOL enabled = OSDisableInterrupts();
            OSWakeupThread(&s.progressWaitQueue);
            OSRestoreInterrupts(enabled);
            lock.lock();
            continue;
        }
        if (s.queue.empty() || !s.queue.front().ready) {
            return nullptr;  // stop requested; ready events all delivered
        }
        const Event e = s.queue.front();
        s.queue.pop_front();
        lock.unlock();
        deliver(s, e);
        lock.lock();
    }
}

void startLocked(State& s) {
    if (!s.running) {
        s.stop = false;
        if (pthread_create(&s.thread, nullptr, interruptMain, nullptr) != 0) {
            OS::fatal("cannot start the GP interrupt thread");
        }
        s.running = true;
    }
}

// Returns the ticket to pass to the flush hook, or 0.
std::uint64_t enqueueLocked(State& s, Event e) {
    if (e.position < s.lastReported) {
        OSPanic(__FILE__, __LINE__, "GX sync: event at stream position %llu reported after position %llu (out of stream order)",
                static_cast<unsigned long long>(e.position), static_cast<unsigned long long>(s.lastReported));
    }
    s.lastReported = e.position;
    startLocked(s);
    std::uint64_t flush = 0;
    if (e.kind == Kind::DrawDone || e.kind == Kind::Breakpoint) {
        if (e.kind == Kind::DrawDone) {
            ++s.drawDonesReported;
            e.lostDrawDones = s.lostDrawDones;
            s.lostDrawDones = 0;
        }
        // Something the game may wait for is now queued behind these tickets.
        for (auto it = s.queue.rbegin(); it != s.queue.rend(); ++it) {
            if (it->kind == Kind::Token && !it->ready) {
                if (it->ticket > s.flushRequested && s.flushHook != nullptr) {
                    s.flushRequested = it->ticket;
                    flush = it->ticket;
                }
                break;
            }
        }
    }
    s.queue.push_back(e);
    s.changed.notify_one();
    return flush;
}

void enqueue(Event e) {
    State& s = state();
    PetariNative::HostAllocationScope hostAllocations;
    FlushHook hook;
    void* user;
    std::uint64_t flush;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        flush = enqueueLocked(s, e);
        hook = s.flushHook;
        user = s.flushUser;
    }
    if (flush != 0) {
        hook(flush, user);
    }
}

// Caller holds s.lock.
bool markBreakpointReachedLocked(State& s, std::uint64_t position, std::uint64_t& flush) {
    if (s.breakpointReached) {
        return false;
    }
    s.breakpointReached = true;
    flush = enqueueLocked(s, {Kind::Breakpoint, 0, position});
    return true;
}

void progressLocked(State& s, std::uint64_t processed, std::uint64_t written) {
    s.processed = processed;
    s.written = written;
    if (processed < s.breakpoint.load(std::memory_order_acquire)) {
        s.breakpointReached = false;
    }
    if (s.progressWaiters > 0 && !s.progressWake) {
        s.progressWake = true;
        startLocked(s);
        s.changed.notify_one();
    }
}

void callFlush(State& s, std::uint64_t ticket) {
    if (ticket == 0) {
        return;
    }
    FlushHook hook;
    void* user;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        hook = s.flushHook;
        user = s.flushUser;
    }
    if (hook != nullptr) {
        hook(ticket, user);
    }
}

}  // namespace

void* positionToPointer(std::uint64_t position) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(kPointerBase + position));
}

std::uint64_t pointerToPosition(const void* pointer) {
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    if (value < kPointerBase) {
        OSPanic(__FILE__, __LINE__, "GX sync: %p is not a FIFO position from GXGetFifoPtrs", pointer);
    }
    return value - kPointerBase;
}

void reportToken(std::uint16_t token, std::uint64_t position) {
    State& s = state();
    s.lastToken.store(token, std::memory_order_release);  // the PE token register
    PetariNative::HostAllocationScope hostAllocations;
    SnapshotHook hook;
    void* user;
    std::uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        hook = s.snapshotHook;
        user = s.snapshotUser;
        Event e{Kind::Token, token, position};
        if (hook != nullptr) {
            ticket = e.ticket = s.nextTicket++;
            e.ready = false;
        }
        enqueueLocked(s, e);
    }
    if (hook != nullptr) {
        hook(ticket, token, position, user);  // processor thread, no lock held
    }
}

void setTokenRegister(std::uint16_t token) {
    state().lastToken.store(token, std::memory_order_release);
}

void reportDrawDone(std::uint64_t position) {
    enqueue({Kind::DrawDone, 0, position});
}

void reportBreakpointReached(std::uint64_t position) {
    State& s = state();
    PetariNative::HostAllocationScope hostAllocations;
    std::uint64_t flush = 0;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        markBreakpointReachedLocked(s, position, flush);
    }
    callFlush(s, flush);
}

void reportProgress(std::uint64_t processed, std::uint64_t written) {
    State& s = state();
    {
        std::lock_guard<std::mutex> guard(s.lock);
        progressLocked(s, processed, written);
    }
    notifyHostWaiters(s);
}

std::uint64_t processLimit(std::uint64_t processed, std::uint64_t target) {
    State& s = state();
    PetariNative::HostAllocationScope hostAllocations;
    std::uint64_t flush = 0;
    std::uint64_t limit = target;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (target > s.written) {
            s.written = target;  // commands are pending even while halted
        }
        const std::uint64_t bp = s.breakpoint.load(std::memory_order_acquire);
        if (bp >= processed && bp < target) {
            limit = bp;
            if (bp == processed) {
                markBreakpointReachedLocked(s, processed, flush);
            }
        }
    }
    callFlush(s, flush);
    return limit;
}

void abortApplied(std::uint64_t processed, std::uint64_t written) {
    State& s = state();
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (s.abortPending) {
            s.abortPending = false;
            if (s.abortIssued > s.drawDonesReported) {
                s.lostDrawDones += s.abortIssued - s.drawDonesReported;
                s.drawDonesReported = s.abortIssued;
            }
        }
        progressLocked(s, processed, written);
    }
    notifyHostWaiters(s);
}

void setSnapshotHook(SnapshotHook hook, void* user) {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.snapshotHook = hook;
    s.snapshotUser = user;
}

void snapshotReady(std::uint64_t ticket) {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (ticket == 0 || ticket >= s.nextTicket) {
        OSPanic(__FILE__, __LINE__, "GX sync: snapshot ticket %llu was never issued", static_cast<unsigned long long>(ticket));
    }
    for (Event& e : s.queue) {
        if (e.kind == Kind::Token && e.ticket == ticket) {
            if (e.ready) {
                OSPanic(__FILE__, __LINE__, "GX sync: snapshot ticket %llu completed twice", static_cast<unsigned long long>(ticket));
            }
            e.ready = true;
            s.changed.notify_one();
            return;
        }
    }
    // Not queued: already delivered, or discarded by shutdown. Tickets are
    // never reused, so a late completion cannot release another capture.
}

void setFlushHook(FlushHook hook, void* user) {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.flushHook = hook;
    s.flushUser = user;
}

std::uint64_t deliveringTicket() {
    return tDeliveringTicket;
}

std::uint64_t heldTicket() {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    for (const Event& e : s.queue) {
        if (e.kind == Kind::Token && !e.ready) {
            return e.ticket;
        }
    }
    return 0;
}

std::uint64_t deliveredTicket() {
    return state().delivered.load(std::memory_order_acquire);
}

std::uint64_t breakpointPosition() {
    return state().breakpoint.load(std::memory_order_acquire);
}

void setBreakpoint(std::uint64_t position) {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.breakpoint.store(position, std::memory_order_release);
    s.breakpointReached = false;
}

void clearBreakpoint() {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.breakpoint.store(kNoBreakpoint, std::memory_order_release);
    s.breakpointReached = false;
}

std::uint64_t noteDrawDoneIssued() {
    return state().drawDoneIssued.fetch_add(1, std::memory_order_acq_rel) + 1;
}

bool drawDoneWaitPending() {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    return s.drawDoneWaiters > 0;
}

void waitDrawDone(std::uint64_t count) {
    State& s = state();
    const auto done = [&] { return s.drawDoneDelivered.load(std::memory_order_acquire) >= count; };
    {
        std::lock_guard<std::mutex> guard(s.lock);
        ++s.drawDoneWaiters;
    }
    if (OS::boundThread() != nullptr) {
        BOOL enabled = OSDisableInterrupts();
        while (!done()) {
            OSSleepThread(&s.drawDoneWaitQueue);  // other game threads run meanwhile
        }
        OSRestoreInterrupts(enabled);
    } else {
        if (OS::interruptsDisabled()) {
            OSPanic(__FILE__, __LINE__, "GXDrawDone from interrupt context would never complete");
        }
        std::unique_lock<std::mutex> lock(s.hostWaitLock);
        s.hostWait.wait(lock, done);
    }
    std::lock_guard<std::mutex> guard(s.lock);
    --s.drawDoneWaiters;
}

void waitProcessed(std::uint64_t position) {
    State& s = state();
    if (OS::boundThread() != nullptr) {
        BOOL enabled = OSDisableInterrupts();
        while (true) {
            {
                std::lock_guard<std::mutex> guard(s.lock);
                if (s.processed >= position) {
                    break;
                }
                ++s.progressWaiters;
            }
            // The waker needs the interrupt lock, held until this sleeps.
            OSSleepThread(&s.progressWaitQueue);
        }
        OSRestoreInterrupts(enabled);
        return;
    }
    if (OS::interruptsDisabled()) {
        OSPanic(__FILE__, __LINE__, "waiting for the GX processor from interrupt context would never complete");
    }
    std::unique_lock<std::mutex> lock(s.hostWaitLock);
    s.hostWait.wait(lock, [&] {
        std::lock_guard<std::mutex> guard(s.lock);
        return s.processed >= position;
    });
}

void abortFrame() {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.breakpoint.store(kNoBreakpoint, std::memory_order_release);
    s.breakpointReached = false;
    s.abortIssued = s.drawDoneIssued.load(std::memory_order_acquire);
    s.abortPending = true;
}

GXDrawSyncCallback setDrawSyncCallback(GXDrawSyncCallback callback) {
    OS::InterruptGuard guard;  // not while a GP interrupt is being delivered
    return state().drawSync.exchange(callback, std::memory_order_acq_rel);
}

GXDrawDoneCallback setDrawDoneCallback(GXDrawDoneCallback callback) {
    OS::InterruptGuard guard;
    return state().drawDone.exchange(callback, std::memory_order_acq_rel);
}

GXBreakPtCallback setBreakpointCallback(GXBreakPtCallback callback) {
    OS::InterruptGuard guard;
    return state().breakpointCallback.exchange(callback, std::memory_order_acq_rel);
}

std::uint16_t lastToken() {
    return state().lastToken.load(std::memory_order_acquire);
}

GPStatus gpStatus() {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    const bool idle = s.processed >= s.written;
    return {idle, idle, s.breakpointReached};
}

std::uint64_t processedPosition() {
    std::lock_guard<std::mutex> guard(state().lock);
    return state().processed;
}

WaitVerdict checkWait(WaitCheck& check, std::uint64_t nowNs) {
    OS::InterruptGuard interrupts;  // lock order: interrupt lock, then s.lock
    State& s = state();
    WaitState waitState;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        if (check.sinceNs == 0 || s.processed != check.processed) {
            check.processed = s.processed;
            check.sinceNs = nowNs;
            return {WaitState::Progress, 0, false};
        }
        if (s.drawDoneDelivered.load(std::memory_order_acquire) >= s.drawDoneIssued.load(std::memory_order_acquire)) {
            waitState = WaitState::Done;
        } else if (!s.queue.empty()) {
            waitState = WaitState::Delivering;
        } else if (s.processed < s.written) {
            waitState = s.breakpoint.load(std::memory_order_acquire) == s.processed ? WaitState::Halted : WaitState::Busy;
        } else {
            waitState = WaitState::Idle;
        }
    }
    const std::uint64_t stalled = nowNs > check.sinceNs ? nowNs - check.sinceNs : 0;
    const bool stuck = waitState == WaitState::Halted || waitState == WaitState::Idle;
    return {waitState, stalled, stuck && stalled >= kWaitAbortAfterNs && OS::cpuQuiescent()};
}

const char* waitStateName(WaitState waitState) {
    switch (waitState) {
    case WaitState::Progress: return "progress";
    case WaitState::Done: return "draw done delivered";
    case WaitState::Busy: return "processor inside a command batch";
    case WaitState::Delivering: return "GP events awaiting delivery";
    case WaitState::Halted: return "processor halted at the FIFO breakpoint";
    case WaitState::Idle: return "processor idle with a draw done outstanding";
    }
    return "?";
}

void dumpState(std::FILE* out) {
    State& s = state();
    bool locked = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!(locked = s.lock.try_lock()) && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::uint64_t bp = s.breakpoint.load(std::memory_order_acquire);
    std::fprintf(out,
                 "[hang] GX sync%s: processed %llu, written %llu (%llu pending), breakpoint %s%llu%s, last reported %llu, "
                 "interrupt thread %s\n",
                 locked ? "" : " (state lock unavailable for 2 s; read without it)",
                 static_cast<unsigned long long>(s.processed), static_cast<unsigned long long>(s.written),
                 static_cast<unsigned long long>(s.written > s.processed ? s.written - s.processed : 0),
                 bp == kNoBreakpoint ? "none" : "at ", static_cast<unsigned long long>(bp == kNoBreakpoint ? 0 : bp),
                 s.breakpointReached ? " (reached)" : "", static_cast<unsigned long long>(s.lastReported),
                 s.running ? "running" : "stopped");
    std::fprintf(out,
                 "[hang] GX sync draw done: issued %llu, reported %llu, delivered %llu, waiters %d; abort pending %d "
                 "(issued at abort %llu, lost %llu); progress waiters %d%s; last token %u\n",
                 static_cast<unsigned long long>(s.drawDoneIssued.load()), static_cast<unsigned long long>(s.drawDonesReported),
                 static_cast<unsigned long long>(s.drawDoneDelivered.load()), s.drawDoneWaiters, s.abortPending ? 1 : 0,
                 static_cast<unsigned long long>(s.abortIssued), static_cast<unsigned long long>(s.lostDrawDones),
                 s.progressWaiters, s.progressWake ? " (wake pending)" : "", static_cast<unsigned>(s.lastToken.load()));
    std::fprintf(out, "[hang] GX sync tickets: next %llu, delivered %llu, flush requested %llu; %zu queued GP events",
                 static_cast<unsigned long long>(s.nextTicket), static_cast<unsigned long long>(s.delivered.load()),
                 static_cast<unsigned long long>(s.flushRequested), s.queue.size());
    if (locked && !s.queue.empty()) {
        const Event& e = s.queue.front();
        std::fprintf(out, "; head: %s at %llu, ticket %llu%s",
                     e.kind == Kind::Token ? "token" : e.kind == Kind::DrawDone ? "draw done" : "breakpoint",
                     static_cast<unsigned long long>(e.position), static_cast<unsigned long long>(e.ticket),
                     e.ready ? "" : " (capture incomplete: holds every later event)");
    }
    std::fputc('\n', out);
    if (locked) {
        s.lock.unlock();
    }
    std::fflush(out);
}

void shutdown() {
    if (OS::interruptsDisabled()) {
        OSPanic(__FILE__, __LINE__, "GX sync shutdown with interrupts disabled would deadlock the GP interrupt thread");
    }
    State& s = state();
    bool join;
    {
        std::lock_guard<std::mutex> guard(s.lock);
        join = s.running;
        s.stop = true;
    }
    s.changed.notify_all();
    if (join) {
        pthread_join(s.thread, nullptr);
    }
    PetariNative::HostAllocationScope hostAllocations;
    std::lock_guard<std::mutex> guard(s.lock);
    s.queue.clear();
    s.running = false;
    s.stop = false;
    s.lastReported = s.processed = s.written = 0;
    s.breakpointReached = false;
    s.snapshotHook = nullptr;
    s.snapshotUser = nullptr;
    s.flushHook = nullptr;
    s.flushUser = nullptr;
    s.drawDonesReported = s.abortIssued = s.lostDrawDones = 0;
    s.abortPending = false;
    s.progressWaiters = s.drawDoneWaiters = 0;
    s.progressWake = false;
    s.breakpoint.store(kNoBreakpoint);
    s.lastToken.store(0);
    s.drawSync.store(nullptr);
    s.drawDone.store(nullptr);
    s.breakpointCallback.store(nullptr);
    s.drawDoneIssued.store(0);
    s.drawDoneDelivered.store(0);
}

}  // namespace PetariNative::Platform::GXSync

extern "C" {

// DIAGNOSTIC, NOT HARDWARE COUNTERS. The Wii returns transform-unit and
// rasterizer performance counters and a GP clock count. The native renderer
// has no such counters. Here:
// - clocks = low 32 bits of FIFO command bytes the command processor has
//   processed. It is a progress indicator, not XF/GP clock cycles; it changes
//   exactly when the processor makes progress, which is what the game's hang
//   check (handleGXAbortAlarm) compares between two reads.
// - xfWaitIn, xfWaitOut, rasBusy = 0: not measured.
void GXReadXfRasMetric(u32* xfWaitIn, u32* xfWaitOut, u32* rasBusy, u32* clocks) {
    *xfWaitIn = 0;
    *xfWaitOut = 0;
    *rasBusy = 0;
    *clocks = static_cast<u32>(PetariNative::Platform::GXSync::processedPosition());
}

}  // extern "C"

namespace {

std::uint64_t steadyNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Full hang reports for a stall that is not aborted: first after this long,
// then again at this interval while it lasts.
constexpr std::uint64_t kWaitReportAfterNs = 10000000000ull;

}  // namespace

extern "C" {

void petari_platform_dump_hang_state(const char* reason) {
    PetariNative::HostAllocationScope hostAllocations;
    std::fprintf(stderr, "[hang] ---- platform state: %s ----\n", reason);
    PetariNative::Platform::GXSync::dumpState(stderr);
    PetariNative::Platform::Diagnostics::dumpOS(stderr);
    std::fprintf(stderr, "[hang] ---- end of platform state ----\n");
    std::fflush(stderr);
}

int petari_gx_wait_check(PetariGXWaitCheck* check, int pipelineWait) {
    namespace GXSync = PetariNative::Platform::GXSync;
    const std::uint64_t now = steadyNs();
    GXSync::WaitCheck wait{check->processed, check->sinceNs};
    const GXSync::WaitVerdict verdict = GXSync::checkWait(wait, now);
    check->processed = wait.processed;
    check->sinceNs = wait.sinceNs;
    if (verdict.state == GXSync::WaitState::Progress) {
        check->reportedNs = 0;
        return 0;
    }
    const double seconds = static_cast<double>(verdict.stalledNs) / 1e9;
    if (verdict.abort) {
        std::fprintf(stderr, "GX abort: no processor progress for %.1f s (%s) and no game thread can run\n", seconds,
                     GXSync::waitStateName(verdict.state));
        petari_platform_dump_hang_state("GX abort");
        return 1;
    }
    std::fprintf(stderr, "GX wait extended: no processor progress for %.1f s (%s), pipeline=%d\n", seconds,
                 GXSync::waitStateName(verdict.state), pipelineWait);
    if (verdict.stalledNs >= kWaitReportAfterNs && (check->reportedNs == 0 || now - check->reportedNs >= kWaitReportAfterNs)) {
        check->reportedNs = now;
        petari_platform_dump_hang_state("GXDrawDone wait without processor progress");
    }
    return 0;
}

}  // extern "C"

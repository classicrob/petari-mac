// Native OS thread scheduler.
//
// Port of src/RVL_SDK/os/OSThread.c. Run queues, priorities, priority
// inheritance, suspension, join/detach, and the OSThread fields keep the SDK's
// data structures and rules. Only the context switch differs.
//
// The Wii has one CPU: exactly one OS thread runs, always the
// highest-priority ready thread. Natively every OSThread is a host pthread,
// and a CPU "baton" (gCurrent) preserves that invariant: a thread executes game
// code only while it holds the baton. SelectThread() hands the baton to the
// chosen thread and parks the previous one on its condition variable.
//
// Preemption: when an OS thread wakes a higher-priority thread, it switches
// immediately, as on the Wii. When an interrupt source (a host thread: DVD
// drive, alarm timer) wakes one, an idle CPU is dispatched at once. If a
// lower-priority thread is running, it is preempted at its next
// interrupt-state change (OSDisableInterrupts/OSRestoreInterrupts, which
// every OS and DVD call makes). The Wii preempts at any instruction. A thread
// in a long computation without OS calls therefore delays higher-priority
// threads natively until its next OS call.
//
// Stacks: game code runs on a host stack (at least 2 MiB). The game-supplied
// stack keeps its SDK bookkeeping (stackBase, stackEnd, 0xDEADBABE marker) but
// is not executed on. The default thread has no game stack; its stackBase and
// stackEnd are null.

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <string>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <unordered_map>

#include "os_internal.hpp"
#include "os_sdk_private.h"
#include "petari/host_allocation.hpp"
#include "petari/platform/os_host.hpp"

namespace PetariNative::Platform::OS {
namespace {

constexpr u16 kStateReady = OS_THREAD_STATE_READY;
constexpr u16 kStateRunning = OS_THREAD_STATE_RUNNING;
constexpr u16 kStateWaiting = OS_THREAD_STATE_WAITING;
constexpr u16 kStateMoribund = OS_THREAD_STATE_MORIBUND;
constexpr u16 kAttrDetach = 1;
constexpr std::size_t kMinHostStack = 2 * 1024 * 1024;

struct HostThread {
    OSThread* thread = nullptr;
    mach_port_t machThread = MACH_PORT_NULL;  // for the preemption diagnostic's PC sample
    std::uintptr_t stackLow = 0, stackHigh = 0; // host stack bounds, for its bounded frame walk
    std::condition_variable cv;
    bool terminated = false;
    void* (*func)(void*) = nullptr;
    void* param = nullptr;
};

// All scheduler state below is protected by the interrupt lock.
OSThreadQueue RunQueue[32];
u32 RunQueueBits;
BOOL RunQueueHint;
s32 Reschedule;
OSThread DefaultThread;
bool gInitialized;
std::atomic<OSThread*> gCurrent{nullptr};
bool gPreemptPending;
// True once the baton holder's host thread is actually executing (it returned
// from waitForCpu, or gave the baton to itself). A holder that was assigned
// the baton but whose host thread has not woken yet has run nothing since its
// dispatch, so a higher-priority thread readied meanwhile may take the baton
// from it directly (see interruptReschedule).
bool gCurrentRunning;

// ---- Preemption-latency diagnostic (PETARI_BATON_DIAG=1) ----
// An episode starts when an interrupt makes a thread ready that outranks the
// baton holder (interruptReschedule) and ends when the baton changes hands.
// Its length is how long the higher-priority thread (for example JAudio2's
// audio thread) waited, which is long only while the holder runs host code
// or code without OS calls. A reporter thread prints the worst wait once a
// second and, for waits over 10 ms, samples where the holder is: it suspends
// the holder briefly, reads its PC, LR and FP, and walks at most 12 frame
// records on the holder's own (suspended) stack, each checked to lie inside
// that thread's stack bounds, aligned, and moving toward the stack base.
bool gBatonDiag = false;
// The open episode. Written under the interrupt lock and episodeLock(); the
// reporter snapshots it under episodeLock() only (never while a thread is
// suspended).
struct BatonEpisode {
    std::uint64_t id = 0;            // 0 = none open
    std::int64_t start = 0;          // steady_clock ticks
    OSThread* holder = nullptr;
    int holderPriority = 0, waiterPriority = 0;
    mach_port_t holderPort = MACH_PORT_NULL;
    std::uintptr_t stackLow = 0, stackHigh = 0;
    std::uint64_t sampleId = 0;      // episode the sample below belongs to
    std::uint64_t sample[12] = {};   // pc, lr, then return addresses
};
// Never destroyed: the detached reporter thread keeps using it while exit()
// runs static destructors.
std::mutex& episodeLock() {
    static std::mutex* lock = [] {
        PetariNative::HostAllocationScope hostAllocations;
        return new std::mutex;
    }();
    return *lock;
}
BatonEpisode gEpisode;
std::atomic<std::uint64_t> gEpisodeId{0};    // id of the open episode, 0 if none (lock-free check)
std::uint64_t gNextEpisodeId = 1;
struct BatonStats {
    std::mutex lock;
    double worstMs = 0;
    int over10 = 0;
    OSThread* holder = nullptr;
    int holderPriority = 0, waiterPriority = 0;
    std::uint64_t frames[12] = {};   // sample of the longest sampled episode
    double sampledWaitMs = 0;
};
BatonStats& batonStats() {
    static BatonStats* stats = [] {
        PetariNative::HostAllocationScope hostAllocations;
        return new BatonStats;
    }();
    return *stats;
}

// Unset, empty, or starting with '0' means off.
bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

std::int64_t nowTicks() {
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

// Interrupt lock held.
void openEpisode(OSThread* holder, int waiterPriority);
void closeEpisode();

std::unordered_map<OSThread*, std::shared_ptr<HostThread>>& hosts() {
    static auto* instance = [] {
        PetariNative::HostAllocationScope hostAllocations;  // first use may be on a game thread
        return new std::unordered_map<OSThread*, std::shared_ptr<HostThread>>;
    }();
    return *instance;
}

thread_local OSThread* tBound = nullptr;
thread_local std::shared_ptr<HostThread> tHost;
// Host blocking (petari/platform/os_host.hpp): the OS thread waits on this
// private queue, like OSSleepThread, while its host thread does host work.
thread_local bool tHostBlocking = false;
thread_local OSThreadQueue tHostBlockingQueue{nullptr, nullptr};

// ---- Queue helpers (the SDK's EnqueueTail/EnqueuePrio/DequeueItem/DequeueHead) ----

void enqueueTail(OSThreadQueue* queue, OSThread* thread) {
    OSThread* prev = queue->tail;
    if (prev == nullptr) {
        queue->head = thread;
    } else {
        prev->link.next = thread;
    }
    thread->link.prev = prev;
    thread->link.next = nullptr;
    queue->tail = thread;
}

void enqueuePrio(OSThreadQueue* queue, OSThread* thread) {
    OSThread* next = queue->head;
    while (next && next->priority <= thread->priority) {
        next = next->link.next;
    }
    if (next == nullptr) {
        enqueueTail(queue, thread);
        return;
    }
    thread->link.next = next;
    OSThread* prev = next->link.prev;
    next->link.prev = thread;
    thread->link.prev = prev;
    if (prev == nullptr) {
        queue->head = thread;
    } else {
        prev->link.next = thread;
    }
}

void dequeueItem(OSThreadQueue* queue, OSThread* thread) {
    OSThread* next = thread->link.next;
    OSThread* prev = thread->link.prev;
    if (next == nullptr) {
        queue->tail = prev;
    } else {
        next->link.prev = prev;
    }
    if (prev == nullptr) {
        queue->head = next;
    } else {
        prev->link.next = next;
    }
}

OSThread* dequeueHead(OSThreadQueue* queue) {
    OSThread* thread = queue->head;
    OSThread* next = thread->link.next;
    if (next == nullptr) {
        queue->tail = nullptr;
    } else {
        next->link.prev = nullptr;
    }
    queue->head = next;
    return thread;
}

void activeEnqueue(OSThread* thread) {
    OSThread* prev = __OSActiveThreadQueue.tail;
    if (prev == nullptr) {
        __OSActiveThreadQueue.head = thread;
    } else {
        prev->linkActive.next = thread;
    }
    thread->linkActive.prev = prev;
    thread->linkActive.next = nullptr;
    __OSActiveThreadQueue.tail = thread;
}

void activeDequeue(OSThread* thread) {
    OSThread* next = thread->linkActive.next;
    OSThread* prev = thread->linkActive.prev;
    if (next == nullptr) {
        __OSActiveThreadQueue.tail = prev;
    } else {
        next->linkActive.prev = prev;
    }
    if (prev == nullptr) {
        __OSActiveThreadQueue.head = next;
    } else {
        prev->linkActive.next = next;
    }
}

bool isActive(OSThread* thread) {
    if (thread->state == 0) {
        return false;
    }
    for (OSThread* active = __OSActiveThreadQueue.head; active; active = active->linkActive.next) {
        if (active == thread) {
            return true;
        }
    }
    return false;
}

bool isSuspended(s32 suspendCount) {
    return 0 < suspendCount;
}

OSPriority highestReadyPriority() {
    return RunQueueBits == 0 ? 32 : static_cast<OSPriority>(__builtin_clz(RunQueueBits));
}

void setRun(OSThread* thread) {
    thread->queue = &RunQueue[thread->priority];
    enqueueTail(thread->queue, thread);
    RunQueueBits |= 1u << (31 - thread->priority);
    RunQueueHint = TRUE;
}

void unsetRun(OSThread* thread) {
    OSThreadQueue* queue = thread->queue;
    dequeueItem(queue, thread);
    if (queue->head == nullptr) {
        RunQueueBits &= ~(1u << (31 - thread->priority));
    }
    thread->queue = nullptr;
}

OSThread* setEffectivePriority(OSThread* thread, OSPriority priority) {
    switch (thread->state) {
    case kStateReady:
        unsetRun(thread);
        thread->priority = priority;
        setRun(thread);
        break;
    case kStateWaiting:
        dequeueItem(thread->queue, thread);
        thread->priority = priority;
        enqueuePrio(thread->queue, thread);
        if (thread->mutex != nullptr) {
            return thread->mutex->thread;
        }
        break;
    case kStateRunning:
        RunQueueHint = TRUE;
        thread->priority = priority;
        break;
    }
    return nullptr;
}

void updatePriority(OSThread* thread) {
    do {
        if (isSuspended(thread->suspend)) {
            break;
        }
        const OSPriority priority = __OSGetEffectivePriority(thread);
        if (thread->priority == priority) {
            break;
        }
        thread = setEffectivePriority(thread, priority);
    } while (thread);
}

// ---- Baton ----

void giveCpu(OSThread* next) {
    if (gBatonDiag) {
        closeEpisode();
    }
    next->queue = nullptr;
    next->state = kStateRunning;
    gCurrentRunning = next == tBound;  // giving the baton to itself: already executing
    gCurrent.store(next, std::memory_order_release);
    auto found = hosts().find(next);
    if (found != hosts().end()) {
        found->second->cv.notify_all();
    }
}

OSThread* popHighestReady() {
    const OSPriority priority = highestReadyPriority();
    OSThreadQueue* queue = &RunQueue[priority];
    OSThread* next = dequeueHead(queue);
    if (queue->head == nullptr) {
        RunQueueBits &= ~(1u << (31 - priority));
    }
    return next;
}

[[noreturn]] void terminateHostThread() {
    // Interrupt lock held. The OS thread is finished; its host thread goes away
    // without unwinding game frames, as a cleared Wii context is never resumed.
    {
        PetariNative::HostAllocationScope hostAllocations;
        auto found = hosts().find(tBound);
        if (found != hosts().end() && found->second == tHost) {
            hosts().erase(found);
        }
        tHost.reset();
    }
    tBound = nullptr;
    interruptMutex().unlock();
    pthread_exit(nullptr);
}

// Parks the calling OS thread until the scheduler gives it the CPU.
void waitForCpu(OSThread* self) {
    std::shared_ptr<HostThread> host = tHost;
    hostWait(host->cv, [&] { return gCurrent.load(std::memory_order_acquire) == self || host->terminated; });
    if (host->terminated) {
        terminateHostThread();
    }
    gCurrentRunning = true;  // interrupt lock held (hostWait returns with it)
}

// Scheduling request from interrupt context, or from an OS thread that does
// not hold the CPU (which cannot happen in a correct program but must not
// corrupt state).
void interruptReschedule() {
    OSThread* current = gCurrent.load(std::memory_order_relaxed);
    if (current == nullptr) {
        if (RunQueueBits != 0) {
            RunQueueHint = FALSE;
            giveCpu(popHighestReady());
        }
        return;
    }
    if (!gCurrentRunning && current->state == kStateRunning && RunQueueBits != 0 && highestReadyPriority() < current->priority) {
        // The holder was dispatched but its host thread has not woken yet, so
        // it has executed nothing: dispatch the higher-priority thread instead
        // and return the holder to the run queue, as the Wii's scheduler would
        // have if both had been ready together. Waiting for the holder to wake
        // just to be preempted cost 5-35 ms (PETARI_BATON_DIAG, boot logs).
        current->state = kStateReady;
        setRun(current);
        RunQueueHint = FALSE;
        giveCpu(popHighestReady());
        return;
    }
    if (current->state != kStateRunning || highestReadyPriority() < current->priority) {
        gPreemptPending = true;
        if (gBatonDiag && current->state == kStateRunning) {
            openEpisode(current, highestReadyPriority());
        }
    }
}

void selectThread(BOOL yield) {
    if (Reschedule > 0) {
        return;
    }
    OSThread* self = tBound;
    if (self == nullptr || gCurrent.load(std::memory_order_relaxed) != self) {
        interruptReschedule();
        return;
    }

    if (self->state == kStateRunning) {
        if (!yield && self->priority <= highestReadyPriority()) {
            gPreemptPending = false;
            return;
        }
        self->state = kStateReady;
        setRun(self);
    }

    RunQueueHint = FALSE;
    gPreemptPending = false;
    if (RunQueueBits == 0) {
        if (gBatonDiag) {
            closeEpisode();
        }
        gCurrent.store(nullptr, std::memory_order_release);
    } else {
        OSThread* next = popHighestReady();
        giveCpu(next);
        if (next == self) {
            return;
        }
    }

    if (self->state == kStateMoribund || self->state == 0) {
        terminateHostThread();
    }
    waitForCpu(self);
}

void requireRunningOsThread(const char* function) {
    if (tHostBlocking) {
        fatal("%s() called between petari_os_begin_host_blocking and petari_os_end_host_blocking (the thread has given up the CPU)", function);
    }
    if (tBound == nullptr) {
        fatal("%s() called from a host thread that is not an OS thread (interrupt context cannot block)", function);
    }
    if (Reschedule > 0) {
        fatal("%s() would block while the scheduler is disabled (OSDisableScheduler)", function);
    }
}

void* hostEntry(void* arg) {
    tHost = *static_cast<std::shared_ptr<HostThread>*>(arg);
    {
        PetariNative::HostAllocationScope hostAllocations;
        delete static_cast<std::shared_ptr<HostThread>*>(arg);
    }
    tBound = tHost->thread;
    tHost->machThread = pthread_mach_thread_np(pthread_self());
    tHost->stackHigh = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    tHost->stackLow = tHost->stackHigh - pthread_get_stacksize_np(pthread_self());
    // An OS thread runs game code: plain new/delete follow the current JKR heap
    // (outside HostAllocationScope), as on the Wii. Host threads keep malloc.
    PetariNative::setGameAllocationThread(true);
    OSDisableInterrupts();
    // Host wake-up latency for high-priority game threads (JAudio2's audio
    // and DVD threads run at priority 2 and 5): they have per-block audio
    // deadlines. Scheduling order among OS threads is still the baton's.
    // (base is read under the interrupt lock.)
    if (tBound->base <= 8) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
    waitForCpu(tBound);
    // Threads start with interrupts enabled (OSInitContext sets MSR[EE]).
    OSEnableInterrupts();
    void* result = tHost->func(tHost->param);
    OSExitThread(result);
    return nullptr;  // not reached
}

void openEpisode(OSThread* holder, int waiterPriority) {
    std::lock_guard<std::mutex> guard(episodeLock());
    if (gEpisode.id != 0) {
        return;  // already waiting on this holder
    }
    auto found = hosts().find(holder);
    gEpisode = BatonEpisode{};
    gEpisode.id = gNextEpisodeId++;
    gEpisode.start = nowTicks();
    gEpisode.holder = holder;
    gEpisode.holderPriority = holder->priority;
    gEpisode.waiterPriority = waiterPriority;
    if (found != hosts().end()) {
        gEpisode.holderPort = found->second->machThread;
        gEpisode.stackLow = found->second->stackLow;
        gEpisode.stackHigh = found->second->stackHigh;
    }
    gEpisodeId.store(gEpisode.id, std::memory_order_release);
}

void closeEpisode() {
    BatonEpisode closed;
    {
        std::lock_guard<std::mutex> guard(episodeLock());
        if (gEpisode.id == 0) {
            return;
        }
        closed = gEpisode;
        gEpisode.id = 0;
        gEpisodeId.store(0, std::memory_order_release);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(nowTicks() - closed.start)).count();
    BatonStats& st = batonStats();
    std::lock_guard<std::mutex> guard(st.lock);
    if (ms > 10.0) {
        ++st.over10;
    }
    if (ms > st.worstMs) {
        st.worstMs = ms;
        st.holder = closed.holder;
        st.holderPriority = closed.holderPriority;
        st.waiterPriority = closed.waiterPriority;
    }
    if (closed.sampleId == closed.id && ms >= st.sampledWaitMs) {
        st.sampledWaitMs = ms;
        std::copy(std::begin(closed.sample), std::end(closed.sample), st.frames);
    }
}

std::string describe(std::uint64_t address) {
    Dl_info info{};
    if (address != 0 && dladdr(reinterpret_cast<void*>(address), &info) && info.dli_sname != nullptr) {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer), "%s+%llu", info.dli_sname,
                      static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(info.dli_saddr)));
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(address));
    return buffer;
}

// Host thread: samples long episodes, reports once a second.
void* batonReporter(void*) {
    PetariNative::HostAllocationScope hostAllocations;
    using namespace std::chrono;
    constexpr double kSampleAfterMs = 10.0;
    auto nextReport = steady_clock::now() + seconds(1);
    while (true) {
        std::this_thread::sleep_for(milliseconds(2));
        BatonEpisode snapshot;
        {
            std::lock_guard<std::mutex> guard(episodeLock());
            snapshot = gEpisode;
        }
        if (snapshot.id != 0 && snapshot.sampleId != snapshot.id && snapshot.holderPort != MACH_PORT_NULL &&
            duration<double, std::milli>(steady_clock::duration(nowTicks() - snapshot.start)).count() > kSampleAfterMs &&
            thread_suspend(snapshot.holderPort) == KERN_SUCCESS) {
            // No lock is held while the holder is suspended (it may hold any).
            std::uint64_t sample[12] = {};
            bool valid = gEpisodeId.load(std::memory_order_acquire) == snapshot.id;  // still the same wait
            arm_thread_state64_t state;
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            if (valid && thread_get_state(snapshot.holderPort, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) ==
                             KERN_SUCCESS) {
                sample[0] = arm_thread_state64_get_pc(state);
                sample[1] = arm_thread_state64_get_lr(state);
                // Frame-pointer walk of the suspended thread's own stack: every
                // frame must lie inside its stack bounds, be aligned, and move
                // toward the stack base.
                std::uintptr_t fp = arm_thread_state64_get_fp(state);
                for (int i = 2; i < 12 && snapshot.stackLow != 0; ++i) {
                    if (fp < snapshot.stackLow || fp + 16 > snapshot.stackHigh || (fp & 7) != 0) {
                        break;
                    }
                    const std::uintptr_t* frame = reinterpret_cast<const std::uintptr_t*>(fp);
                    const std::uintptr_t next = frame[0];
                    sample[i] = frame[1] & 0x0000FFFFFFFFFFFFull;  // strip pointer authentication
                    if (next <= fp) {
                        break;
                    }
                    fp = next;
                }
            } else {
                valid = false;
            }
            thread_resume(snapshot.holderPort);
            if (valid) {
                std::lock_guard<std::mutex> guard(episodeLock());
                if (gEpisode.id == snapshot.id) {  // attach only to the episode it was taken in
                    gEpisode.sampleId = snapshot.id;
                    std::copy(std::begin(sample), std::end(sample), gEpisode.sample);
                }
            }
        }
        if (steady_clock::now() >= nextReport) {
            nextReport += seconds(1);
            BatonStats& st = batonStats();
            double worst, sampledWait;
            int over10, holderPriority, waiterPriority;
            OSThread* holder;
            std::uint64_t frames[12];
            {
                std::lock_guard<std::mutex> guard(st.lock);
                worst = st.worstMs;
                over10 = st.over10;
                holder = st.holder;
                holderPriority = st.holderPriority;
                waiterPriority = st.waiterPriority;
                std::copy(std::begin(st.frames), std::end(st.frames), frames);
                sampledWait = st.sampledWaitMs;
                st.worstMs = st.sampledWaitMs = 0;
                st.over10 = 0;
                std::fill(std::begin(st.frames), std::end(st.frames), 0);
            }
            if (worst > 5.0) {
                std::fprintf(stderr, "[baton] worst preemption wait %.1f ms (priority-%d thread waited on OS thread %p, priority %d); %d waits over 10 ms",
                             worst, waiterPriority, static_cast<void*>(holder), holderPriority, over10);
                if (frames[0] != 0) {
                    std::fprintf(stderr, "; holder at %s", describe(frames[0]).c_str());
                    for (int i = 1; i < 12 && frames[i] != 0; ++i) {
                        std::fprintf(stderr, " <- %s", describe(frames[i]).c_str());
                    }
                    std::fprintf(stderr, " (%.1f ms wait)", sampledWait);
                }
                std::fputc('\n', stderr);
            }
        }
    }
}

}  // namespace

OSThread* boundThread() {
    // A host-blocking OS thread has given up the CPU: platform waits must
    // treat it as a host thread.
    return tHostBlocking ? nullptr : tBound;
}

void onInterruptsDisabled() {
    if (tBound != nullptr && gPreemptPending && gCurrent.load(std::memory_order_relaxed) == tBound) {
        selectThread(FALSE);
    }
}

void onInterruptsEnabling() {
    if (tBound == nullptr) {
        if (RunQueueHint) {
            interruptReschedule();
        }
    } else if (gPreemptPending && gCurrent.load(std::memory_order_relaxed) == tBound) {
        selectThread(FALSE);
    }
}

}  // namespace PetariNative::Platform::OS

using namespace PetariNative::Platform::OS;

extern "C" {

OSThreadQueue __OSActiveThreadQueue;

void __OSThreadInit(void) {
    BOOL enabled = OSDisableInterrupts();
    if (gInitialized) {
        fatal("__OSThreadInit() called twice");
    }
    gInitialized = true;

    OSThread* thread = &DefaultThread;
    thread->state = kStateRunning;
    thread->attr = kAttrDetach;
    thread->priority = thread->base = 16;
    thread->suspend = 0;
    thread->value = reinterpret_cast<void*>(-1);
    thread->mutex = nullptr;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = nullptr;
    thread->stackBase = nullptr;
    thread->stackEnd = nullptr;

    RunQueueBits = 0;
    RunQueueHint = FALSE;
    for (int prio = 0; prio <= 31; prio++) {
        OSInitThreadQueue(&RunQueue[prio]);
    }
    OSInitThreadQueue(&__OSActiveThreadQueue);
    activeEnqueue(thread);
    Reschedule = 0;

    {
        PetariNative::HostAllocationScope hostAllocations;
        auto host = std::make_shared<HostThread>();
        host->thread = thread;
        host->machThread = pthread_mach_thread_np(pthread_self());
        host->stackHigh = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
        host->stackLow = host->stackHigh - pthread_get_stacksize_np(pthread_self());
        hosts()[thread] = host;
        tHost = host;
    }
    tBound = thread;
    PetariNative::setGameAllocationThread(true);  // the default thread runs game code
    if (envEnabled("PETARI_BATON_DIAG") && !gBatonDiag) {
        gBatonDiag = true;
        pthread_t reporter;
        PetariNative::HostAllocationScope hostAllocations;
        if (pthread_create(&reporter, nullptr, batonReporter, nullptr) == 0) {
            pthread_detach(reporter);
        }
    }
    gCurrentRunning = true;
    gCurrent.store(thread, std::memory_order_release);
    OSRestoreInterrupts(enabled);
}

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->head = queue->tail = nullptr;
}

OSThread* OSGetCurrentThread(void) {
    return gCurrent.load(std::memory_order_acquire);
}

BOOL OSIsThreadSuspended(OSThread* thread) {
    return (0 < thread->suspend) ? TRUE : FALSE;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return (thread->state == kStateMoribund || thread->state == 0) ? TRUE : FALSE;
}

s32 OSDisableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule++;
    OSRestoreInterrupts(enabled);
    return count;
}

s32 OSEnableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule--;
    OSRestoreInterrupts(enabled);
    return count;
}

OSPriority __OSGetEffectivePriority(OSThread* thread) {
    OSPriority priority = thread->base;
    for (OSMutex* mutex = thread->queueMutex.head; mutex; mutex = mutex->link.next) {
        OSThread* blocked = mutex->queue.head;
        if (blocked != nullptr && blocked->priority < priority) {
            priority = blocked->priority;
        }
    }
    return priority;
}

void __OSPromoteThread(OSThread* thread, OSPriority priority) {
    do {
        if (thread->suspend > 0 || thread->priority <= priority) {
            break;
        }
        thread = setEffectivePriority(thread, priority);
    } while (thread);
}

void __OSReschedule(void) {
    if (RunQueueHint) {
        selectThread(FALSE);
    }
}

void OSYieldThread(void) {
    BOOL enabled = OSDisableInterrupts();
    selectThread(TRUE);
    OSRestoreInterrupts(enabled);
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr) {
    if (priority < 0 || 31 < priority) {
        return FALSE;
    }
    if (!gInitialized) {
        fatal("OSCreateThread() before __OSThreadInit()");
    }

    thread->state = kStateReady;
    thread->attr = static_cast<u16>(attr & kAttrDetach);
    thread->priority = thread->base = priority;
    thread->suspend = 1;
    thread->value = reinterpret_cast<void*>(-1);
    thread->mutex = nullptr;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = nullptr;
    thread->queue = nullptr;
    thread->stackBase = static_cast<u8*>(stack);
    thread->stackEnd = nullptr;
    if (stack != nullptr && stackSize >= sizeof(u32)) {
        thread->stackEnd = reinterpret_cast<u32*>(static_cast<u8*>(stack) - stackSize);
        *thread->stackEnd = 0xDEADBABE;
    }
    thread->error = 0;
    thread->specific[0] = thread->specific[1] = nullptr;

    BOOL enabled = OSDisableInterrupts();
    std::shared_ptr<HostThread> host;
    {
        PetariNative::HostAllocationScope hostAllocations;
        host = std::make_shared<HostThread>();
        host->thread = thread;
        host->func = func;
        host->param = param;

        // Reinitialising an OSThread whose previous host thread never ran to
        // completion (for example, created but never resumed) retires it.
        auto found = hosts().find(thread);
        if (found != hosts().end()) {
            found->second->terminated = true;
            found->second->cv.notify_all();
        }

        pthread_attr_t attrs;
        pthread_attr_init(&attrs);
        std::size_t hostStack = std::max<std::size_t>(kMinHostStack, std::size_t(stackSize) * 4);
        hostStack = (hostStack + 0x3FFF) & ~std::size_t(0x3FFF);
        pthread_attr_setstacksize(&attrs, hostStack);
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
        auto* arg = new std::shared_ptr<HostThread>(host);
        pthread_t handle;
        const int error = pthread_create(&handle, &attrs, hostEntry, arg);
        pthread_attr_destroy(&attrs);
        if (error != 0) {
            delete arg;
            if (found != hosts().end()) {
                hosts().erase(found);
            }
            OSRestoreInterrupts(enabled);
            return FALSE;
        }
        hosts()[thread] = host;
    }
    activeEnqueue(thread);
    OSRestoreInterrupts(enabled);
    return TRUE;
}

void OSExitThread(void* val) {
    OSDisableInterrupts();
    OSThread* currentThread = tBound;
    if (currentThread == nullptr || gCurrent.load(std::memory_order_relaxed) != currentThread) {
        fatal("OSExitThread() called outside a running OS thread");
    }

    if (currentThread->attr & kAttrDetach) {
        activeDequeue(currentThread);
        currentThread->state = 0;
    } else {
        currentThread->state = kStateMoribund;
        currentThread->value = val;
    }

    __OSUnlockAllMutex(currentThread);
    OSWakeupThread(&currentThread->queueJoin);
    // A finished thread gives up the CPU even with the scheduler disabled.
    Reschedule = 0;
    RunQueueHint = TRUE;
    selectThread(FALSE);
    terminateHostThread();
}

void OSCancelThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();

    switch (thread->state) {
    case kStateReady:
        if (!isSuspended(thread->suspend)) {
            unsetRun(thread);
        }
        break;
    case kStateRunning:
        RunQueueHint = TRUE;
        break;
    case kStateWaiting:
        dequeueItem(thread->queue, thread);
        thread->queue = nullptr;
        if (!isSuspended(thread->suspend) && thread->mutex) {
            updatePriority(thread->mutex->thread);
        }
        break;
    default:
        OSRestoreInterrupts(enabled);
        return;
    }

    if (thread->attr & kAttrDetach) {
        activeDequeue(thread);
        thread->state = 0;
    } else {
        thread->state = kStateMoribund;
    }

    __OSUnlockAllMutex(thread);

    if (thread != tBound) {
        // Not running here: its host thread is parked and exits when woken.
        // A running thread cancelled from interrupt context stops at its next
        // scheduling point.
        auto found = hosts().find(thread);
        if (found != hosts().end() && thread != gCurrent.load(std::memory_order_relaxed)) {
            found->second->terminated = true;
            found->second->cv.notify_all();
        }
    }

    OSWakeupThread(&thread->queueJoin);
    if (thread == tBound) {
        Reschedule = 0;
        RunQueueHint = TRUE;
        selectThread(FALSE);
        terminateHostThread();
    }
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSJoinThread(OSThread* thread, void** val) {
    BOOL enabled = OSDisableInterrupts();

    if (!(thread->attr & kAttrDetach) && thread->state != kStateMoribund && thread->queueJoin.head == nullptr) {
        OSSleepThread(&thread->queueJoin);
        if (!isActive(thread)) {
            OSRestoreInterrupts(enabled);
            return FALSE;
        }
    }

    if (static_cast<volatile OSThread*>(thread)->state == kStateMoribund) {
        if (val) {
            *val = thread->value;
        }
        activeDequeue(thread);
        thread->state = 0;
        OSRestoreInterrupts(enabled);
        return TRUE;
    }

    OSRestoreInterrupts(enabled);
    return FALSE;
}

void OSDetachThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    thread->attr |= kAttrDetach;
    if (thread->state == kStateMoribund) {
        activeDequeue(thread);
        thread->state = 0;
    }
    OSWakeupThread(&thread->queueJoin);
    OSRestoreInterrupts(enabled);
}

s32 OSResumeThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend--;

    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        switch (thread->state) {
        case kStateReady:
            thread->priority = __OSGetEffectivePriority(thread);
            setRun(thread);
            break;
        case kStateWaiting:
            dequeueItem(thread->queue, thread);
            thread->priority = __OSGetEffectivePriority(thread);
            enqueuePrio(thread->queue, thread);
            if (thread->mutex) {
                updatePriority(thread->mutex->thread);
            }
            break;
        }
        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return suspendCount;
}

s32 OSSuspendThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend++;

    if (suspendCount == 0) {
        switch (thread->state) {
        case kStateRunning:
            RunQueueHint = TRUE;
            thread->state = kStateReady;
            break;
        case kStateReady:
            unsetRun(thread);
            break;
        case kStateWaiting:
            dequeueItem(thread->queue, thread);
            thread->priority = 32;
            enqueueTail(thread->queue, thread);
            if (thread->mutex) {
                updatePriority(thread->mutex->thread);
            }
            break;
        }
        if (thread == tBound && Reschedule > 0) {
            fatal("OSSuspendThread() of the running thread while the scheduler is disabled");
        }
        __OSReschedule();
    }

    OSRestoreInterrupts(enabled);
    return suspendCount;
}

void OSSleepThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    requireRunningOsThread("OSSleepThread");
    OSThread* currentThread = tBound;

    currentThread->state = kStateWaiting;
    currentThread->queue = queue;
    enqueuePrio(queue, currentThread);
    RunQueueHint = TRUE;
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

void OSWakeupThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    while (queue->head) {
        OSThread* thread = dequeueHead(queue);
        thread->state = kStateReady;
        if (!isSuspended(thread->suspend)) {
            setRun(thread);
        }
    }
    __OSReschedule();
    OSRestoreInterrupts(enabled);
}

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    if (priority < 0 || priority > 31) {
        return FALSE;
    }
    BOOL enabled = OSDisableInterrupts();
    if (thread->base != priority) {
        thread->base = priority;
        updatePriority(thread);
        __OSReschedule();
    }
    OSRestoreInterrupts(enabled);
    return TRUE;
}

OSPriority OSGetThreadPriority(OSThread* thread) {
    return thread->base;
}

void petari_os_begin_host_blocking(void) {
    if (tHostBlocking) {
        fatal("petari_os_begin_host_blocking() nested");
    }
    if (interruptsDisabled()) {
        fatal("petari_os_begin_host_blocking() with interrupts disabled");
    }
    OSDisableInterrupts();
    requireRunningOsThread("petari_os_begin_host_blocking");
    OSThread* self = tBound;
    if (gCurrent.load(std::memory_order_relaxed) != self) {
        fatal("petari_os_begin_host_blocking() from an OS thread that does not hold the CPU");
    }
    // As OSSleepThread on a private queue, but the host thread keeps running
    // (host work only) instead of parking.
    self->state = kStateWaiting;
    self->queue = &tHostBlockingQueue;
    enqueuePrio(&tHostBlockingQueue, self);
    tHostBlocking = true;
    RunQueueHint = FALSE;
    gPreemptPending = false;
    if (RunQueueBits == 0) {
        if (gBatonDiag) {
            closeEpisode();
        }
        gCurrent.store(nullptr, std::memory_order_release);
    } else {
        giveCpu(popHighestReady());
    }
    OSEnableInterrupts();
}

void petari_os_end_host_blocking(void) {
    if (!tHostBlocking) {
        fatal("petari_os_end_host_blocking() without petari_os_begin_host_blocking()");
    }
    if (interruptsDisabled()) {
        fatal("petari_os_end_host_blocking() with interrupts disabled");
    }
    OSDisableInterrupts();
    OSThread* self = tBound;
    // As OSWakeupThread from interrupt context: ready again at its priority
    // (unless another thread suspended it meanwhile); an idle CPU is taken at
    // once, otherwise the running thread is preempted at its next
    // interrupt-state change if this one has higher priority.
    dequeueItem(&tHostBlockingQueue, self);
    self->state = kStateReady;
    if (!isSuspended(self->suspend)) {
        setRun(self);
    }
    tHostBlocking = false;
    interruptReschedule();
    waitForCpu(self);
    OSEnableInterrupts();
}

static void SleepAlarmHandler(OSAlarm* alarm, OSContext*) {
    OSResumeThread(static_cast<OSThread*>(OSGetAlarmUserData(alarm)));
}

void OSSleepTicks(OSTime tick) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* current = tBound;
    if (current == nullptr) {
        fatal("OSSleepTicks() called from a host thread that is not an OS thread");
    }
    OSAlarm sleepAlarm;
    OSCreateAlarm(&sleepAlarm);
    OSSetAlarmTag(&sleepAlarm, 0);
    OSSetAlarmUserData(&sleepAlarm, current);
    OSSetAlarm(&sleepAlarm, tick, SleepAlarmHandler);
    OSSuspendThread(current);
    OSCancelAlarm(&sleepAlarm);
    OSRestoreInterrupts(enabled);
}

}  // extern "C"

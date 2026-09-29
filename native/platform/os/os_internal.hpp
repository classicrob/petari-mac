#pragma once
// Shared internals of the native OS layer. Not for game code.
//
// Interrupt model: OSDisableInterrupts() acquires one global mutex (the
// "interrupt lock"). Each host thread keeps its own enabled/disabled state,
// as each Wii thread keeps its own MSR[EE]. Host threads that are not OS
// threads (DVD drive, alarm timer, later VI/audio) act as interrupt handlers:
// they take the interrupt lock, may make OS threads ready, and on re-enabling
// interrupts hand the CPU to a newly ready thread if the CPU is idle, or ask
// the running thread to reschedule.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <pthread/qos.h>
#include <string>
#include <vector>
#include <mutex>

#include <revolution/os.h>

namespace PetariNative::Platform::OS {

std::mutex& interruptMutex();

// True if the calling host thread currently has interrupts disabled.
bool interruptsDisabled();

// Hang diagnostics: the host thread (pthread_self) that last took the
// interrupt lock and still holds it, or 0. Maintained by the lock's users in
// this directory (other waits that release it inside a condition variable,
// such as the VI thread's, leave it stale while they sleep).
std::uintptr_t interruptOwner();
void setInterruptOwner(bool held);  // interrupt lock held (true) / about to be released (false)

// The OS thread bound to the calling host thread, or null for host threads
// (interrupt context).
OSThread* boundThread();

// For host threads (not OS threads) that hold the interrupt lock: waits on cv,
// releasing the interrupt lock while blocked. OS threads must block with
// OSSleepThread instead, so the CPU passes to another thread.
template <class Predicate>
void hostWait(std::condition_variable& cv, Predicate predicate) {
    std::unique_lock<std::mutex> lock(interruptMutex(), std::adopt_lock);
    setInterruptOwner(false);  // released while waiting
    cv.wait(lock, predicate);
    setInterruptOwner(true);
    lock.release();
}

// Interrupt lock held. True when no game code can run until an interrupt
// source (a host thread) makes an OS thread ready: no thread holds the CPU,
// none is ready, and none is doing host work between
// petari_os_begin_host_blocking and petari_os_end_host_blocking.
bool cpuQuiescent();

// Hang reports (petari/platform/diagnostics.hpp). dumpThreads lists every OS
// thread with its scheduler state, host run state and a frame-pointer sample
// of its host stack; dumpAlarms lists the pending alarms. Both take the
// interrupt lock if the caller does not hold it, waiting at most two seconds;
// if it stays unavailable they read the state without it (the process is
// hung) and say so.
void dumpThreads(std::FILE* out);
void dumpAlarms(std::FILE* out);

// Host waits while holding the CPU (os_thread.cpp's baton monitor).
struct BatonBlockStats {
    std::uint64_t stretches;  // waits of at least the threshold
    double totalMs, maxMs;
    std::vector<std::string> sites;
    std::uint64_t forcedPreemptions;  // busy-waiting holders preempted at safe points
    std::uint64_t unsafePreemptions;  // polls that found the holder at an unsafe point
};
BatonBlockStats batonBlockStats();
void setBatonBlockThresholdMs(double ms);
void setBatonBlockLogAll(bool enabled);
void setForcedPreemption(bool enabled, double afterMs);  // PETARI_FORCED_PREEMPTION; default on, 4 ms  // tests: log every stretch, not the first three per site
void dumpBatonBlocks(std::FILE* out);  // interrupt lock held, or read racily by dumpThreads
std::string describeAddress(std::uint64_t address);  // symbol+offset, or hex

// Scheduler hooks used by the interrupt layer (os_thread.cpp).
void onInterruptsEnabling();   // interrupt lock still held
void onInterruptsDisabled();   // interrupt lock just acquired
// Host QoS inheritance for the baton holder (os_thread.cpp). Interrupt lock
// held: take the overrides detached by changes of hands; the caller ends them
// with endOverrides() AFTER releasing the interrupt mutex.
void takeDeferredOverrides(std::vector<pthread_override_t>& out);  // swaps; no allocation under the lock
void endOverrides(std::vector<pthread_override_t>& overrides);       // clears
// For tests; read with the interrupt lock held for a consistent view.
struct HolderOverrideStats {
    std::uint64_t started;
    std::uint64_t detached;
    std::uint64_t ended;
    bool active;
    OSThread* target;
    int deferred;
};
HolderOverrideStats holderOverrideStats();

// PETARI_BATON_DIAG: counts the calling OS thread's OSDisableInterrupts calls
// (nested = interrupts were already disabled, so it was a no-op).
void noteInterruptDisable(bool nested);

[[noreturn]] void fatal(const char* fmt, ...);

// Crash-report bookkeeping (os_crash.cpp): recent OSReport lines and the last
// OSPanic/OSFatal message.
void recordReport(const char* text);
void recordPanic(const char* message);

// RAII interrupt disable for native platform code.
class InterruptGuard {
public:
    InterruptGuard() : mEnabled(OSDisableInterrupts()) {}
    ~InterruptGuard() { OSRestoreInterrupts(mEnabled); }
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

private:
    BOOL mEnabled;
};

}  // namespace PetariNative::Platform::OS

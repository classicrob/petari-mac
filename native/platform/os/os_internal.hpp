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
#include <pthread.h>
#include <pthread/qos.h>
#include <vector>
#include <mutex>

#include <revolution/os.h>

namespace PetariNative::Platform::OS {

std::mutex& interruptMutex();

// True if the calling host thread currently has interrupts disabled.
bool interruptsDisabled();

// The OS thread bound to the calling host thread, or null for host threads
// (interrupt context).
OSThread* boundThread();

// For host threads (not OS threads) that hold the interrupt lock: waits on cv,
// releasing the interrupt lock while blocked. OS threads must block with
// OSSleepThread instead, so the CPU passes to another thread.
template <class Predicate>
void hostWait(std::condition_variable& cv, Predicate predicate) {
    std::unique_lock<std::mutex> lock(interruptMutex(), std::adopt_lock);
    cv.wait(lock, predicate);
    lock.release();
}

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

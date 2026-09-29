// OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts on the host.
// See os_internal.hpp for the model.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "os_internal.hpp"
#include "petari/host_allocation.hpp"

namespace PetariNative::Platform::OS {
namespace {

thread_local bool tDisabled = false;

void disable() {
    interruptMutex().lock();
    tDisabled = true;
    onInterruptsDisabled();
}

void enable() {
    onInterruptsEnabling();
    tDisabled = false;
    interruptMutex().unlock();
}

}  // namespace

std::mutex& interruptMutex() {
    // Never destroyed: interrupt sources may still run during static destruction.
    static std::mutex* instance = [] {
        PetariNative::HostAllocationScope hostAllocations;  // first use may be on a game thread
        return new std::mutex;
    }();
    return *instance;
}

bool interruptsDisabled() {
    return tDisabled;
}

void fatal(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    std::fputs("OS fatal: ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::abort();
}

}  // namespace PetariNative::Platform::OS

using namespace PetariNative::Platform::OS;

extern "C" {

BOOL OSDisableInterrupts(void) {
    if (tDisabled) {
        noteInterruptDisable(true);
        return FALSE;
    }
    disable();
    noteInterruptDisable(false);
    return TRUE;
}

BOOL OSEnableInterrupts(void) {
    if (!tDisabled) {
        return TRUE;
    }
    enable();
    return FALSE;
}

BOOL OSRestoreInterrupts(BOOL level) {
    const BOOL previous = tDisabled ? FALSE : TRUE;
    if (level && tDisabled) {
        enable();
    } else if (!level && !tDisabled) {
        disable();
    }
    return previous;
}

}  // extern "C"

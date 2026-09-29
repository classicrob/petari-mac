// Power, reset, shutdown functions, and exit on the host. Replaces the
// relevant parts of src/RVL_SDK/os/{OSReset,OSStateTM}.c (see
// petari/platform/power.hpp). Also PPCSync and OSRegisterVersion.
//
// Kept from the SDK: one-shot power/reset callbacks that fall back to no-op
// defaults, the latched reset-button state, the priority-ordered shutdown
// function queue and its non-final/final passes, the shutdown event numbers,
// and OSGetResetCode's 0x80000000 restart flag. Native differences: there is
// no SRAM to sync, no audio/PAD hardware to stop here, no Wii Menu, standby,
// or console reboot (the application decides), and OS threads are not
// cancelled before the exit handler runs.

#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSResetSW.h>

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "os_internal.hpp"
#include "petari/platform/power.hpp"

namespace PPower = PetariNative::Platform::Power;

namespace {

void defaultResetCallback() {}
void defaultPowerCallback() {}

// Protected by the interrupt lock.
OSResetCallback gResetCallback = defaultResetCallback;
OSPowerCallback gPowerCallback = defaultPowerCallback;
BOOL gResetDown = FALSE;
bool gResetHeld = false;
PPower::ExitHandler gExitHandler = nullptr;
void* gExitUser = nullptr;

struct ShutdownQueue {
    OSShutdownFunctionInfo* head;
    OSShutdownFunctionInfo* tail;
} gShutdownQueue;

void enqueueByPriority(OSShutdownFunctionInfo* info) {
    OSShutdownFunctionInfo* next = gShutdownQueue.head;
    while (next && next->priority <= info->priority) {
        next = next->next;
    }
    if (next == nullptr) {
        info->prev = gShutdownQueue.tail;
        info->next = nullptr;
        if (gShutdownQueue.tail) {
            gShutdownQueue.tail->next = info;
        } else {
            gShutdownQueue.head = info;
        }
        gShutdownQueue.tail = info;
        return;
    }
    info->next = next;
    info->prev = next->prev;
    next->prev = info;
    if (info->prev) {
        info->prev->next = info;
    } else {
        gShutdownQueue.head = info;
    }
}

[[noreturn]] void leave(PPower::Intent intent, std::uint32_t resetCode, std::uint32_t event) {
    // __OSShutdownDevices: non-final passes until every function agrees, then
    // the final pass with interrupts disabled.
    while (!__OSCallShutdownFunctions(FALSE, event)) {
    }
    OSDisableInterrupts();
    __OSCallShutdownFunctions(TRUE, event);
    const PPower::ExitHandler handler = gExitHandler;
    void* user = gExitUser;
    OSEnableInterrupts();

    const PPower::Exit exit{intent, resetCode, event};
    if (handler) {
        handler(exit, user);
        OSPanic(__FILE__, __LINE__, "power exit handler returned; OS exit functions cannot return");
    }
    switch (intent) {
    case PPower::Intent::Restart:
        PPower::relaunch(resetCode);
        OSPanic(__FILE__, __LINE__, "OSRestart(): relaunching the application failed");
        break;
    case PPower::Intent::ReturnToMenu:
        OSReport("OSReturnToMenu(): there is no Wii Menu on this host; exiting.\n");
        break;
    case PPower::Intent::Reboot:
        OSReport("OSRebootSystem(): there is no console to reboot on this host; exiting.\n");
        break;
    case PPower::Intent::Shutdown:
        break;
    }
    std::fflush(nullptr);
    std::exit(EXIT_SUCCESS);
}

}  // namespace

namespace PetariNative::Platform::Power {

void setExitHandler(ExitHandler handler, void* user) {
    OS::InterruptGuard guard;
    gExitHandler = handler;
    gExitUser = user;
}

void pressPowerButton() {
    BOOL enabled = OSDisableInterrupts();
    OSPowerCallback cb = gPowerCallback;
    gPowerCallback = defaultPowerCallback;
    cb();
    OSRestoreInterrupts(enabled);
}

void setResetButton(bool pressed) {
    BOOL enabled = OSDisableInterrupts();
    const bool edge = pressed && !gResetHeld;
    gResetHeld = pressed;
    if (edge) {
        gResetDown = TRUE;
        OSResetCallback cb = gResetCallback;
        gResetCallback = defaultResetCallback;
        cb();
    }
    OSRestoreInterrupts(enabled);
}

void relaunch(std::uint32_t resetCode) {
    char value[16];
    std::snprintf(value, sizeof(value), "%u", resetCode);
    setenv(kResetCodeEnvironmentVariable, value, 1);
    char** argv = *_NSGetArgv();
    std::fflush(nullptr);
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        execv(path, argv);
    }
    if (argv && argv[0]) {
        execv(argv[0], argv);
    }
}

}  // namespace PetariNative::Platform::Power

namespace OS = PetariNative::Platform::OS;

extern "C" {

OSPowerCallback OSSetPowerCallback(OSPowerCallback callback) {
    OS::InterruptGuard guard;
    OSPowerCallback previous = gPowerCallback;
    gPowerCallback = callback ? callback : defaultPowerCallback;
    return previous == defaultPowerCallback ? nullptr : previous;
}

OSResetCallback OSSetResetCallback(OSResetCallback callback) {
    OS::InterruptGuard guard;
    OSResetCallback previous = gResetCallback;
    gResetCallback = callback ? callback : defaultResetCallback;
    return previous == defaultResetCallback ? nullptr : previous;
}

// True once per press of the (host) reset button.
BOOL OSGetResetButtonState(void) {
    OS::InterruptGuard guard;
    const BOOL state = gResetDown;
    gResetDown = FALSE;
    return state;
}

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) {
    OS::InterruptGuard guard;
    enqueueByPriority(info);
}

BOOL __OSCallShutdownFunctions(BOOL final, u32 event) {
    BOOL failed = FALSE;
    u32 priority = 0;
    for (OSShutdownFunctionInfo* info = gShutdownQueue.head; info; info = info->next) {
        // Lower-priority functions wait until every higher-priority one succeeds.
        if (failed && priority != info->priority) {
            break;
        }
        failed |= !info->func(final, event);
        priority = info->priority;
    }
    return failed ? FALSE : TRUE;
}

// Reset code of this boot: 0x80000000 | code after OSRestart(code), else 0
// (power-on).
u32 OSGetResetCode(void) {
    const char* value = std::getenv(PPower::kResetCodeEnvironmentVariable);
    if (value == nullptr || *value == '\0') {
        return 0;
    }
    return 0x80000000u | static_cast<u32>(std::strtoul(value, nullptr, 10));
}

void OSShutdownSystem(void) {
    leave(PPower::Intent::Shutdown, 0, 2);
}

void OSRestart(u32 resetCode) {
    leave(PPower::Intent::Restart, resetCode, 4);
}

void OSReturnToMenu(void) {
    leave(PPower::Intent::ReturnToMenu, 0, 5);
}

void OSRebootSystem(void) {
    leave(PPower::Intent::Reboot, 0, 1);
}

// Memory barrier: ordering only on a coherent host.
void PPCSync(void) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void OSRegisterVersion(const char* id) {
    OSReport("%s\n", id);
}

}  // extern "C"

// Native VI. Replaces src/RVL_SDK/vi/{vi,vi3in1,i2c}.c. Register programming
// becomes pending/flushed/latched state (see petari/platform/vi.hpp). The
// retrace handler keeps __VIRetraceHandler's order: count, pre-callback,
// latch, post-callback, wake VIWaitForRetrace sleepers, dimming bookkeeping.
//
// Not implemented: beam position (VIGetCurrentLine, position callbacks),
// gamma/3-in-1 video DAC programming beyond the trap-filter flag, DVD motor
// idle stop, and the forbidden PAL<->NTSC mode-change panic (the host has no
// boot-ROM TV mode besides setBootTvMode).

#include <revolution/os.h>
#include <revolution/sc.h>
#include <revolution/vi.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

#include "os_internal.hpp"
#include "petari/frame_telemetry.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/vi.hpp"

namespace OS = PetariNative::Platform::OS;
namespace VIN = PetariNative::Platform::VI;

namespace {

using Clock = std::chrono::steady_clock;

struct Pending {
    void* frameBuffer = nullptr;
    bool black = true;
    bool configured = false;
    GXRenderModeObj renderMode{};
    u32 tvMode = VI_TVMODE_NTSC_INT;
    u16 panX = 0, panY = 0, panWidth = 0, panHeight = 0;
    bool trapFilter = false;
};

// All state below is protected by the interrupt lock.
bool gInitialized;
VIN::Clock gClock = VIN::Clock::Internal;
u32 gBootTvMode = VI_TVMODE_NTSC_INT;
Pending gPending;   // changed by VISet*/VIConfigure
Pending gFlushed;   // armed by VIFlush
bool gFlushFlag;
Pending gLatched;   // shown
u32 gRetraceCount;
VIRetraceCallback gPreCB;
VIRetraceCallback gPostCB;
OSThreadQueue gRetraceQueue;
VIN::Hooks gHooks;

// Dimming (screen saver), counted in retraces as in vi.c.
bool gDimmingEnabled = true;
bool gActivity;
u32 gIdleCount;
bool gDimmed;

// VI thread.
bool gThreadRunning;
bool gStopRequested;
pthread_t gThread;

std::condition_variable& viCv() {
    static auto* instance = [] {
        PetariNative::HostAllocationScope hostAllocations;  // first use may be on a game thread
        return new std::condition_variable;
    }();
    return *instance;
}

// VIGetTvFormat's mapping of VITVMode >> 2.
u32 tvFormatOf(u32 tvMode) {
    const u32 format = tvMode >> 2;
    switch (format) {
    case 1:
    case 4:
        return VI_PAL;
    case 2:
    case 5:
        return format;
    default:
        return VI_NTSC;
    }
}

u32 dimmingThreshold() {
    // Five minutes of retraces (NEW_TIME_TO_DIMMING for VI_DM_DEFAULT).
    return tvFormatOf(gLatched.tvMode) == VI_PAL ? 15000 : 18000;
}

VIN::DisplayState snapshotLocked() {
    VIN::DisplayState s;
    s.frameBuffer = gLatched.frameBuffer;
    s.black = gLatched.black;
    s.configured = gLatched.configured;
    s.renderMode = gLatched.renderMode;
    s.tvFormat = tvFormatOf(gLatched.tvMode);
    s.scanMode = gLatched.tvMode & 3;
    s.panX = gLatched.panX;
    s.panY = gLatched.panY;
    s.panWidth = gLatched.panWidth;
    s.panHeight = gLatched.panHeight;
    s.trapFilter = gLatched.trapFilter;
    s.dimmed = gDimmed;
    s.retraceCount = gRetraceCount;
    s.fieldRate = VIN::fieldRateFor(gLatched.tvMode);
    return s;
}

// __VIRetraceHandler. Interrupts disabled, on a host (interrupt) thread.
void retraceInterrupt() {
    gRetraceCount++;
    PetariNative::FrameTelemetry::lastRetraceNs.store(PetariNative::FrameTelemetry::nowNs(), std::memory_order_relaxed);
    if (gPreCB) {
        gPreCB(gRetraceCount);
    }
    if (gFlushFlag) {
        gLatched = gFlushed;
        gFlushFlag = false;
        if (gHooks.onLatched) {
            gHooks.onLatched(snapshotLocked(), gHooks.user);
        }
    }
    if (gPostCB) {
        gPostCB(gRetraceCount);
    }
    OSWakeupThread(&gRetraceQueue);

    if (gActivity) {
        gActivity = false;
        gIdleCount = 0;
        gDimmed = false;
    } else if (gDimmingEnabled && gIdleCount < 0xFFFFFFFF) {
        gIdleCount++;
        if (gIdleCount >= dimmingThreshold()) {
            gDimmed = true;
        }
    }
}

// Retrace timer thread scheduling: a Mach time-constraint (real-time) thread,
// so a busy host does not delay retraces the way it delays ordinary threads.
// A retrace does little work (callbacks, a latch, thread wake-ups).
void makeRetraceThreadRealtime(Clock::duration period) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    const auto ticks = [&](double ms) { return static_cast<std::uint32_t>(ms * 1e6 * timebase.denom / timebase.numer); };
    thread_time_constraint_policy_data_t policy;
    policy.period = ticks(std::chrono::duration<double, std::milli>(period).count());
    policy.computation = ticks(0.5);
    policy.constraint = ticks(2.0);
    policy.preemptible = TRUE;
    if (thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
}

// Sleeps until an absolute steady-clock deadline without holding any lock.
void sleepUntil(Clock::time_point deadline) {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return info;
    }();
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now()).count();
    if (remaining <= 0) {
        return;
    }
    const std::uint64_t ticks = static_cast<std::uint64_t>(remaining) * timebase.denom / timebase.numer;
    mach_wait_until(mach_absolute_time() + ticks);
}

Clock::duration fieldPeriodLocked() {
    return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / VIN::fieldRateFor(gLatched.tvMode)));
}

// The retrace clock. Waits for each deadline with no lock held, then takes
// the interrupt lock (as the hardware interrupt waits while interrupts are
// disabled) and delivers one retrace. A stop request is seen at the next
// deadline, within one field.
void* viThreadMain(void*) {
    namespace Telemetry = PetariNative::FrameTelemetry;
    OSDisableInterrupts();
    Clock::duration period = fieldPeriodLocked();
    OSEnableInterrupts();
    // PETARI_VI_LEGACY_TIMER=1: default scheduling and a condition-variable
    // timed wait, as before, for A/B measurement.
    const char* legacyValue = std::getenv("PETARI_VI_LEGACY_TIMER");
    const bool legacy = legacyValue != nullptr && legacyValue[0] != '\0' && legacyValue[0] != '0';
    if (!legacy) {
        makeRetraceThreadRealtime(period);
    }
    auto next = Clock::now();
    while (true) {
        next += period;
        if (legacy) {
            std::mutex sleepLock;
            std::condition_variable sleeper;
            std::unique_lock<std::mutex> lock(sleepLock);
            sleeper.wait_until(lock, next, [] { return false; });
        } else {
            sleepUntil(next);
        }
        const auto woke = Clock::now();
        if (woke > next) {
            Telemetry::add(Telemetry::ViTimerLate, std::chrono::duration_cast<std::chrono::nanoseconds>(woke - next).count());
        }
        OSDisableInterrupts();
        Telemetry::add(Telemetry::ViInterruptLockWait,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - woke).count());
        if (gStopRequested) {
            OSEnableInterrupts();
            break;
        }
        // One interrupt per wake-up, like the hardware. After a long host
        // stall the timeline restarts instead of firing a burst.
        if (woke - next > 4 * period) {
            next = woke;
        }
        retraceInterrupt();
        period = fieldPeriodLocked();
        // Interrupt return: delivers any reschedule the handlers requested.
        OSEnableInterrupts();
    }
    return nullptr;
}

}  // namespace

namespace PetariNative::Platform::VI {

void setHooks(const Hooks& hooks) {
    OS::InterruptGuard guard;
    gHooks = hooks;
}

DisplayState displayState() {
    OS::InterruptGuard guard;
    return snapshotLocked();
}

void setClock(Clock clock) {
    OS::InterruptGuard guard;
    if (gInitialized) {
        OS::fatal("VI clock must be chosen before VIInit()");
    }
    gClock = clock;
}

void signalRetrace() {
    if (OS::boundThread() != nullptr) {
        OS::fatal("signalRetrace() called from an OS thread; retraces are interrupts and need a host thread");
    }
    OS::InterruptGuard guard;
    if (!gInitialized || gClock != Clock::External) {
        OS::fatal("signalRetrace() needs VIInit() with the External clock");
    }
    retraceInterrupt();
}

void setBootTvMode(std::uint32_t viTvMode) {
    OS::InterruptGuard guard;
    gBootTvMode = viTvMode;
}

double fieldRateFor(std::uint32_t viTvMode) {
    return tvFormatOf(viTvMode) == VI_PAL && (viTvMode & 3) != VI_PROGRESSIVE ? 50.0 : 60000.0 / 1001.0;
}

void shutdown() {
    bool join = false;
    pthread_t thread{};
    {
        OS::InterruptGuard guard;
        if (gThreadRunning) {
            gStopRequested = true;
            viCv().notify_all();
            thread = gThread;
            join = true;
        }
    }
    if (join) {
        pthread_join(thread, nullptr);
    }
    OS::InterruptGuard guard;
    gThreadRunning = false;
    gStopRequested = false;
    gInitialized = false;
    gClock = Clock::Internal;
    gPending = gFlushed = gLatched = Pending{};
    gFlushFlag = false;
    gRetraceCount = 0;
    gPreCB = gPostCB = nullptr;
    OSInitThreadQueue(&gRetraceQueue);
    gHooks = Hooks{};
    gDimmingEnabled = true;
    gActivity = false;
    gIdleCount = 0;
    gDimmed = false;
}

}  // namespace PetariNative::Platform::VI

extern "C" {

void VIInit(void) {
    OS::InterruptGuard guard;
    if (gInitialized) {
        return;
    }
    gInitialized = true;
    Pending boot;
    boot.tvMode = gBootTvMode;
    boot.black = true;
    gPending = gFlushed = gLatched = boot;
    gFlushFlag = false;
    gRetraceCount = 0;
    OSInitThreadQueue(&gRetraceQueue);
    if (gClock == VIN::Clock::Internal) {
        PetariNative::HostAllocationScope hostAllocations;
        gStopRequested = false;
        if (pthread_create(&gThread, nullptr, viThreadMain, nullptr) != 0) {
            OS::fatal("cannot start the VI retrace thread");
        }
        gThreadRunning = true;
    }
}

void VIConfigure(const GXRenderModeObj* rm) {
    VIN::Hooks hooks;
    {
        OS::InterruptGuard guard;
        gPending.renderMode = *rm;
        gPending.configured = true;
        gPending.tvMode = rm->viTVmode;
        // The SDK's VIConfigure also resets panning to the whole framebuffer.
        gPending.panX = 0;
        gPending.panY = 0;
        gPending.panWidth = rm->fbWidth;
        gPending.panHeight = rm->xfbHeight;
        hooks = gHooks;
    }
    if (hooks.onConfigure) {
        hooks.onConfigure(rm, hooks.user);
    }
}

void VIConfigurePan(u16 xOrg, u16 yOrg, u16 width, u16 height) {
    OS::InterruptGuard guard;
    gPending.panX = xOrg;
    gPending.panY = yOrg;
    gPending.panWidth = width;
    gPending.panHeight = height;
}

void VIFlush(void) {
    OS::InterruptGuard guard;
    gFlushed = gPending;
    gFlushFlag = true;
}

void VIWaitForRetrace(void) {
    BOOL enabled = OSDisableInterrupts();
    const u32 count = gRetraceCount;
    do {
        OSSleepThread(&gRetraceQueue);
    } while (count == gRetraceCount);
    OSRestoreInterrupts(enabled);
}

void VISetNextFrameBuffer(void* fb) {
    OS::InterruptGuard guard;
    gPending.frameBuffer = fb;
}

void* VIGetNextFrameBuffer(void) {
    OS::InterruptGuard guard;
    return gFlushed.frameBuffer;
}

void* VIGetCurrentFrameBuffer(void) {
    OS::InterruptGuard guard;
    return gLatched.frameBuffer;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    OS::InterruptGuard guard;
    VIRetraceCallback old = gPreCB;
    gPreCB = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    OS::InterruptGuard guard;
    VIRetraceCallback old = gPostCB;
    gPostCB = cb;
    return old;
}

void VISetBlack(BOOL black) {
    OS::InterruptGuard guard;
    gPending.black = black != FALSE;
}

u32 VIGetRetraceCount(void) {
    OS::InterruptGuard guard;
    return gRetraceCount;
}

u32 VIGetTvFormat(void) {
    OS::InterruptGuard guard;
    return tvFormatOf(gLatched.tvMode);
}

u32 VIGetScanMode(void) {
    OS::InterruptGuard guard;
    return gLatched.tvMode & 3;
}

// Digital (component/HDMI-class) output detected: the host display is always
// a progressive-capable digital display.
u32 VIGetDTVStatus(void) {
    return 1;
}

void VISetTrapFilter(VIBool filter) {
    OS::InterruptGuard guard;
    gPending.trapFilter = filter != 0;
}

BOOL VIEnableDimming(BOOL enable) {
    OS::InterruptGuard guard;
    const BOOL old = gDimmingEnabled ? TRUE : FALSE;
    if (enable == TRUE && SCGetScreenSaverMode() == 0) {
        enable = FALSE;
    }
    if ((enable != FALSE) != gDimmingEnabled) {
        gIdleCount = 0;
        gDimmed = false;
    }
    gDimmingEnabled = enable != FALSE;
    return old;
}

u32 VIGetDimmingCount(void) {
    OS::InterruptGuard guard;
    const u32 threshold = dimmingThreshold();
    return gIdleCount >= threshold ? 0 : threshold - gIdleCount;
}

BOOL VIResetDimmingCount(void) {
    OS::InterruptGuard guard;
    gActivity = true;
    return TRUE;
}

}  // extern "C"

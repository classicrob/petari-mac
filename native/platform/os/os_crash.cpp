// Host crash reporting, the OS error-handler registry, OSContext bookkeeping,
// and the PowerPC machine-state calls game code reaches. See
// petari/platform/crash.hpp.
//
// No PowerPC state is invented:
// - OSContext objects are kept and handed out as the SDK does (current
//   context per thread, OSClearContext resets their FPU mode/state), but
//   nothing fills their registers.
// - OSFillFPUContext and OSProtectRange need PowerPC FPU state or MEM1
//   protection channels; they abort with an explanation if reached (only
//   JUTException's exception path, which native faults never enter, calls
//   them).
// - PPCMfmsr/PPCMtmsr model the two MSR fields with host meaning: EE (the
//   calling thread's interrupt state) and FE0/FE1 (floating-point exception
//   enables, which Apple silicon cannot trap: clearing is honoured, setting
//   aborts). Other MSR bits read as zero.
// - OSGetStackPointer returns 0: there is no PowerPC stack, and a truncated
//   host pointer would mislead.
// The signal handler formats with snprintf and walks the stack with
// backtrace(), which are not formally async-signal-safe; the process is
// already dying, and the report is written with write(2).

#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/os/OSError.h>

#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include "os_internal.hpp"
#include "petari/host_allocation.hpp"
#include "petari/platform/crash.hpp"

namespace OS = PetariNative::Platform::OS;

namespace {

constexpr u32 kMsrEe = 0x8000;
constexpr u32 kMsrFe0 = 0x0800;
constexpr u32 kMsrFe1 = 0x0100;

OSErrorHandler gErrorHandlers[OS_ERROR_MAX];
thread_local OSContext* tCurrentContext = nullptr;
OSContext gHostThreadContext;

// Recent OSReport output and the last panic, for crash reports.
constexpr int kReportLines = 32;
constexpr int kLineLength = 160;
char gReportRing[kReportLines][kLineLength];
std::atomic<unsigned> gReportNext{0};
char gPanicMessage[512];
std::atomic<bool> gHavePanic{false};

char gReportPath[1024];
bool gInstalled;

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}

// The Wii exception a game handler would have been registered for.
const char* errorName(int error) {
    static const char* names[OS_ERROR_MAX] = {"SYSTEM_RESET", "MACHINE_CHECK", "DSI", "ISI", "EXTERNAL_INTERRUPT", "ALIGNMENT",
                                              "PROGRAM", "FLOATING_POINT", "DECREMENTER", "SYSTEM_CALL", "TRACE", "PERFORMANCE_MONITOR",
                                              "BREAKPOINT", "SYSTEM_INTERRUPT", "THERMAL_INTERRUPT", "PROTECTION", "FPE"};
    return (error >= 0 && error < OS_ERROR_MAX) ? names[error] : "?";
}

void emit(int fd, const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n > 0) {
        const size_t len = std::min<size_t>(static_cast<size_t>(n), sizeof(line) - 1);
        ssize_t w = ::write(STDERR_FILENO, line, len);
        if (fd >= 0) {
            w = ::write(fd, line, len);
        }
        (void)w;
    }
}

void writeReport(int sig, siginfo_t* info, void* ucontextRaw) {
    const int fd = gReportPath[0] ? ::open(gReportPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    emit(fd, "==== Petari native crash report ====\n");
    emit(fd, "signal: %s (%d), code %d, fault address %p\n", signalName(sig), sig, info ? info->si_code : 0, info ? info->si_addr : nullptr);
    if (gHavePanic.load()) {
        emit(fd, "last panic: %s\n", gPanicMessage);
    }
    OSThread* bound = OS::boundThread();
    OSThread* running = OSGetCurrentThread();
    if (bound) {
        emit(fd, "OS thread: %p (priority %d, base %d) - faulting code ran on this game thread\n", static_cast<void*>(bound), bound->priority,
             bound->base);
    } else {
        emit(fd, "OS thread: none on this host thread (interrupt/host context); running OS thread %p\n", static_cast<void*>(running));
    }
    emit(fd, "interrupts disabled on faulting thread: %s\n", OS::interruptsDisabled() ? "yes" : "no");

    if (ucontextRaw) {
        auto* uc = static_cast<ucontext_t*>(ucontextRaw);
        const auto& ss = uc->uc_mcontext->__ss;
        emit(fd, "host arm64 registers (not PowerPC):\n");
        emit(fd, "  pc %016llx  lr %016llx  sp %016llx  fp %016llx\n", static_cast<unsigned long long>(__darwin_arm_thread_state64_get_pc(ss)),
             static_cast<unsigned long long>(__darwin_arm_thread_state64_get_lr(ss)),
             static_cast<unsigned long long>(__darwin_arm_thread_state64_get_sp(ss)),
             static_cast<unsigned long long>(__darwin_arm_thread_state64_get_fp(ss)));
        for (int i = 0; i < 29; i += 4) {
            emit(fd, "  x%-2d %016llx", i, static_cast<unsigned long long>(ss.__x[i]));
            for (int j = i + 1; j < i + 4 && j < 29; ++j) {
                emit(fd, "  x%-2d %016llx", j, static_cast<unsigned long long>(ss.__x[j]));
            }
            emit(fd, "\n");
        }
    }

    emit(fd, "host backtrace:\n");
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    if (fd >= 0) {
        backtrace_symbols_fd(frames, count, fd);
    }

    emit(fd, "game error handlers (registered, not called: no PowerPC context natively):\n");
    bool any = false;
    for (int e = 0; e < OS_ERROR_MAX; ++e) {
        if (gErrorHandlers[e]) {
            emit(fd, "  %s -> %p\n", errorName(e), reinterpret_cast<void*>(gErrorHandlers[e]));
            any = true;
        }
    }
    if (!any) {
        emit(fd, "  (none)\n");
    }

    emit(fd, "recent OSReport output:\n");
    const unsigned next = gReportNext.load();
    for (unsigned i = (next > kReportLines ? next - kReportLines : 0); i < next; ++i) {
        emit(fd, "  %s\n", gReportRing[i % kReportLines]);
    }
    emit(fd, "==== end of report ====\n");
    if (fd >= 0) {
        ::close(fd);
    }
}

void crashHandler(int sig, siginfo_t* info, void* ucontext) {
    writeReport(sig, info, ucontext);
    // Default action: terminate with this signal (debuggers and the system
    // crash reporter still see it).
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

namespace PetariNative::Platform::OS {

void recordReport(const char* text) {
    // Split into lines; keep the last kReportLines.
    const char* p = text;
    while (*p) {
        const char* end = std::strchr(p, '\n');
        const size_t len = end ? static_cast<size_t>(end - p) : std::strlen(p);
        if (len > 0) {
            const unsigned slot = gReportNext.fetch_add(1) % kReportLines;
            const size_t n = std::min<size_t>(len, kLineLength - 1);
            std::memcpy(gReportRing[slot], p, n);
            gReportRing[slot][n] = '\0';
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
}

void recordPanic(const char* message) {
    std::snprintf(gPanicMessage, sizeof(gPanicMessage), "%s", message);
    gHavePanic.store(true);
}

}  // namespace PetariNative::Platform::OS

namespace PetariNative::Platform::Crash {

void install(const std::filesystem::path& reportDirectory) {
    if (gInstalled) {
        return;
    }
    gInstalled = true;
    PetariNative::HostAllocationScope hostAllocations;
    if (!reportDirectory.empty()) {
        std::snprintf(gReportPath, sizeof(gReportPath), "%s/petari-crash-%ld-%d.txt", reportDirectory.c_str(), static_cast<long>(std::time(nullptr)),
                      static_cast<int>(::getpid()));
    }
    // Faults caused by stack overflow need their own stack.
    static char altStack[128 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction action{};
    action.sa_sigaction = crashHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) {
        sigaction(sig, &action, nullptr);
    }
}

std::filesystem::path reportPath() {
    return gReportPath[0] ? std::filesystem::path(gReportPath) : std::filesystem::path();
}

}  // namespace PetariNative::Platform::Crash

extern "C" {

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    if (error >= OS_ERROR_MAX) {
        return nullptr;
    }
    OS::InterruptGuard guard;
    OSErrorHandler previous = gErrorHandlers[error];
    gErrorHandlers[error] = handler;
    return previous;
}

// SDK default (OSError.c). FPSCR exception enables have no host effect.
u32 __OSFpscrEnableBits = 0xF8;

OSContext* OSGetCurrentContext(void) {
    if (tCurrentContext) {
        return tCurrentContext;
    }
    if (OSThread* bound = OS::boundThread()) {
        return &bound->context;
    }
    return &gHostThreadContext;
}

void OSSetCurrentContext(OSContext* context) {
    tCurrentContext = context;
}

// Resets the context's FPU ownership state, as the SDK does; registers are untouched.
void OSClearContext(OSContext* context) {
    context->mode = 0;
    context->state = 0;
}

void OSFillFPUContext(OSContext*) {
    OSPanic(__FILE__, __LINE__, "OSFillFPUContext(): there is no PowerPC FPU state on this host");
}

void OSProtectRange(u32 channel, void*, u32, u32) {
    OSPanic(__FILE__, __LINE__, "OSProtectRange(%u): MEM1 protection channels are not available on this host", channel);
}

u32 OSGetStackPointer(void) {
    return 0;  // no PowerPC stack natively
}

u32 PPCMfmsr(void) {
    u32 msr = OS::interruptsDisabled() ? 0 : kMsrEe;
    // Floating-point exceptions are never enabled on the host.
    return msr;
}

void PPCMtmsr(u32 value) {
    if (value & (kMsrFe0 | kMsrFe1)) {
        OSPanic(__FILE__, __LINE__, "PPCMtmsr(%08x): floating-point exceptions cannot be enabled on Apple silicon", value);
    }
    if (value & kMsrEe) {
        OSEnableInterrupts();
    } else {
        OSDisableInterrupts();
    }
}

void PPCHalt(void) {
    OSPanic(__FILE__, __LINE__, "PPCHalt(): the game halted the processor");
}

void OSFatal(GXColor, GXColor, const char* message) {
    OS::recordPanic(message);
    std::fprintf(stderr, "OSFatal: %s\n", message);
    std::fflush(nullptr);
    std::abort();
}

}  // extern "C"

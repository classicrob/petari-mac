// Tests for host crash reporting, the error-handler registry, OSContext
// bookkeeping, and the modelled PowerPC machine-state calls.

#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/os/OSError.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "petari/platform/crash.hpp"

extern "C" void __OSThreadInit(void);

namespace fs = std::filesystem;
namespace PCrash = PetariNative::Platform::Crash;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

void gameErrorHandler(OSError, OSContext*, ...) {}

fs::path tempDir(const char* name) {
    const char* base = std::getenv("TMPDIR");
    fs::path path = fs::path(base ? base : "/tmp") / (std::string("petari_crash_") + name + "_" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

// Runs fn in a child that installs crash reporting into dir; returns the wait
// status and the report text.
std::pair<int, std::string> crashInChild(const fs::path& dir, const std::function<void()>& fn) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        std::freopen("/dev/null", "w", stdout);
        PCrash::install(dir);
        fn();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    std::string text;
    for (const auto& entry : fs::directory_iterator(dir)) {
        std::ifstream in(entry.path());
        std::stringstream buffer;
        buffer << in.rdbuf();
        text += buffer.str();
        fs::remove(entry.path());
    }
    return {status, text};
}

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void testRegistryAndContexts() {
    check(OSSetErrorHandler(OS_ERROR_DSI, gameErrorHandler) == nullptr, "no handler registered initially");
    check(OSSetErrorHandler(OS_ERROR_DSI, nullptr) == gameErrorHandler, "previous handler returned");
    check(OSSetErrorHandler(OS_ERROR_MAX, gameErrorHandler) == nullptr, "out-of-range error ignored");

    OSThread* thread = OSGetCurrentThread();
    check(OSGetCurrentContext() == &thread->context, "current context defaults to the running thread's");
    static OSContext exceptionContext;
    std::memset(&exceptionContext, 0xAB, sizeof(exceptionContext));
    OSSetCurrentContext(&exceptionContext);
    check(OSGetCurrentContext() == &exceptionContext, "OSSetCurrentContext");
    OSClearContext(&exceptionContext);
    check(exceptionContext.mode == 0 && exceptionContext.state == 0 && exceptionContext.gpr[0] == 0xABABABAB,
          "OSClearContext resets FPU state only; registers are not invented");
    OSSetCurrentContext(nullptr);
    check(OSGetStackPointer() == 0, "no PowerPC stack pointer natively");

    check(PPCMfmsr() & 0x8000, "MSR[EE] reflects enabled interrupts");
    BOOL e = OSDisableInterrupts();
    check((PPCMfmsr() & 0x8000) == 0, "MSR[EE] clear with interrupts disabled");
    OSRestoreInterrupts(e);
    PPCMtmsr(PPCMfmsr() & ~0x0900);  // JUTException::run: FP exceptions off
    check(PPCMfmsr() & 0x8000, "clearing FE0/FE1 keeps interrupts enabled");
    PPCMtmsr(0);
    check((PPCMfmsr() & 0x8000) == 0, "PPCMtmsr EE=0 disables interrupts");
    PPCMtmsr(0x8000);
    check(PPCMfmsr() & 0x8000, "PPCMtmsr EE=1 enables interrupts");
    PPCSync();
}

void testCrashReports() {
    const fs::path dir = tempDir("reports");
    auto [segv, segvReport] = crashInChild(dir, [] {
        OSSetErrorHandler(OS_ERROR_DSI, gameErrorHandler);
        OSReport("loading scene %d\n", 7);
        volatile int* null = nullptr;
        *null = 1;
    });
    check(WIFSIGNALED(segv) && (WTERMSIG(segv) == SIGSEGV || WTERMSIG(segv) == SIGBUS), "fault terminates with its signal");
    check(has(segvReport, "==== Petari native crash report ====") && (has(segvReport, "SIGSEGV") || has(segvReport, "SIGBUS")), "report names the signal");
    check(has(segvReport, "fault address 0x0"), "report has the fault address");
    check(has(segvReport, "host arm64 registers (not PowerPC)") && has(segvReport, "  pc ") && has(segvReport, "x28"), "host registers, labelled");
    check(has(segvReport, "host backtrace:"), "host backtrace");
    check(has(segvReport, "OS thread: 0x"), "running OS thread");
    check(has(segvReport, "DSI -> 0x"), "registered game handler listed, not called");
    check(has(segvReport, "loading scene 7"), "recent OSReport output");
    check(has(segvReport, "==== end of report ===="), "complete report");

    auto [panic, panicReport] = crashInChild(dir, [] { OSPanic("Scene.cpp", 42, "bad state %d", 3); });
    check(WIFSIGNALED(panic) && WTERMSIG(panic) == SIGABRT, "OSPanic aborts");
    check(has(panicReport, "SIGABRT") && has(panicReport, "last panic: bad state 3 in \"Scene.cpp\" on line 42."), "panic message in the report");

    GXColor black{0, 0, 0, 255};
    auto [fatal, fatalReport] = crashInChild(dir, [&] { OSFatal(black, black, "disc read error"); });
    check(WIFSIGNALED(fatal) && WTERMSIG(fatal) == SIGABRT && has(fatalReport, "last panic: disc read error"), "OSFatal reports its message and aborts");

    auto [fpu, fpuReport] = crashInChild(dir, [] {
        static OSContext ctx;
        OSFillFPUContext(&ctx);
    });
    check(WIFSIGNALED(fpu) && has(fpuReport, "no PowerPC FPU state"), "OSFillFPUContext refuses to invent FPU state");
    auto [fe, feReport] = crashInChild(dir, [] { PPCMtmsr(0x8000 | 0x0900); });
    check(WIFSIGNALED(fe) && has(feReport, "floating-point exceptions cannot be enabled"), "enabling FP exceptions is refused");
    auto [protect, protectReport] = crashInChild(dir, [] { OSProtectRange(0, nullptr, 0, 3); });
    check(WIFSIGNALED(protect) && has(protectReport, "protection channels are not available"), "OSProtectRange is explicit about being unavailable");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    __OSThreadInit();
    testRegistryAndContexts();
    testCrashReports();
    OSReport("platform crash tests passed (%d checks)\n", checks);
    return 0;
}

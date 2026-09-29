// PETARI_BATON_DIAG: a game thread running host code while it holds the CPU
// baton blocks a higher-priority thread that an interrupt made ready. The
// diagnostic must report the wait, the holder, and a PC sample inside the
// holder's code. stderr is captured to a file and checked.

#include <revolution/os.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

extern "C" void __OSThreadInit(void);

namespace {

OSMessageQueue gQueue;
OSMessage gSlot;
std::atomic<bool> gWoke{false};
std::atomic<std::int64_t> gSentAt{0};  // steady_clock ticks when the interrupt readied the thread

void* audioLike(void*) {
    OSReceiveMessage(&gQueue, nullptr, OS_MESSAGE_BLOCK);
    gWoke = true;
    return nullptr;
}

// Host work on the game thread, with no OS call: the baton is not released.
__attribute__((noinline)) void holdBatonInHostCode(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    volatile unsigned spin = 0;
    while (std::chrono::steady_clock::now() < end) {
        spin = spin + 1;
    }
}

}  // namespace

int main() {
    char path[] = "/tmp/petari_baton_diag_XXXXXX";
    const char* tmp = std::getenv("TMPDIR");
    std::string file = std::string(tmp ? tmp : "/tmp") + "/petari_baton_diag_" + std::to_string(::getpid()) + ".log";
    const int fd = ::open(file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    const int savedErr = ::dup(2);
    ::dup2(fd, 2);
    (void)path;

    ::setenv("PETARI_BATON_DIAG", "1", 1);
    __OSThreadInit();
    OSInitMessageQueue(&gQueue, &gSlot, 1);
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, audioLike, nullptr, stack + sizeof(stack), sizeof(stack), 2, 0);
    OSResumeThread(&thread);  // runs and blocks on the queue

    // An "interrupt" (host thread) readies the priority-2 thread while this
    // priority-16 thread is inside host code holding the baton.
    std::thread interrupt([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        BOOL enabled = OSDisableInterrupts();
        gSentAt = std::chrono::steady_clock::now().time_since_epoch().count();
        OSSendMessage(&gQueue, nullptr, OS_MESSAGE_NOBLOCK);
        OSRestoreInterrupts(enabled);
    });
    holdBatonInHostCode(40);
    interrupt.join();  // host thread: the send has happened
    const bool wokeDuringHold = gWoke.load();
    const std::int64_t yieldedAt = std::chrono::steady_clock::now().time_since_epoch().count();
    OSYieldThread();  // an OS call: the waiting thread runs now
    // The wait the diagnostic must report: from the interrupt's send (the
    // episode starts under the same interrupt lock) to this yield.
    const double expectedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(yieldedAt - gSentAt.load())).count();
    OSSleepTicks(OSMillisecondsToTicks(1500));  // let the reporter print

    std::fflush(stderr);
    ::dup2(savedErr, 2);
    ::close(fd);
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    const std::string log = text.str();
    std::remove(file.c_str());

    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", label);
            ++failures;
        }
    };
    check(!wokeDuringHold, "host code holding the baton is not preempted (the modelled limitation)");
    check(gWoke.load(), "the waiting thread runs at the holder's next OS call");
    const auto at = log.find("[baton] worst preemption wait ");
    check(at != std::string::npos, "the diagnostic reports the wait");
    if (at != std::string::npos) {
        const double ms = std::atof(log.c_str() + at + std::string("[baton] worst preemption wait ").size());
        if (std::abs(ms - expectedMs) > 2.0) {
            std::fprintf(stderr, "reported %.2f ms, measured send-to-yield %.2f ms\n", ms, expectedMs);
        }
        check(expectedMs > 10.0, "the hold outlasted the send (test precondition)");
        check(std::abs(ms - expectedMs) <= 2.0, "the reported wait equals the measured send-to-yield time (within 2 ms)");
        check(log.find("priority-2 thread waited", at) != std::string::npos && log.find("priority 16", at) != std::string::npos,
              "waiter and holder priorities");
        check(log.find("holder at ", at) != std::string::npos && log.find("holdBatonInHostCode", at) != std::string::npos,
              "the PC sample names the holder's host code");
    }
    if (failures != 0) {
        std::fprintf(stderr, "diagnostic output was:\n%s\n", log.c_str());
        return 1;
    }
    std::printf("baton diagnostic tests passed\n");
    return 0;
}

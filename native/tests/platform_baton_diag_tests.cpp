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

// Host code that blocks (a host sleep, not an OS sleep) while holding the
// baton: the holder is off-CPU in a blocking call, not runnable.
__attribute__((noinline)) void holdBatonBlockedInHost(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
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

    // Phase 2: the holder blocks in host code while the priority-2 thread waits.
    static OSThread thread2;
    alignas(32) static u8 stack2[0x4000];
    gWoke = false;
    OSCreateThread(&thread2, audioLike, nullptr, stack2 + sizeof(stack2), sizeof(stack2), 2, 0);
    OSResumeThread(&thread2);
    std::thread interrupt2([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        OSSendMessage(&gQueue, nullptr, OS_MESSAGE_NOBLOCK);
    });
    holdBatonBlockedInHost(40);
    interrupt2.join();
    OSYieldThread();
    OSSleepTicks(OSMillisecondsToTicks(1500));

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
        const char* sampleEnv = std::getenv("PETARI_BATON_SAMPLE");
        const bool sampling = sampleEnv == nullptr || sampleEnv[0] == '\0' || sampleEnv[0] != '0';
        if (sampling) {
            check(log.find("holder at ", at) != std::string::npos && log.find("holdBatonInHostCode", at) != std::string::npos,
                  "the PC sample names the holder's host code");
        } else {
            // PETARI_BATON_SAMPLE=0: never suspends the holder.
            check(log.find("holder at ", at) == std::string::npos && log.find("sampler pause 0.00 ms", at) != std::string::npos,
                  "with sampling off there is no PC sample and no sampler pause");
        }
        // The holder spun in host code: it made no interrupt-state change
        // during the wait (its handoff disable is counted after the episode
        // closes) and was on a CPU for it.
        const auto during = log.find("holder during it: ", at);
        check(during != std::string::npos, "the report has the holder's activity during the wait");
        if (during != std::string::npos) {
            const double cpu = std::atof(log.c_str() + during + std::string("holder during it: ").size());
            check(log.find("ms CPU, 0 interrupt disables, 0 nested, sampler pause ", during) != std::string::npos,
                  "no interrupt-state changes by a holder in host code");
            if (cpu < 0.5 * ms) {
                std::fprintf(stderr, "holder CPU %.1f ms of a %.1f ms wait\n", cpu, ms);
            }
            // Lower bound only: Mach thread CPU times lag by up to a quantum.
            check(cpu >= 0.5 * ms, "a spinning holder was on a CPU for most of the wait (quiet machine)");
        }
    }
    // Mid-wait run state (a point sample taken >10 ms into the wait).
    if (at != std::string::npos) {
        check(log.find("holder state at ", at) != std::string::npos, "a mid-wait run-state sample for the spinning holder");
        const auto second = log.find("[baton] worst preemption wait ", at + 1);
        check(second != std::string::npos, "a report for the blocked holder");
        if (second != std::string::npos) {
            const auto end = log.find('\n', second);
            const std::string line = log.substr(second, end == std::string::npos ? std::string::npos : end - second);
            check(line.find("holder state at ") != std::string::npos && line.find("WAITING (blocked)") != std::string::npos,
                  "a holder blocked in a host call reads WAITING at the mid-wait sample");
            check(line.find("0 interrupt disables, 0 nested") != std::string::npos, "and made no interrupt-state change");
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "diagnostic output was:\n%s\n", log.c_str());
        return 1;
    }
    std::printf("baton diagnostic tests passed\n");
    return 0;
}

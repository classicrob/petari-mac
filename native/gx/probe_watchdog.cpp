#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<unsigned> progress{0};
std::atomic<bool> finished{false};
void stalled(int) {
    std::fputs("Probe watchdog: main thread made no frame progress for 20 seconds\n", stderr);
    void* frames[40];
    const int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    std::_Exit(124);
}
}

extern "C" void petari_probe_start_watchdog() {
    std::signal(SIGUSR2, stalled);
    const pthread_t mainThread = pthread_self();
    std::thread([mainThread] {
        unsigned previous = 0;
        unsigned idlePeriods = 0;
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (finished.load()) return;
            const unsigned current = progress.load();
            idlePeriods = current == previous ? idlePeriods + 1 : 0;
            previous = current;
            if (idlePeriods == 2) {
                pthread_kill(mainThread, SIGUSR2);
                return;
            }
        }
    }).detach();
}
extern "C" void petari_probe_watchdog_progress() { ++progress; }
extern "C" void petari_probe_stop_watchdog() { finished = true; }

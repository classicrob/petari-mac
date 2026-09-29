// Tests for native power/reset: STM-style one-shot callbacks, reset button
// latching, the shutdown function protocol, exit intents (handler and
// defaults), and a real OSRestart relaunch reporting its reset code.

#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSResetSW.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "petari/platform/power.hpp"

extern "C" void __OSThreadInit(void);

namespace PPower = PetariNative::Platform::Power;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

// Runs fn in a child; returns its wait status.
int inChild(const std::function<void()>& fn) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        std::freopen("/dev/null", "w", stdout);
        fn();
        ::_exit(99);  // exit functions must not return
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

int gPowerCalls = 0;
int gResetCalls = 0;
void onPower() {
    check(OSDisableInterrupts() == FALSE, "power callback runs with interrupts disabled");
    ++gPowerCalls;
}
void onReset() {
    ++gResetCalls;
}

void testCallbacks() {
    check(OSSetPowerCallback(onPower) == nullptr, "default power callback reports as NULL");
    check(OSSetPowerCallback(onPower) == onPower, "previous callback returned");
    std::thread button([] { PPower::pressPowerButton(); });  // e.g. window close on another thread
    button.join();
    check(gPowerCalls == 1, "power button runs the callback");
    PPower::pressPowerButton();
    check(gPowerCalls == 1, "power callback is one-shot (reverts to the default)");
    check(OSSetPowerCallback(nullptr) == nullptr, "NULL installs the default");

    check(!OSGetResetButtonState(), "reset button not pressed");
    check(OSSetResetCallback(onReset) == nullptr, "install reset callback");
    PPower::setResetButton(true);
    check(gResetCalls == 1, "reset press runs the reset callback");
    check(OSGetResetButtonState() && !OSGetResetButtonState(), "a press is reported once");
    PPower::setResetButton(true);  // still held: no new press
    check(!OSGetResetButtonState() && gResetCalls == 1, "holding the button is one press; callback one-shot");
    PPower::setResetButton(false);
    PPower::setResetButton(true);
    check(OSGetResetButtonState() && gResetCalls == 1, "release and press again reports again");
    PPower::setResetButton(false);
}

std::vector<std::string> gShutdownLog;
int gSlowAttempts = 0;
BOOL slowHigh(BOOL final, u32 event) {
    gShutdownLog.push_back(std::string(final ? "F" : "n") + "high" + std::to_string(event));
    if (!final && ++gSlowAttempts < 3) {
        return FALSE;  // e.g. waiting for an async NAND close
    }
    return TRUE;
}
BOOL lowA(BOOL final, u32 event) {
    if (final) {
        check(OSDisableInterrupts() == FALSE, "final shutdown pass runs with interrupts disabled");
    }
    gShutdownLog.push_back(std::string(final ? "F" : "n") + "lowA");
    return TRUE;
}
BOOL lowB(BOOL final, u32) {
    gShutdownLog.push_back(std::string(final ? "F" : "n") + "lowB");
    return TRUE;
}

void testShutdownFunctions() {
    static OSShutdownFunctionInfo a{lowA, 200, nullptr, nullptr};
    static OSShutdownFunctionInfo b{lowB, 200, nullptr, nullptr};
    static OSShutdownFunctionInfo high{slowHigh, 100, nullptr, nullptr};
    OSRegisterShutdownFunction(&a);
    OSRegisterShutdownFunction(&b);
    OSRegisterShutdownFunction(&high);

    int pipefd[2];
    check(::pipe(pipefd) == 0, "pipe");
    const int status = inChild([&] {
        PPower::setExitHandler(
            [](const PPower::Exit& exit, void* user) {
                std::string log;
                for (const auto& e : gShutdownLog) {
                    log += e + ",";
                }
                log += "intent" + std::to_string(static_cast<int>(exit.intent)) + ",code" + std::to_string(exit.resetCode) + ",event" +
                       std::to_string(exit.shutdownEvent);
                const int fd = *static_cast<int*>(user);
                ssize_t written = ::write(fd, log.data(), log.size());
                (void)written;
                ::_exit(33);
            },
            &pipefd[1]);
        OSRestart(7);
    });
    ::close(pipefd[1]);
    char buf[512] = {};
    const ssize_t n = ::read(pipefd[0], buf, sizeof(buf) - 1);
    ::close(pipefd[0]);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 33 && n > 0, "exit handler receives control and does not return");
    const std::string expected = "nhigh4,nhigh4,nhigh4,nlowA,nlowB,Fhigh4,FlowA,FlowB,intent1,code7,event4";
    check(std::string(buf) == expected,
          "shutdown functions: priority order, lower priorities wait for higher ones, repeated until all succeed, then a final pass");
}

void testExitIntents() {
    struct Case {
        std::function<void()> call;
        int intent;
        int event;
    } cases[] = {
        {[] { OSShutdownSystem(); }, static_cast<int>(PPower::Intent::Shutdown), 2},
        {[] { OSReturnToMenu(); }, static_cast<int>(PPower::Intent::ReturnToMenu), 5},
        {[] { OSRebootSystem(); }, static_cast<int>(PPower::Intent::Reboot), 1},
    };
    for (const auto& c : cases) {
        const int status = inChild([&] {
            PPower::setExitHandler([](const PPower::Exit& exit, void*) { ::_exit(10 + static_cast<int>(exit.intent) * 10 + static_cast<int>(exit.shutdownEvent)); },
                                   nullptr);
            c.call();
        });
        check(WIFEXITED(status) && WEXITSTATUS(status) == 10 + c.intent * 10 + c.event, "each exit function delivers its intent and event");
    }
    // Defaults without a handler: the process ends normally.
    const int shutdown = inChild([] { OSShutdownSystem(); });
    check(WIFEXITED(shutdown) && WEXITSTATUS(shutdown) == 0, "default shutdown exits 0");
    const int menu = inChild([] { OSReturnToMenu(); });
    check(WIFEXITED(menu) && WEXITSTATUS(menu) == 0, "no Wii Menu: return to menu exits 0");
    // A handler that returns violates the no-return contract.
    const int returned = inChild([] {
        PPower::setExitHandler([](const PPower::Exit&, void*) {}, nullptr);
        OSShutdownSystem();
    });
    check(WIFSIGNALED(returned) && WTERMSIG(returned) == SIGABRT, "an exit handler that returns aborts");
}

void testRelaunch(const char* self) {
    check(OSGetResetCode() == 0 && !OSIsRestart(), "cold boot has reset code 0");
    char path[256];
    std::snprintf(path, sizeof(path), "%s/petari_power_relaunch_%d.txt", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", ::getpid());
    std::remove(path);
    setenv("PETARI_POWER_TEST_RESULT", path, 1);
    const int status = inChild([] { OSRestart(5); });  // default handler: relaunch this executable
    unsetenv("PETARI_POWER_TEST_RESULT");
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    std::remove(path);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "relaunched instance ran to completion");
    check(line == "restart 80000005", "OSRestart relaunches the application, which reports OSIsRestart and the reset code");
    (void)self;
}

}  // namespace

int main(int argc, char** argv) {
    // Relaunched instance of testRelaunch: report and leave.
    if (const char* result = std::getenv("PETARI_POWER_TEST_RESULT"); result && OSIsRestart()) {
        std::ofstream out(result);
        char code[16];
        std::snprintf(code, sizeof(code), "%x", OSGetResetCode());
        out << "restart " << code << "\n";
        return 0;
    }
    (void)argc;
    __OSThreadInit();
    PPCSync();
    OSRegisterVersion("<< petari platform power test >>");
    testCallbacks();
    testShutdownFunctions();
    testExitIntents();
    testRelaunch(argv[0]);
    OSReport("platform power tests passed (%d checks)\n", checks);
    return 0;
}

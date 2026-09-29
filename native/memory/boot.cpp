#include <petari/boot.hpp>
#include <petari/host_allocation.hpp>
#include <revolution/os.h>
#include <revolution/sc.h>
#include <mutex>
#include <pthread.h>

extern "C" void OSInit() {
    if (!pthread_main_np()) {
        OSPanic(__FILE__, __LINE__, "Native OSInit must run on the macOS main thread");
    }
    static std::once_flag initialized;
    PetariNative::HostAllocationScope hostAllocations;
    std::call_once(initialized, [] {
        OSGetArenaLo();
        OSGetMEM2ArenaLo();
        __OSThreadInit();
        PetariNative::setGameAllocationThread(true);
        SCInit();
    });
}

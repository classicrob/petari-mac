#pragma once
// Hang reports: the native platform's state when something stopped making
// progress (the smoke watchdog, the game's GX hang check). Output only; safe
// from any host or OS thread, with or without the OS interrupt lock held.

#include <cstdint>
#include <cstdio>

namespace PetariNative::Platform::Diagnostics {

// Every OS thread: scheduler state, what it waits on, its host thread's run
// state and CPU time, and a frame-pointer sample of its host stack; the CPU
// baton holder, run queue, scheduler/preemption flags and the interrupt lock
// holder; then the pending alarms (os_thread.cpp, os_alarm.cpp).
void dumpOS(std::FILE* out);

}  // namespace PetariNative::Platform::Diagnostics

extern "C" {

// GX sync state (GXSync::dumpState) and dumpOS, to stderr, framed by `reason`
// (gx_sync.cpp).
void petari_platform_dump_hang_state(const char* reason);

// The game's GX abort alarm (src/Game/System/MainLoopFramework.cpp, which
// declares the same layout). Zero-initialise before a wait; call from the
// alarm handler at every expiry. `pipelineWait`: the renderer reports a
// blocking pipeline wait, for the message only. Returns 1 when the frame
// should be aborted (GXSync::checkWait's verdict), after printing why and
// dumping the platform state; otherwise 0, printing progress-free waits and
// dumping the state after 10 s without progress and every 10 s after that.
struct PetariGXWaitCheck {
    std::uint64_t processed;   // GXSync::WaitCheck
    std::uint64_t sinceNs;
    std::uint64_t reportedNs;  // last full dump during this stall, 0 if none
};
int petari_gx_wait_check(PetariGXWaitCheck* check, int pipelineWait);

}  // extern "C"

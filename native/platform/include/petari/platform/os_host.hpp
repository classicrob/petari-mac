#pragma once
// Host work on an OS thread without holding the CPU.
//
// The native OS runs one OS thread at a time (the CPU "baton"). An OS thread
// that must do long or blocking host work (the app's frame seam:
// aurora_end_frame, present, event polling) brackets it with these calls so
// other ready OS threads (audio, loaders, DrawSyncManager) run meanwhile.
//
// petari_os_begin_host_blocking: the calling OS thread gives up the CPU as if
//   it slept (state waiting, on a private queue). Its host thread keeps
//   running host code only: calling an OS function that would block or
//   schedule (OSSleepThread, OSSendMessage with OS_MESSAGE_BLOCK, ...) aborts.
//   Platform waits it makes (GX drain, VI, DVD) wait as a host thread does.
//   Must not be nested, called with interrupts disabled, or called from a
//   host (non-OS) thread.
// petari_os_end_host_blocking: the thread becomes ready at its priority (or
//   stays suspended if another thread suspended it meanwhile), waits for the
//   CPU, and returns holding it, with interrupts enabled.
//
// C linkage so the app can bind them without SDK headers.

#ifdef __cplusplus
extern "C" {
#endif

void petari_os_begin_host_blocking(void);
void petari_os_end_host_blocking(void);

// For host waits that may run anywhere (a lock the renderer's threads also
// take): gives up the CPU as petari_os_begin_host_blocking does and returns 1
// when the caller is an OS thread holding it with interrupts and the
// scheduler enabled and not already doing host work; otherwise does nothing
// and returns 0 (host threads, interrupt context, disabled regions, nested
// use). End with petari_os_end_host_blocking only when it returned 1.
int petari_os_try_begin_host_blocking(void);

// A preemption point for game loops that poll without OS calls (they would
// otherwise hold the CPU until the baton monitor preempts them): delivers a
// pending preemption at once. Callable from game code on an OS thread; a no-op
// otherwise.
void petari_os_preemption_point(void);

#ifdef __cplusplus
}
#endif

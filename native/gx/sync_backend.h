#pragma once
/* C ABI between the Aurora renderer and Petari's GX synchronisation.
 *
 * Plain C with <stdint.h> only, so it can be included both from Aurora
 * translation units (dolphin/ headers) and from SDK-side ones (revolution/
 * headers) without type collisions.
 *
 * - petari_gx_sync_*: implemented by sync_backend.cpp on top of
 *   petari/platform/gx_sync.hpp. The processor calls are made by the patched
 *   Aurora fifo.cpp (patch_aurora_sync.py) on the FIFO processor thread,
 *   never under fifo.cpp's sBufferMutex. The snapshot calls are for the
 *   renderer's token-time EFB capture.
 * - petari_aurora_*: implemented by the patched Aurora sources, used by the
 *   public GX wrappers in sync_bridge.cpp.
 *
 * Stream positions are 64-bit byte offsets into Aurora's FIFO stream
 * (sStreamBase + offset). They only increase. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Processor side (patched fifo.cpp, FIFO processor thread) ---- */

/* Bytes in [processed, returned position) may be processed. A return equal to
 * `processed` means the processor is halted at the FIFO breakpoint; it must
 * wait for petari_aurora_fifo_wake(). */
uint64_t petari_gx_sync_process_limit(uint64_t processed, uint64_t target);

/* After each process() call, at a command boundary. `sync_bp` is the raw
 * value of the BP write that ended the call (draw done 0x45, PE token 0x47,
 * PE token interrupt 0x48), or 0 if the call ended at the end of its range.
 * `published` is the stream position made available to the processor. A
 * token interrupt runs the snapshot hook from here. */
void petari_gx_sync_processed(uint64_t processed, uint64_t published, uint32_t sync_bp);

/* The processor skipped the stream discarded by GXAbortFrame. */
void petari_gx_sync_abort_applied(uint64_t processed, uint64_t published);

/* fifo::drain's wait: until `position` has been processed. Game OS threads
 * sleep and yield the CPU baton; host threads block. */
void petari_gx_sync_wait_processed(uint64_t position);

/* ---- Game-thread waits for the processor's locks (patched fifo.cpp) ----
 *
 * A game OS thread that must wait for a lock the processor or renderer
 * threads can hold (sBufferMutex) gives up the OS CPU for the wait, so other
 * game threads (audio) keep running. begin returns 1 when it released the CPU
 * (then call end once the host work is done, with no Aurora lock held),
 * 0 when the caller cannot release it (host thread, interrupts or scheduler
 * disabled, already in host work): it then waits holding it, as before. */
int petari_gx_sync_begin_host_wait(void);
void petari_gx_sync_end_host_wait(void);

/* ---- Token-time snapshot contract (renderer <-> platform) ----
 *
 * Installed by the renderer. Called on the FIFO processor thread right after
 * a PE token interrupt BP (GXSetDrawSync) is processed and before any later
 * command is processed, outside sBufferMutex and with no platform lock held.
 * The renderer captures the EFB for `ticket` there. The token is not reported
 * to the game until petari_gx_sync_snapshot_ready(ticket).
 *
 * Tickets start at 1, increase in stream order, and are never reused. With
 * no hook installed, tokens are delivered on processing and no token-time
 * capture exists (GXPeekZ at tokens is unavailable, not approximated). */
typedef void (*PetariGXSnapshotHook)(uint64_t ticket, uint16_t token, uint64_t stream_position, void* user);
void petari_gx_sync_set_snapshot_hook(PetariGXSnapshotHook hook, void* user);

/* From any thread, once the capture for `ticket` is readable (or known to be
 * unavailable). Tickets may complete in any order; delivery to the game stays
 * in stream order: later tokens, draw dones, and breakpoint hits are held,
 * never dropped, until earlier tickets complete. The processor itself is
 * never blocked by an incomplete ticket. Every ticket must be completed. */
void petari_gx_sync_snapshot_ready(uint64_t ticket);

/* Optional. Called on the FIFO processor thread, at a command boundary and
 * outside sBufferMutex, when a draw done or a breakpoint hit is queued behind
 * an incomplete ticket: the game may be waiting on an event that depends on
 * that capture completing without aurora_end_frame. Covers every incomplete
 * ticket <= `ticket`; at most once per ticket. */
typedef void (*PetariGXFlushHook)(uint64_t ticket, void* user);
void petari_gx_sync_set_flush_hook(PetariGXFlushHook hook, void* user);

/* For GXPeekZ/GXPeekARGB: on the thread delivering a token callback, that
 * token's ticket; 0 outside token callbacks. */
uint64_t petari_gx_sync_delivering_ticket(void);

/* Oldest ticket whose capture is incomplete (GP events are held behind it),
 * or 0. For the hang alarm: the processor can be idle while GXDrawDone waits
 * only for this capture, which is progress, not a GP hang. */
uint64_t petari_gx_sync_held_ticket(void);

/* The last ticket whose callback has returned. Deliveries are in ticket
 * order, so captures up to it can no longer be read by the game. */
uint64_t petari_gx_sync_delivered_ticket(void);

/* ---- Aurora side (patched fifo.cpp / GXFifo.cpp) ---- */

uint64_t petari_aurora_fifo_write_position(void);     /* writer thread: sStreamBase + buffered bytes */
uint64_t petari_aurora_fifo_processed_position(void);
void petari_aurora_fifo_publish(void);                /* Aurora's publish(): only inside a frame */
void petari_aurora_fifo_publish_all(void);            /* everything written, as fifo::drain publishes */
void petari_aurora_fifo_abort(void);                  /* discard everything written but not processed */
void petari_aurora_fifo_wake(void);                   /* breakpoint changed: re-evaluate the limit */
const void* petari_aurora_cpu_fifo(void);             /* GXFifoObj last passed to GXSetCPUFifo, or null */
const void* petari_aurora_gp_fifo(void);              /* GXFifoObj last passed to GXSetGPFifo, or null */

#ifdef __cplusplus
}
#endif

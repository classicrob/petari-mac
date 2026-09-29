#pragma once
// GX synchronisation between game threads and the native graphics processor
// (GP): draw-sync tokens, draw done, FIFO breakpoints, abort, and GP status.
// See native/platform/GX_SYNC_PLAN.md.
//
// Division of work:
// - The renderer (root's native/gx and Aurora patches) owns the FIFO stream.
//   Its public GX wrappers (GXSetDrawSync, GXDrawDone, GXEnableBreakPt,
//   GXGetFifoPtrs, ...) write commands and call the game-side functions
//   below. Its command processor calls the report* functions from its own
//   thread as it processes the stream.
// - This platform component turns reports into GP interrupts delivered in
//   stream order on a GP interrupt thread with the OS interrupt lock held,
//   keeps the token register and callbacks, holds the breakpoint, and
//   implements waits that yield the OS CPU baton.
//
// FIFO positions are 64-bit byte offsets into the stream. Game code sees them
// as opaque pointer-width values (positionToPointer) that it never
// dereferences; they are at least 4 GiB, so DrawSyncManager's
// pointer-versus-token test stays correct.

#include <cstdint>
#include <cstdio>

#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXManage.h>

namespace PetariNative::Platform::GXSync {

inline constexpr std::uint64_t kNoBreakpoint = UINT64_MAX;

// ---- FIFO positions as game-visible pointers ----
void* positionToPointer(std::uint64_t position);
std::uint64_t pointerToPosition(const void* pointer);  // aborts on a value not made by positionToPointer

// ---- Renderer -> platform (command-processor thread). Never block on OS
// locks. Positions must not decrease across reports: events are delivered in
// report order, which must be stream order. Nothing is dropped. ----
void reportToken(std::uint16_t token, std::uint64_t position);  // PE token interrupt BP (0x48) processed
void setTokenRegister(std::uint16_t token);                     // PE token BP without interrupt (0x47) processed
void reportDrawDone(std::uint64_t position);                     // draw-done BP processed
void reportBreakpointReached(std::uint64_t position);           // processing halted at the breakpoint (deduplicated)
void reportProgress(std::uint64_t processed, std::uint64_t written);  // stream positions, for GP status and waits

// Platform -> renderer: the processor must not process bytes at or beyond
// this position (kNoBreakpoint when none).
std::uint64_t breakpointPosition();
// The processor may process [processed, returned position). A return equal
// to `processed` means it is halted at the breakpoint (reported here, once);
// it must wait for the renderer's wake, which the GX wrappers issue after
// every breakpoint change and abort.
std::uint64_t processLimit(std::uint64_t processed, std::uint64_t target);
// The processor skipped the stream discarded by GXAbortFrame and is now at
// `processed`.
void abortApplied(std::uint64_t processed, std::uint64_t written);

// ---- Token-time snapshots (renderer readback) ----
// With a snapshot hook installed, every processed token interrupt gets a
// ticket (1, 2, ... in stream order). The hook runs on the processor thread
// right after the token is processed and before any later command, with no
// platform lock held; the renderer captures the EFB there. That token's
// interrupt, and every later token interrupt, is delivered only after
// snapshotReady(ticket), from any thread; draw-done and breakpoint interrupts
// are not held back by it (see waitTokensDelivered). The processor is never blocked by
// an incomplete ticket. Every ticket must eventually be completed, including
// across GXAbortFrame (a capture may complete as "unavailable").
//
// Without a hook, tokens get ticket 0 and are delivered on processing: no
// token-time capture exists then, so GXPeekZ fidelity at tokens is
// unavailable, not approximated.
using SnapshotHook = void (*)(std::uint64_t ticket, std::uint16_t token, std::uint64_t position, void* user);
void setSnapshotHook(SnapshotHook hook, void* user);
void snapshotReady(std::uint64_t ticket);
// Optional. Called on the processor thread, no platform lock held, when a
// draw done or a breakpoint hit is queued behind an incomplete ticket: the
// game may be waiting for an event that depends on that capture. Covers every
// incomplete ticket <= `ticket`. At most once per ticket.
using FlushHook = void (*)(std::uint64_t ticket, void* user);
void setFlushHook(FlushHook hook, void* user);
// On the calling thread inside a token callback: that token's ticket; else 0.
std::uint64_t deliveringTicket();
// Oldest ticket whose capture is not complete yet (events are held behind
// it), or 0. For hang detection: the processor may be idle while a draw done
// waits only for this capture.
std::uint64_t heldTicket();
// Last ticket whose callback has returned. Deliveries are in ticket order, so
// every capture up to this one is no longer readable by the game.
std::uint64_t deliveredTicket();

// ---- Game side (called by the renderer's GX wrappers) ----
void setBreakpoint(std::uint64_t position);  // GXEnableBreakPt
void clearBreakpoint();                      // GXDisableBreakPt
// GXSetDrawDone: call after writing the draw-done BP; returns the draw-done
// count to wait for.
std::uint64_t noteDrawDoneIssued();
// Frame boundary (GameSystem::frameLoop, after the retrace wait): waits until
// every draw-sync token reported so far has been delivered, so the next
// frame's game code sees all of the previous frame's token callbacks (draw
// done no longer waits for token-time EFB captures). OS threads give up the
// CPU while waiting. Reports after 1 s; gives up after kTokenWaitGiveUpSeconds.
void waitTokensDelivered();
constexpr int kTokenWaitGiveUpSeconds = 10;
// GXDrawDone/GXWaitDrawDone: waits until that many draw-done interrupts have
// been delivered. OS threads sleep (other game threads run); host threads wait
// on a condition variable. Aborts if called from interrupt context.
void waitDrawDone(std::uint64_t count);
// True while an OS or host thread waits in waitDrawDone.
bool drawDoneWaitPending();
// Waits until the processor has processed up to `position` (Aurora's
// fifo::drain). Yields the OS CPU baton like waitDrawDone.
void waitProcessed(std::uint64_t position);
// GXAbortFrame: clears the breakpoint and records the draw-done BPs issued so
// far. When the processor applies the discard (abortApplied), those not yet
// processed are lost; the next delivered draw done also completes their
// waits, as the SDK's single draw-done flag would. Interrupts already reported
// are still delivered (they happened). Safe in interrupt context.
void abortFrame();

GXDrawSyncCallback setDrawSyncCallback(GXDrawSyncCallback callback);
GXDrawDoneCallback setDrawDoneCallback(GXDrawDoneCallback callback);
GXBreakPtCallback setBreakpointCallback(GXBreakPtCallback callback);
std::uint16_t lastToken();  // GXReadDrawSync: last token the GP processed

// Actual processor state for GXGetGPStatus. FIFO watermark flags (overhi,
// underlow) are not modelled and report false.
struct GPStatus {
    bool readIdle;     // processed == written
    bool commandIdle;  // same: no unprocessed commands
    bool breakpoint;   // halted at the breakpoint
};
GPStatus gpStatus();

// Last processed stream position reported by the renderer.
std::uint64_t processedPosition();

// ---- Hang check for GXDrawDone waits (the game's GX abort alarm) ----
// On the Wii, MainLoopFramework's alarm aborts the frame when a GXDrawDone
// wait sees no GP progress for 0.5 s. Natively the processor reports progress
// only at command-batch boundaries, and one batch can block for many seconds
// while first-use pipelines compile (serially, with short gaps between them),
// so an unchanged position is not evidence of a hang. checkWait classifies
// why no progress was seen. Only a stall nothing else can end asks for the
// abort: the processor halted at the breakpoint, or idle with a draw done
// outstanding, for kWaitAbortAfterNs while no game code can run (no OS thread
// holds or waits for the CPU, none is in host work) to move the breakpoint.
enum class WaitState {
    Progress,    // the processed position moved since the last check
    Done,        // every issued draw done was delivered; the waiter has not run yet
    Busy,        // unprocessed commands, not at the breakpoint: the processor is inside a batch
    Delivering,  // processed events not yet delivered (GP interrupt thread, or held behind a capture)
    Halted,      // halted at the breakpoint with commands pending
    Idle,        // nothing left to process or deliver, yet a draw done is outstanding
};
inline constexpr std::uint64_t kWaitAbortAfterNs = 1000000000;
struct WaitCheck {
    std::uint64_t processed = 0;  // position at the last check
    std::uint64_t sinceNs = 0;    // when it last moved; 0: no check yet (the first one counts as progress)
};
struct WaitVerdict {
    WaitState state;
    std::uint64_t stalledNs;  // since the position last moved
    bool abort;
};
// Takes the interrupt lock (reentrant: the alarm handler holds it). nowNs is
// any nonzero monotonic nanosecond clock, the same for every call on one
// WaitCheck.
WaitVerdict checkWait(WaitCheck& check, std::uint64_t nowNs);
const char* waitStateName(WaitState state);

// Hang report: positions, breakpoint, queued GP events, draw-done and ticket
// counts. Never blocks for long: if the state lock stays unavailable for two
// seconds the state is read without it, and the report says so.
void dumpState(std::FILE* out);

// Stops the GP interrupt thread and resets all state; undelivered events
// (including ones waiting for a snapshot) are discarded. For tests and
// shutdown. Must not be called with interrupts disabled: delivery takes the
// interrupt lock, so the join could never finish (aborts instead).
void shutdown();

}  // namespace PetariNative::Platform::GXSync

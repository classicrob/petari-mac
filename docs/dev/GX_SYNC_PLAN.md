# GX draw sync, draw done, and FIFO breakpoints: native plan

Status: implemented, not yet wired into the root build. Platform:
`native/platform/gx_sync`. FIFO/processor side and public wrappers:
`native/gx/patch_aurora_sync.py`, `sync_backend.{h,cpp}`, `sync_bridge.cpp`.
Tests: `native/tests/platform_gx_sync_tests.cpp` and
`native/tests/gx_sync_backend_tests.cpp` (fake Aurora FIFO). Root owns the
token-time EFB capture (`native/gx/efb_snapshot.*`), `GXPeekZ`/`GXPeekARGB`,
frame submission, and the CMake wiring. The sections below keep the original
analysis; "Implementation" and "Token-time snapshots" describe what was built.

## What the game relies on

Sources: `Game/System/DrawSyncManager.cpp`, `MainLoopFramework.cpp`,
`JUTVideo.cpp`, `StarPointerDirector.cpp`, `TalkDirector.cpp`, `LensFlare.cpp`,
and `MarioActorSpecialDraw.cpp`.

1. **Draw-sync tokens with depth reads.** StarPointer, TalkPeekZ, and
   LensFlare reserve token ranges (`DrawSyncManager::setCallback`). Each then:
   - calls `pushBreakPoint()`, which does `GXFlush`, `GXGetCPUFifo`, and
     `GXGetFifoPtrs` and posts the FIFO write pointer to the DrawSyncManager
     thread;
   - issues `GXSetDrawSync(token)`.

   When the graphics processor (GP) reaches the token, the PE-token interrupt
   calls `GXSetDrawSyncCallback`'s callback in interrupt context. That
   dispatches to the owner, which calls `GXPeekZ`: the EFB depth *at that
   point of the frame* (pointer occlusion, speech-bubble placement, lens-flare
   visibility). The manager then posts the token.
2. **Frame pipelining with FIFO breakpoints.** The DrawSyncManager thread
   (priority 15) keeps a ring of posted write pointers:
   - when two are pending, it calls `GXEnableBreakPt(ptr)`;
   - each token retires one and moves the breakpoint to the next;
   - it calls `GXDisableBreakPt` when one remains.

   The GP stops fetching at the breakpoint, so it cannot run past the next
   frame's peek point before the previous token's callback has read depth. The
   manager tells pointers from tokens **by value**: messages ≥ `0x80000000` are
   FIFO pointers, below `0x10000` tokens, and `0x10000` quits.
3. **Draw done.**
   - `GXDrawDone()` blocks until the GP has finished all prior commands. It is
     used in the main loop's single-buffer path, `exchangeXfb_double` without
     `_C`, `OdhConverter`, and `MarioActorSpecialDraw` (4×).
   - `GXSetDrawDone()` with `GXSetDrawDoneCallback` (JUTVideo) is the
     asynchronous form: the callback runs in interrupt context and unblocks the
     frame swap.
4. **Hang recovery.** `waitDrawDoneAndSetAlarm` arms a 0.5 s alarm around
   `GXDrawDone`. If the GP hangs, `handleGXAbortAlarm`:
   - reads `GXReadXfRasMetric` and `GXGetGPStatus`;
   - calls `GXDisableBreakPt` and `GXAbortFrame`;
   - clears the manager FIFO;
   - writes a raw draw-done BP (`0x61`, `0x5800000f`);
   - calls `GXSetDrawDone`.

## What Aurora provides today

Checked in `../aurora-reference` and the pinned revision.

- `GXGetCPUFifo()`/`GXGetGPFifo()` return pointers, which mismatches the
  SDK's out-parameter ABI. `GXGetFifoPtrs` returns null read and write
  pointers. `DrawSyncManager` would treat a null write pointer (0 < 0x10000)
  as a **token**, which is wrong.
- `GXEnableBreakPt`, `GXDisableBreakPt`, `GXSetBreakPtCallback`,
  `GXSetDrawSync`, `GXSetDrawSyncCallback`, `GXReadDrawSync`, and
  `GXAbortFrame` are TODO or absent.
- `GXDrawDone` writes the draw-done BP and calls `fifo::drain()`, which waits
  on `sProcessed.wait` until the FIFO worker has *decoded* the commands. That
  is CPU-side command processing, not GPU completion. The wait also holds the
  OS CPU baton, so no other game thread runs meanwhile.
- The draw-done callback runs on the Aurora FIFO worker thread without the
  OS interrupt lock. JUTVideo's callback uses OS primitives, which is a data
  race with game threads and outside the interrupt model.
- `GXPeekZ` returns the latest periodic depth snapshot (`depth_peek`,
  rate-limited), not the depth at the token's position.

## Proposed semantics

- **Stream positions.** The FIFO is a monotonically increasing 64-bit byte
  stream (Aurora's `sStreamBase + sBufferSize`). `GXGetFifoPtrs` returns the
  write position as an opaque pointer-width value:
  `kFifoPointerBase + position`, with `kFifoPointerBase` ≥ `0x1'0000'0000`.
  The game never dereferences these; it compares them and hands them back to
  `GXEnableBreakPt`. This keeps DrawSyncManager's pointer/token
  classification correct. The read pointer is the processed position.
- **Tokens.** `GXSetDrawSync(token)` writes the PE token BP into the stream.
  When the command processor reaches it, the token register (read by
  `GXReadDrawSync`) is updated and a *GP interrupt* is queued.
- **Breakpoints.** `GXEnableBreakPt(p)`: the command processor processes up
  to, but not past, `p` until the breakpoint moves or is disabled; reaching it
  raises the breakpoint callback. `GXDisableBreakPt()` releases it.
  `GXSetBreakPtCallback` registers the callback.
- **Draw done.**
  - `GXSetDrawDone()` writes the draw-done BP. When it is processed, a GP
    interrupt calls the draw-done callback.
  - `GXDrawDone()` is `GXSetDrawDone` plus a wait for that event.
  - "Done" means processed by the command processor and submitted to the
    renderer. That is today's Aurora meaning; GPU pixel completion is not
    waited for, see depth below.
- **Interrupt context.** The command processor never calls game code. It
  queues GP events (token N at position P; draw done at P; breakpoint reached
  at P) to a *GP interrupt thread* in the platform. That thread delivers them
  in stream order with the OS interrupt lock held, exactly as the AI, DSP, and
  VI interrupts do. Callbacks may use OS calls (`OSSendMessage` etc.) as on
  the console.
- **Baton-safe waiting.** `GXDrawDone` from an OS thread sleeps with
  `OSSleepThread` on a GP queue woken by the draw-done interrupt, so other
  game threads run while the GPU works. This also removes a deadlock: a game
  thread holding the interrupt lock while spinning in `fifo::drain` can block
  the worker's callback delivery. Host (non-OS) threads may wait on a
  condition variable.
- **Abort.** `GXAbortFrame` discards the unprocessed stream: the published
  position jumps to the write position without processing. It clears
  breakpoints and pending draw-done/token state, as a GP reset does. The game
  then re-arms with a raw draw-done BP, which must still be processed.
- **`GXPeekZ` at a token.** Depth "at the token" needs a depth copy at that
  point in the frame. Proposal: when the processor reaches a draw-sync token
  whose owner will peek, the renderer records a depth-copy marker at that pass
  position. The copy is read back asynchronously. The token's GP interrupt is
  delivered **when that readback is available**, so the callback's `GXPeekZ`
  reads the token-time depth. Because the breakpoint holds the processor
  before the next frame's peek point, this matches the game's pipelining; the
  cost is up to about one frame of callback latency. It is explicit and
  documented, and replaces today's rate-limited "latest snapshot" behaviour.
  Other tokens are delivered on processing.
- **Hang recovery.** With a native GP, `GXDrawDone` returns when the
  processor is done, so the 0.5 s abort alarm fires only if the processor or
  renderer really stalls. `GXReadXfRasMetric` and `GXGetGPStatus` report real
  processor state: idle/busy, breakpoint reached, and 0 for rasterizer
  counters. Nothing is invented; the counters are declared unsupported.
  `GXReadXfRasMetric`'s `clocks` is a **diagnostic processed-byte counter**
  (low 32 bits of FIFO bytes processed), not XF or GP clock cycles; it only
  shows whether the processor moved between two reads.

## Proposed ownership

| Area | Owner | Files |
|---|---|---|
| GP interrupt thread, event queue, delivery in interrupt context; token register; draw-done/draw-sync/breakpoint callback registry; baton-safe `GXDrawDone` waits; breakpoint state; tests | platform (this worker) | new `native/platform/gx_sync/*`, `include/petari/platform/gx_sync.hpp`, `native/tests/platform_gx_sync_tests.cpp` |
| Writing token and draw-done BPs; stream-position `GXGetCPUFifo`/`GXGetFifoPtrs` with SDK out-parameter ABI; command processor stopping at the breakpoint position; reporting token/draw-done/breakpoint events with positions through the platform hook; `GXAbortFrame` stream discard; token-time depth copy and readback | root (native/gx and Aurora patches) | native/gx, Aurora patch scripts |
| Public GX entry points (`GXSetDrawSync`, `GXReadDrawSync`, `GXSetDrawSyncCallback`, `GXEnableBreakPt`, `GXDisableBreakPt`, `GXSetBreakPtCallback`, `GXDrawDone`, `GXSetDrawDone`, `GXWaitDrawDone`, `GXSetDrawDoneCallback`, `GXAbortFrame`, `GXGetCPUFifo`, `GXGetFifoPtrs`) | root, as thin wrappers: they write FIFO commands, then call platform functions for waiting and state | native/gx |

Hook contract (platform side, sketch):

```cpp
namespace PetariNative::Platform::GXSync {
// Renderer -> platform, from the command-processor thread; never blocks on
// OS locks.
void reportToken(u16 token, u64 position);          // PE token processed
void reportDrawDone(u64 position);                   // draw-done BP processed
void reportBreakpointReached(u64 position);          // processor halted at breakpoint
// Platform -> renderer: current breakpoint (UINT64_MAX when disabled); the
// processor must not process bytes at or beyond it.
u64 breakpointPosition();
// Game-side operations used by root's GX wrappers.
void setBreakpoint(u64 position);                    // GXEnableBreakPt
void clearBreakpoint();                              // GXDisableBreakPt
void waitDrawDone();                                 // GXDrawDone wait, OS-baton safe
void abort();                                        // GXAbortFrame: drop pending events
GXDrawSyncCallback setDrawSyncCallback(GXDrawSyncCallback);
GXDrawDoneCallback setDrawDoneCallback(GXDrawDoneCallback);
GXBreakPtCallback setBreakpointCallback(GXBreakPtCallback);
u16 lastToken();                                     // GXReadDrawSync
}
```

## Tests to write (platform side, no GPU)

- A fake command processor thread consumes a synthetic stream and reports
  events:
  - tokens delivered in stream order, in interrupt context;
  - `GXReadDrawSync` tracks the last token;
  - draw-done callback after all prior tokens;
  - `GXDrawDone` from an OS thread sleeps (a lower-priority thread runs
    meanwhile) and wakes only after the event;
  - `GXDrawDone` from a host thread;
  - breakpoint halts processing until moved or disabled;
  - abort drops pending events;
  - no deadlock when a game thread disables interrupts around GX calls while
    the processor runs.
- DrawSyncManager replay: compile `Game/System/DrawSyncManager.cpp` against
  the platform with a fake renderer. Post frames and tokens and check that its
  breakpoint moves keep the processor at most one peek point ahead, and that
  every reserved token range reaches its callback.

## Open questions for root

1. Is a pointer-width opaque stream position acceptable for `GXGetFifoPtrs`
   (never dereferenced by game code), or does anything read FIFO memory
   through it? A search found no dereference in `src/Game` or `src/JSystem`.
2. Can Aurora's command processor stop mid-stream at an arbitrary byte
   position? A breakpoint is always a command boundary: a write pointer taken
   after `GXFlush`.
3. Depth copy at a token position: can the render worker insert a depth copy
   between passes at a recorded position, or does the frame need splitting?

## Implementation

| Area | Owner | Files |
|---|---|---|
| GP interrupt thread, ordered event queue, snapshot tickets, token register, callbacks, breakpoint state, baton-safe draw-done and processing waits, abort bookkeeping | platform worker | `native/platform/gx_sync/*`, `include/petari/platform/gx_sync.hpp` |
| Aurora FIFO patch: breakpoint limit, stop at sync BPs, reports outside `sBufferMutex`, abort discard, baton-safe `fifo::drain`; removal of Aurora's sync entry points and `GXPeekZ` | platform worker | `native/gx/patch_aurora_sync.py` |
| C ABI between Aurora and SDK headers | platform worker | `native/gx/sync_backend.h`, `sync_backend.cpp` |
| Public SDK entry points: `GXSetDrawSync`, `GXReadDrawSync`, `GXSetDrawSyncCallback`, `GXSetDrawDone`, `GXDrawDone`, `GXWaitDrawDone`, `GXSetDrawDoneCallback`, `GXEnableBreakPt`, `GXDisableBreakPt`, `GXSetBreakPtCallback`, `GXAbortFrame`, `GXGetCPUFifo`/`GXGetGPFifo` (SDK `GXBool f(GXFifoObj*)`), `GXGetFifoPtrs`, `GXGetGPStatus` | platform worker | `native/gx/sync_bridge.cpp` |
| `GXReadXfRasMetric` (diagnostic byte counter) | platform worker | `native/platform/gx_sync/gx_sync.cpp` |
| Token-time EFB capture, `GXPeekZ`/`GXPeekARGB`, frame submission, CMake wiring | root | `native/gx/efb_snapshot.*`, native/gx CMake |

Patch composition: `patch_aurora_allocations.py` first (fifo.cpp,
GXManage.cpp), then `patch_aurora_sync.py` on its output; command_processor.cpp,
GXFifo.cpp and GXCpu2Efb.cpp are patched from the originals. Patched files
include `"sync_backend.h"`, so aurora_gx needs `native/gx` on its include
path. Checked with `-fsyntax-only` against the pinned Aurora revision
(08122911e8621acb7ded6563813b264bec1494b5).

Behaviour:

- **Command boundaries.** `process()` returns after every draw-done (0x45),
  PE token (0x47) and PE token interrupt (0x48) BP. fifo.cpp reports it at
  that boundary, after releasing `sBufferMutex`, before any later command.
  0x48 raises the token interrupt; 0x47 only sets the token register (SDK
  `GXSetDrawSync` writes 0x48 then 0x47).
- **Breakpoint.** Before each `process()` call the processor asks for its
  limit; it never processes at or past the breakpoint. Halting reports the
  breakpoint interrupt once. `GXEnableBreakPt`/`GXDisableBreakPt` wake the
  worker. While halted, GP status reports the pending bytes (not idle).
- **Draw done.** `GXSetDrawDone` keeps Aurora's gating (processing only
  inside a frame), except while a `GXDrawDone` is waiting: then it publishes
  everything, as `GXDrawDone` itself does (like Aurora's old `fifo::drain`),
  so handleGXAbortAlarm's recovery can complete. `GXDrawDone` and
  `fifo::drain` wait with the platform: game OS threads sleep and yield the
  CPU baton; host threads block. Neither holds a renderer lock while waiting.
- **Abort.** `GXAbortFrame` (callable from interrupt context) clears the
  breakpoint, records draw dones issued so far, and marks everything written
  as discarded. The processor stops at the next command boundary (it cannot
  interrupt a command in progress, unlike the hardware reset), skips the
  discarded bytes, and reports how many issued draw dones were lost. The next
  delivered draw done also completes their waits, as the SDK's single
  draw-done flag would. Deviation: the SDK also drops pending dirty shadow
  state (`gx->dirtyState = 0`); Aurora keeps it and sends it with the next
  draw.
- **FIFO objects and pointers.** `GXGetCPUFifo(GXFifoObj*)` copies the object
  last passed to `GXSetCPUFifo` and returns TRUE (FALSE if none).
  `GXGetFifoPtrs` returns opaque stream positions (≥ 4 GiB): read = processed,
  write = everything written. They are never dereferenced.

## Token-time snapshots and the end-of-frame deadlock

Aurora submits a frame's GPU work only in `aurora_end_frame`: one command
encoder and one mapped staging buffer per frame, with passes copying from
that buffer. Depth read back "at a token" therefore needs that part of the
frame submitted. If the token's interrupt waits for a readback that waits
for `aurora_end_frame`, two deadlocks follow:

1. Events are delivered in stream order, so a draw done after token N is held
   behind N. A game thread in `GXDrawDone` before `aurora_end_frame` (the
   single-buffer main loop, OdhConverter, MarioActorSpecialDraw ×4) waits
   forever.
2. DrawSyncManager puts the breakpoint at the next frame's peek point and
   moves it only when token N is delivered. The processor halts, so the next
   frame's end-of-frame drain never finishes.

The contract (`sync_backend.h`) removes the dependency:

- With a snapshot hook installed, each token interrupt gets a ticket (1, 2,
  ..., stream order, never reused). The hook runs on the processor thread at
  the token's command boundary, outside `sBufferMutex`. Root's `captureEfb`
  seals the pass, rotates the staging buffer, submits the segment
  immediately, and completes the ticket from the render worker once an
  immutable CPU snapshot exists. It blocks the processor only until the
  segment is submitted, never until a game callback runs.
- The platform holds that token's interrupt and every later event until the
  ticket completes. Tickets may complete out of order; delivery never
  reorders. The processor keeps going. Nothing is dropped.
- During the callback, `petari_gx_sync_delivering_ticket()` names the capture
  `GXPeekZ` must read. `petari_gx_sync_delivered_ticket()` says which captures
  may be retired.
- An optional flush hook is called on the processor thread when a draw done
  or a breakpoint hit is queued behind an incomplete ticket. Root's immediate
  submission does not need it. It is there if submission later becomes
  conditional, so that decision is made at a FIFO boundary, never from a
  game or wait thread.
- Without a hook, tokens are delivered on processing and no token-time
  capture exists: `GXPeekZ` fidelity at tokens is then unavailable, not
  approximated.

Remaining risk: capture completion must not depend on the game thread
(`aurora_end_frame`), or deadlock 1 or 2 returns. Every ticket must be
completed, including across `GXAbortFrame`; a capture may complete as
"unavailable".

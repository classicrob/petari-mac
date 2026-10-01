# Native game launch and frame loop: integration plan

Written for the port's integration work. Line references are to the tree as of
this plan. "Aurora" means the pinned `aurora-reference` checkout.

**Status.** §3 (start-up) and §4 (frame seam) are implemented in
`native/app/` and `petari/app.hpp`, with one native call in
`GameSystem::frameLoop`; see §9. Presentation (the GXCopyDisp image, VI
black and dimming, the image rectangle) and the CPU release are hook
contracts that root and the platform OS layer fill. Until they do, the app
says so at start-up instead of assuming them.

Not covered, because other workers own it: GX readbacks and the XFB/EFB copy
implementation (root), and FIFO draw-sync/draw-done/breakpoint semantics
(platform, `docs/dev/GX_SYNC_PLAN.md`). Where this plan depends on
them, it states the requirement and leaves the design to them.

## 1. What the game does (traced)

### Boot: `petari_game_main` (`src/Game/System/GameSystem.cpp:50`)

1. `OSInitFastCast`, `DVDInit`, `VIInit`, root heaps, `LytInit`,
   `FileRipper::setup`, `GameSystemException::init`, acos table.
2. `GameSystem::init` (`:85`):
   - `JKRAram::create`.
   - `new GameSystemObjHolder`: `initDvd`, `initNAND`, and mounting the
     embedded error archive.
   - Font from embedded data.
   - **`initGX` → `GXInit(fifo)`** (`:164`).
   - **`DrawSyncManager::start(0x300, 15)`**: creates an OS thread at
     priority 15 and sets the draw-sync callback.
   - **`mObjHolder->init()`** (`GameSystemObjHolder.cpp:65`):
     - `initAudio` (resource request only);
     - **`initRenderMode` → `JUTVideo::createManager`** (`JUTVideo.cpp:27`):
       `VIInit`, `setRenderMode` → **`VIConfigure`**, `VIFlush`,
       **`VIWaitForRetrace` ×2** (the constructor starts with
       `mSetBlack = true`), `VISetBlack(1)`, and installs the pre/post
       retrace callbacks and `GXSetDrawDoneCallback`;
     - message resources, `FunctionAsyncExecutor`, `WPadHolder`
       (`KPADInit` → `WPADInit`), `StarPointerDirector`;
     - **`initDisplay`**: **three XFBs**, so `MainLoopFramework` uses
       *triple buffering*.
   - Scene controller, error watcher, `GameSystemResetAndPowerProcess`
     (`OSSetPowerCallback`), `HomeButtonLayout`.
3. `while (true) frameLoop();`. Audio is created later, asynchronously, in
   `exeInitializeAudio` on a `FunctionAsyncExecutor` thread (priority 14).
   That thread ends up in `AIInit`/`AIStartDMA`.

The window and GX backend must therefore exist **before** `petari_game_main`:
- `GXInit`, `VIConfigure` (whose `onConfigure` hook calls
  `aurora::vi::configure`), and GX texture/DL setup all run inside
  `GameSystem::init`.
- `VIWaitForRetrace` runs there too, before any frame.

### One frame: `GameSystem::frameLoop` (`GameSystem.cpp:198`)

| Step | Code | GX / VI effects |
| --- | --- | --- |
| `beginRender` | `MainLoopFramework.cpp:233` | `exchangeXfb_triple` (index bookkeeping, `callDirectDraw`), `clearEfb` (quad draw), `preGX` |
| `draw()` | `GameSystem.cpp:209` | Scene, sequences, star pointer, `ScreenPreserver::draw`, wipes, error watcher, **`HomeButtonLayout::draw` → `HBMDraw`**, reset/power fade. `DrawSyncManager::pushBreakPoint` from TalkDirector, LensFlare, StarPointerDirector (`GXFlush`, `GXGetFifoPtrs`, message to the DrawSync thread). |
| `endRender` | `:270` | `endGX` (console, `GXFlush`), **`copyXfb_triple` → `GXCopyDisp(drawingXfb, GX_FALSE)`, `GXPixModeSync`** |
| `update()`, `calcAnim()` | | CPU work overlapping the GP. `WPadHolder::update` reads KPAD here. |
| `captureIfAllowForScreenPreserver` | | May copy the EFB (screen preserver). |
| `endFrame` | `:288` | **`waitDrawDoneAndSetAlarm`**: arms a 0.5 s OS alarm, then `GXDrawDone()`. If the alarm fires, `handleGXAbortAlarm` runs `GXAbortFrame`, clears the DrawSync FIFO, writes a raw draw-done, and calls `DrawSyncManager::prepareReset` (`:471`, `:493`). Then `GXFlush`. |
| `waitForRetrace` | `:228` | **`waitForTick`**: `OSReceiveMessage(JUTVideo queue, BLOCK)` until the retrace count posted by `JUTVideo::postRetraceProc` reaches the target (`:436`). |

The retrace callbacks run on the VI interrupt thread
(`JUTVideo.cpp:54`, `:162`). With three buffers, `preRetraceProc` only calls:
- `VISetNextFrameBuffer(displaying XFB)`, `VIFlush`, `VISetBlack(FALSE)`;
- or `VISetBlack(TRUE)` during the first 2 black frames (`mSetBlack`) or
  when no XFB manager exists.

It makes **no GX calls in triple-buffer mode**; the `GXCopyDisp` in the
single-buffer branch is dead here. `drawDoneCallback` (`:142`) only clears a
flag with three buffers.

Aurora today (upstream):
- `GXCopyDisp` is a no-op, and the present shows the EFB as it stands at
  `aurora_end_frame`.
- `GXDrawDone` = `GXFlush` + a draw-done BP + `fifo::drain()`.
- `fifo::publish()` and `finish_draw()` do nothing while no Aurora frame is
  active (`lib/gx/fifo.cpp:170`).
- `aurora_end_frame` drains the whole FIFO (`drain()` waits for
  `sProcessed >= written`).

### Reset and exit

- The platform's reset button or `pressPowerButton` makes
  `GameSystemResetAndPowerProcess` fade out. `exePrepareReset` (`:116`)
  calls `forceToDeactivateHomeButtonLayout`, audio reset, and NWC24.
- `exeReset` (`:150`): `DVDCheckDiskAsync` → for anything but
  ApplicationReset, `exitApplication` (`:202`):
  `DrawSyncManager::end` (joins its thread), then
  `MainLoopFramework::setForOSResetSystem` (cancels alarms,
  `VISetBlack(TRUE)`, `VIFlush`, **`VIWaitForRetrace`**), then
  `OSRestart`/`OSReturnToMenu`/`OSShutdownSystem`.
- The platform power layer runs shutdown functions, then calls the app's
  exit handler **with interrupts disabled** (`native/platform/os/os_power.cpp:81-89`).
  The handler must not return.
- ApplicationReset (the Home menu's "Restart from Title", or a reset key)
  stays in-process and does not reinitialise GX.
- **`GXInit` runs again after a GX abort**, not on reset:
  1. `handleGXAbortAlarm` (`MainLoopFramework.cpp:493`) runs on the OS
     alarm thread while the main thread waits in `GXDrawDone`.
  2. It calls `GXAbortFrame`, writes a raw BP and a new draw-done, and calls
     `DrawSyncManager::prepareReset`.
  3. When `GXDrawDone` returns, `resetIfAborted` → `DrawSyncManager::reset`
     (`DrawSyncManager.cpp:156`) calls `GameSystem::initGX()` → `GXInit`.

  Aurora's `GXInit` (`lib/dolphin/gx/GXManage.cpp:16`) memsets the `__gx`
  shadow state and writes register initialisation into the stream. Its FIFO
  object functions are no-ops, and it leaves the FIFO buffer, worker, frame
  state and draw-done callback alone. §7 has the re-entry proposal for the
  FIFO owner.

## 2. Threading model

**Decision: the game runs on the macOS main thread, as the GX probe already
does.**
- `OSInit` must run there: it panics otherwise (`native/memory/boot.cpp`).
  It binds the main thread as the default OS thread.
- Aurora's window, SDL event pumping, and the Metal surface are main-thread
  APIs on macOS.
- The game writes GX only from its main loop. On the Wii, GX has a single
  writer too.

This forces all host work (Aurora frame, SDL events, ImGui) into
**cooperative points in the game's main thread**. Everything else follows
from that:

| Thread | Kind | Runs |
| --- | --- | --- |
| macOS main | default OS thread (holds the CPU baton while running game code) | game loop, all GX writes, Aurora frame/update, ImGui, SDL events |
| DrawSync (prio 15), FunctionAsyncExecutor (14), JKR loaders, JAudio, NWC24 | OS threads (baton) | game code; **must not write GX** (only `GXEnableBreakPt`/`GXDisableBreakPt`, which go to the platform's GXSync) |
| VI retrace, AI DMA, DSP, DVD drive, OS alarm, GP interrupt (platform) | host interrupt threads | platform callbacks with the interrupt lock held |
| Aurora FIFO worker, render worker, Dawn | host threads | command processing; report to GXSync without OS locks |
| SDL audio callback | host realtime | `Platform::Audio::pull` only |

Rules for root's code on the main thread:
- **Every Aurora/SDL/ImGui/Dawn call runs inside
  `PetariNative::HostAllocationScope`.** The main thread is a registered
  game allocation thread, so otherwise `operator new` uses the current JKR
  heap. The VI bridge already does this in `configure`.
- **Host work that can block (end_frame drain, surface acquire, present,
  paused event waits) runs with the OS CPU baton released.** Otherwise audio
  (JAudio thread), loaders, and the DrawSync thread starve. See D2/D3 in
  §6. The OS layer has no public API for this yet; it is requested from the
  platform worker in §7.
- Add a debug assertion in root's GX wrappers that the writer is the main
  thread (`pthread_main_np()`). This catches a GX write from a loader thread
  racing `aurora_end_frame`.

## 3. Startup sequence (`native/app/main.cpp`, root)

On the main thread, in this order:

1. `Platform::Crash::install(<user>/Crashes)` first: alternate signal stack,
   main thread.
2. Resolve paths:
   - data: `--data` or default `build/game-data/RMGE01/files`;
   - user dir: `~/Library/Application Support/Petari`.
3. `Platform::DVD::mount(...)`: the disc must be mounted before
   `DVDInit`/FST use in `petari_game_main`.
4. `Platform::NAND::mount(<user>/NAND)`: before the game's first NAND call.
   Without a mount, `NANDInit` fails like a console without a file system.
5. `Platform::SC::setStore(<user>/settings.txt)`: **before `OSInit`**,
   because `OSInit` calls `SCInit` (`boot.cpp`).
6. Input: load bindings (`Input::Bindings::parse`) and settings. Keep the
   default `Clock::Alarm`.
7. `aurora_initialize` with:
   - `desiredBackend = BACKEND_METAL`, `pauseOnFocusLost = false`
     (see D5), `mem1Size = mem2Size = 0` (the platform owns the arenas);
   - `vsync`: see §4 "Pacing";
   - `userPath`/`cachePath` = user dir;
   - `logCallback`.

   The window is shown inside `aurora_initialize` (`lib/aurora.cpp:174`).
   This runs before `OSInit`, while the main thread is not yet a game
   allocation thread, so no scope is needed.
8. `OSInit()`: binds the main thread and runs `SCInit`.
9. VI: keep `Clock::Internal` (see D4), then `petari_attach_vi_renderer()`.
10. GX sync: whatever attach call the FIFO/GXSync work defines.
11. Audio: `SDL_InitSubSystem(SDL_INIT_AUDIO)` here, inside a scope. The
    sink's `start` also calls it, but on the FunctionAsyncExecutor thread; a
    refcounted pre-init keeps the first init on the main thread. Then
    `AudioSDL::install()` (before `AIStartDMA`).
12. `Platform::Power::setExitHandler(exitHandler)` (§5).
13. **Open the first Aurora frame:** `frameOpen()` from §4 (loop
    `aurora_update` / `aurora_begin_frame` until it succeeds). `GXInit`,
    boot GX, and any boot-time GX wait then run inside an active frame
    (D1).
14. `petari_game_main()`: it never returns.

## 4. The frame seam: exact hook

Add one native-only call in `GameSystem::frameLoop`, between `endFrame()` and
`waitForRetrace()` (`GameSystem.cpp:205-206`):

```cpp
    MainLoopFramework::sManager->endFrame();
#ifdef PETARI_NATIVE
    petari_host_frame_seam();
#endif
    MainLoopFramework::sManager->waitForRetrace();
```

Why here:
- The frame's `GXCopyDisp` has been issued, and its `GXDrawDone` has
  returned, so the stream is fully processed: `end_frame`'s drain cannot
  wait on a breakpoint.
- Everything the game draws for frame N is inside Aurora frame N, including
  `HBMDraw`, so the Home menu overlay matches that frame.
- The retrace wait that follows still paces the game at the VI rate.

`petari_host_frame_seam()` lives in `native/app/frame_seam.cpp`:

```text
HostAllocationScope; ReleaseCpu (baton released for the whole seam)
  HomeMenu::drawImGuiOverlay(image rect in ImGui/window points)
  aurora_end_frame()                     // drain (already processed), encode, present
  events = aurora_update()               // SDL poll; imgui/rmlui see events
  for each event:
    AURORA_EXIT            -> Power::pressPowerButton() once; a second request,
                              or no exit within ~5 s, force-quits (_Exit) after
                              stopping the audio device
    AURORA_SDL_EVENT       -> Input::SDL3::handleEvent(ev.sdl)
    WINDOW_RESIZED / DISPLAY_SCALE_CHANGED
                           -> recompute the present rect; Input::setViewport(rect)
    focus lost/gained (SDL window events)
                           -> Input::focusChanged(...)
  frameOpen():  while (!aurora_begin_frame()) { aurora_update(); handle events as above }
```

Notes:
- **The overlay goes before `aurora_end_frame`.** ImGui's frame is opened by
  `aurora_begin_frame` (`imgui::new_frame`) and frozen in `end_frame`.
- **Present rect.** Input (`Input::Viewport`) and the overlay need the same
  image rectangle Aurora presents into
  (`webgpu::calculate_present_viewport` over the present source size). Root
  should export it from its present code, in window points, and derive
  `Input::Viewport` from it. The image aspect follows the latched VI render
  mode and the SC aspect setting (16:9 by default).
- **What is presented.** Aurora presents at the seam, after `endFrame`.
  Between `GXCopyDisp` (endRender) and the seam, the game may still touch
  the EFB (`captureIfAllowForScreenPreserver`). Root's readback/XFB work
  should present the image snapshotted at `GXCopyDisp` (the XFB), not the EFB
  at `end_frame`. It must also honour `VI::displayState().black` and
  `.dimmed`: boot shows 2 black frames, reset/exit shows black, and screen
  dimming darkens the image.
- **Pacing.** The game paces itself on VI retraces (Internal clock,
  59.94 Hz). With `vsync = true` (FIFO present), the seam's present may also
  block for a display refresh. Stacked on `waitForTick`, that can halve the
  frame rate or cause judder. Use `vsync = false` or a mailbox present mode
  at first, and measure. Driving VI from the display link (External clock)
  would run the game at 120 Hz on ProMotion displays, and deadlocks at boot
  (D4).
- **Not presentable (minimised, hidden).** `frameOpen` loops, and the game
  thread waits in the seam with the baton released:
  - audio keeps being serviced;
  - game logic pauses (the Wii has no equivalent);
  - `aurora_update` blocks in `SDL_WaitEvent` while paused
    (`lib/window.cpp:293`), which is fine with the baton released;
  - `waitForTick` afterwards does not try to catch up, because it waits for
    `msg >= nextCount` only.
- The seam is also the only place SDL events are pumped. Other waits on the
  main thread must stay short (see D6).

## 5. Power, reset, exit

- **Window close / Cmd+Q** (`AURORA_EXIT`) → `Power::pressPowerButton()`.
  The game's own power flow fades out, then calls `exitApplication` →
  `OSShutdownSystem` → handler.
- **Home menu "Quit"** → `requestGoWiiMenu` → `OSReturnToMenu` → handler.
  **"Restart from Title"** → ApplicationReset, in-process.
- No reset key is bound. The approved controls have none, and the Home menu
  covers restart. `Power::setResetButton` stays available if a key is
  approved later.
- **Exit handler** (`Exit{intent,...}`):
  - It runs on the main thread **with interrupts disabled** (the OS
    interrupt lock is held; `os_power.cpp:81`).
  - It must not call anything that joins or waits on a platform interrupt
    thread that takes the interrupt lock: `Audio::shutdown`,
    `VI::shutdown`, `GXSync::shutdown`, `DVD::shutdown`. Those would
    deadlock (D7).
  - What it does:
    1. Under `HostAllocationScope`, pause the SDL audio stream (host only).
    2. `fflush` the logs.
    3. For `Restart`, call `Power::relaunch(code)`.
    4. Otherwise `std::_Exit(0)`.
  - NAND writes are already on disk when the NAND API returns. Confirm with
    the platform worker that no NAND data is buffered in-process.
  - Skip `aurora_shutdown`. If a clean Aurora shutdown is wanted, stash the
    intent and `longjmp`/unwind to `main` instead. That is not recommended:
    game frames are on the stack.
- **`setForOSResetSystem` calls `VIWaitForRetrace`** after
  `JUTVideo::destroyManager`. VI must still be running then, so never stop
  the VI thread in response to the power button.
- **After a GX abort, `GXInit` runs again mid-run**, inside the open Aurora
  frame (§1, §7).

## 6. Deadlocks and stalls

| # | Chain | Consequence | Mitigation (owner) |
| --- | --- | --- | --- |
| D1 | A GX command or wait issued while no Aurora frame is active: `publish()` is a no-op (`fifo.cpp:170`), so a draw-done/token is never processed. | `GXDrawDone` never returns; the 0.5 s alarm aborts the frame (`handleGXAbortAlarm`) or boot hangs. | Keep a frame **always open** while game code runs: open before `petari_game_main`, and have the seam end + begin. The seam loops until `begin_frame` succeeds (root). Alternatively, the platform's baton-safe draw-done could force processing outside frames (platform/root). |
| D2 | The main thread blocks in host code (`end_frame` drain/present, paused `SDL_WaitEvent`, vsync) **while holding the baton**. | Other OS threads cannot run: JAudio underruns, loaders stall, DrawSync callbacks are delayed. | Run the seam with the baton released (request R1). |
| D3 | A drain waits for the processor, which is halted at a DrawSync breakpoint. Only the DrawSync OS thread (prio 15) releases it, and it needs the baton that the main thread holds while it waits. | Hard deadlock if any drain or wait runs while a breakpoint holds the processor with the baton held. | Seam after `endFrame` (no pending breakpoint once draw-done completed); baton-safe `GXDrawDone` (platform plan); baton released during the seam (R1). |
| D4 | VI on an External clock driven by the main thread or its display link. `JUTVideo::setRenderMode` → `VIWaitForRetrace` ×2 inside `GameSystem::init`, and `setForOSResetSystem` waits for a retrace too. | Boot/exit hang: no seam runs to signal. | Keep `VI::Clock::Internal`. If a display-link clock is ever wanted, signal it from a dedicated host thread, never the main thread. |
| D5 | `pauseOnFocusLost = true`: `aurora_update` blocks in `SDL_WaitEvent` whenever the window loses focus. | Game frozen in the background with the baton held (D2). Differs from the Wii, where the game runs on. | `pauseOnFocusLost = false`. Input already releases keys on focus loss. Minimise/hide still pauses, with the baton released. |
| D6 | Long main-thread stretches without a seam (boot `GameSystem::init`, synchronous archive loads inside a frame, long `GXDrawDone`). | No event pumping, so macOS shows the spinning cursor after ~2 s. Harmless otherwise. | Accept for boot; measure. If needed, an OS idle hook could call `SDL_PumpEvents` on the main thread while it sleeps with the baton released (request R2). Aurora's only SDL event watch just sets pause flags (`window.cpp:148`), so pumping there is safe. |
| D7 | The exit handler runs with the interrupt lock held and calls a shutdown that joins an interrupt thread needing that lock (audio, VI, GXSync, DVD). | Hang on quit. | The exit handler only pauses the audio device and calls `_Exit`/`relaunch` (§5). |
| D8 | Token delivery waits for a GPU depth readback that only completes after the frame is submitted (`aurora_end_frame`, at the seam). Meanwhile the processor is halted at a breakpoint in the same frame, and `endFrame`'s `GXDrawDone` must process past it. This happens when two `pushBreakPoint`s land in one frame (TalkDirector + LensFlare + StarPointer can). | 0.5 s abort every frame, or a hard hang. | Requirement for root readbacks + platform GXSync: **no GP event may depend on `aurora_end_frame` running on the game thread.** Either the render worker submits and completes the passes up to a token mid-frame, or token-time depth is read from a copy that finishes asynchronously (Dawn device polled by a host thread, not the main thread's next Aurora call). |
| D9 | Root is making GX pipeline requests blocking, for correct first frames and token-time depth. First use of a pipeline then compiles while `GXDrawDone` waits. | The 0.5 s abort alarm can fire although nothing is hung: the frame is aborted, DrawSync is reset, and `GXInit` runs again. | Proposal: a native-only branch in `waitDrawDoneAndSetAlarm`/`handleGXAbortAlarm`. While the renderer reports compilation in progress (root exposes the status), the alarm re-arms instead of aborting; if the processor makes no progress for 0.5 s with nothing compiling, it aborts as on the console. Log every re-arm and abort. Owner: whoever owns `MainLoopFramework.cpp`'s native branch, with root's status API. |
| D10 | Any GX write from a non-main OS thread while the main thread runs `aurora_end_frame` with the baton released. | Data race on the FIFO buffer. | Only the main thread writes GX in normal operation; add a debug assertion in the GX wrappers (root). **Exception:** `handleGXAbortAlarm` writes GX (`GXAbortFrame`, a raw BP, `GXSetDrawDone`) from the OS alarm thread while the main thread is parked in `GXDrawDone`. The assertion must allow that, e.g. "main thread, or main thread parked in a draw-done wait". |

## 7. Requests to other owners

- **R1, platform OS:** a CPU release for host work. Proposed
  to them as `extern "C" petari_os_begin_host_blocking()` /
  `petari_os_end_host_blocking()`; the app binds them through
  `App::setCpuRelease`.
  - Constructor: the calling OS thread gives up the CPU as if sleeping, so
    ready threads run.
  - Destructor: it re-queues at its priority and waits for the baton.
  - It must not be entered with interrupts disabled.
  - The seam and any host wait on the main thread use it.
- **R2, platform OS (optional):** a main-thread idle hook called while the
  default thread is parked, for `SDL_PumpEvents` during long waits (D6).
- **Platform GXSync + root readbacks:** confirm D8's requirement, and that
  GP events never need the main thread to call Aurora.
- **Root GX:**
  - fill `App::PresentHooks` (§9) from `petari_attach_vi_renderer`:
    `composeFrame` presents the `GXCopyDisp` image and honours VI black/dim,
    and `imageRect` exports the present rectangle;
  - expose "pipeline compilation in progress" for the D9 watchdog.
- **Platform GXSync (FIFO owner): `GXInit` re-entry after an abort (§1, §5).**
  Proposal:
  - `GXInit` resets GX *shadow* state only. It must not reset GXSync:
    - keep the draw-done, draw-sync and breakpoint callbacks. JUTVideo sets
      its draw-done callback once, in its constructor; if `GXInit` cleared
      it, `preRetraceProc` would stop flipping XFBs because
      `data_80451544` would stay set;
    - keep the token register and the monotonic stream positions;
    - don't restart the GP interrupt thread.
  - The abort path must leave the counts consistent. Draw-dones discarded
    by `GXAbortFrame` are never delivered or counted. The handler's new
    `GXSetDrawDone` is counted and delivered, so the main thread's pending
    `waitDrawDone(n)` is satisfied by it.
  - The handler's GX writes come from the alarm (host) thread while the main
    thread is parked, and must be accepted (D10).

## 8. Validation plan

1. **Boot to the first seam** with a watchdog like `probe_watchdog.cpp`.
   Log the first frame's `beginRender`/seam/`waitForTick` timestamps, and
   count abort-alarm firings (expect 0).
2. **Frame pacing.** 600 frames:
   - seam-to-seam period ≈ 16.68 ms;
   - `VIGetRetraceCount` delta = 1 per frame;
   - no audio underruns (`AudioSDL::submittedFrames` against wall time).
3. **Focus and minimise:**
   - focus loss keeps the game running;
   - minimise pauses at the seam, while audio continues and the DrawSync
     thread stays responsive;
   - restore resumes.
4. **Home menu:**
   - F1 opens it and the overlay matches the frame;
   - Restart returns to the title, which exercises `GXInit` re-entry;
   - Quit exits with status 0 and no hang;
   - window close runs the power fade, then exits.
5. **Crash:** a deliberate fault writes a report to `<user>/Crashes`.

## 9. Implementation (native/app)

| File | Side | Content |
| --- | --- | --- |
| `native/include/petari/app.hpp` | neither SDK nor Aurora | `petari_host_frame_seam()`; `App::PresentHooks` (composeFrame, imageRect); `App::CpuRelease` (begin, end) |
| `native/app/host.hpp` | standard headers only | internal interfaces between the units below |
| `native/app/app_main.cpp` | Aurora/SDL | arguments and paths, Aurora (Metal, `vsync` off, `pauseOnFocusLost` off), SDL audio pre-init, start-up order of §3 |
| `native/app/frame_seam.cpp` | Aurora/SDL | the seam of §4, first frame, events, quit, viewport |
| `native/app/input_events.cpp` | input only | SDL events → input, `controls.txt`, pointer viewport |
| `native/app/platform_host.cpp` | SDK/platform | crash reports, disc, NAND, settings, `OSInit`, VI Internal clock + `petari_attach_vi_renderer`, exit handler, audio sink, `petari_game_main` |
| `src/Game/System/GameSystem.cpp` | game | one `#ifdef PETARI_NATIVE` call to `petari_host_frame_seam()` between `endFrame()` and `waitForRetrace()` |

Behaviour:
- **Paths.**
  - Disc: `--disc`, else `$PETARI_GAME_DIR`, else
    `./build/game-data/RMGE01`.
  - User data: `--user`, else `~/Library/Application Support/Petari`, with
    `NAND/`, `Crashes/`, `settings.txt`, and an optional `controls.txt` (in
    `Input::Bindings::serialize` form).
- **Quit.** The first window close presses the power button, so the game's
  own shutdown runs. A second close quits immediately. The exit handler only
  flushes stdio and then exits, or relaunches for `OSRestart`. It calls no
  platform shutdowns (D7).
- **Missing hooks.** Without `PresentHooks` or `CpuRelease`, the app prints
  one line each at start-up saying what is missing:
  - no presentation hook: the window shows Aurora's frame as is (not the
    `GXCopyDisp` image, and without VI black or dimming);
  - no image rectangle: the pointer and overlays span the whole window;
  - no CPU release: other game threads wait during the seam.
- **Tests.** `native/tests/app_seam_tests.cpp` checks:
  - seam order (release, compose, overlay, end frame, events, next frame,
    reacquire);
  - retrying the first and later frames while the window cannot present;
  - events reaching input while waiting;
  - the quit path;
  - viewport updates and fallbacks;
  - that every Aurora call runs under a host allocation scope.

  Standalone: `cmake -S native/app -B build/app`.

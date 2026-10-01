# Native platform services

Host replacements for the Wii SDK services `GameSystem` needs. Game code keeps
calling the original SDK API; these libraries implement it on macOS.

## Implemented

### DVD (`petari_platform_dvd`)

Replaces `src/RVL_SDK/dvd/*.c` except `dvdidutils.c`, which is portable and is
compiled unchanged. Do not also link the Wii DVD driver sources.

- **Mounting.** `PetariNative::Platform::DVD::mount()` (see
  `include/petari/platform/dvd.hpp`) accepts a directory containing `files/`
  (Dolphin layout) or `DATA/files/` (wit/nodtool layout). If
  `PETARI_GAME_DIR` is set, `DVDInit()` mounts it. With `sys/fst.bin`, entry
  numbers, names, and disc offsets are the disc's own. Every listed file must
  exist with its listed size, or the mount fails and lists the mismatches. With
  `ignoreDiscFst`, or when there is no `fst.bin`, the FST is built from `files/`
  in the mastering tools' order (ASCII upper-case name, then exact name). Disc
  offsets are then synthetic and 32 KiB aligned. `.DS_Store` and `._*` files
  are skipped. `sys/boot.bin` supplies `DVDGetCurrentDiskID()`. Without it, the
  ID is all zero and `MountInfo::hasDiskId` is false.
- **Paths.** `DVDConvertPathToEntrynum`, `DVDOpen`, `DVDFastOpen`,
  `DVDClose`, `DVDOpenDir`, `DVDReadDir`, `DVDCloseDir`, and
  `DVDGetCurrentDir` follow the lookup rules of `dvdfs.c`: ASCII
  case-insensitive names, `.`, `..`, and a trailing `/` that requires a
  directory. `DVDFileInfo::startAddr` keeps its Wii meaning, a disc word
  address.
- **Drive.** `DVDReadPrio`, `DVDReadAsyncPrio`, `DVDReadAbsAsyncPrio`,
  `DVDCancel`, `DVDCancelAsync`, `DVDCancelAllAsync`,
  `DVDGetCommandBlockStatus`, `DVDGetDriveStatus`, `DVDCheckDiskAsync`,
  `DVDPause`, `DVDResume`, and `DVDSetAutoInvalidation` run on one drive
  thread with four priority FIFOs.
  - A command's state moves WAITING → BUSY → END, CANCELED, or FATAL_ERROR.
  - Reads transfer in 0x80000-byte chunks. Cancelling a busy read takes effect
    after the current chunk.
  - Completion callbacks run on the drive thread while the drive lock is held.
    This is the host equivalent of running with interrupts disabled. Callbacks
    may issue or cancel commands. A synchronous DVD call from a callback aborts.
  - Before a disc is mounted, commands wait in NO_DISK and can be cancelled.
  - A host I/O failure puts the drive in the fatal error state. The game's
    disc-error path then runs.
- **Deviations, all explicit.**
  - Reads between files return zeros. A disc returns neighbouring bytes.
  - An offset that is not a multiple of 4 aborts. The SDK silently truncates it.
  - The SDK's out-of-file checks abort with a message instead of calling
    `OSPanic`.
  - Buffers need no 32-byte alignment, and lengths need not be multiples of 32.
  - `DVDInquiryAsync`, `DVDLow*`, partition and ticket functions,
    `__DVDPrepareReset`, and the other internal functions are not implemented.
    Linking code that calls them fails. Only `OS.c`/`OSExec.c` use them, and
    those need native replacements anyway.
  - `fst.bin` names are used as raw bytes. Non-ASCII (Shift-JIS) names have
    not been checked against an extractor's host names.
  - SDK warnings go through `OSReport`. Host I/O diagnostics go to stderr.
- **Tests.** `native/tests/platform_dvd_tests.cpp` (154 checks). They use
  synthetic extracted discs in `$TMPDIR` and cover:
  - FST parsing and validation, and the directory scan
  - path rules and directory enumeration
  - synchronous, asynchronous, absolute, chained, and concurrent reads
  - priority order
  - cancellation while waiting, while busy, and with no disc
  - disc check, fatal errors, shutdown
  - abort-on-misuse, run in forked children

  They pass under ASan+UBSan, TSan, and Release (20 runs).

### OS (`petari_platform_os`)

Interrupts, threads, mutexes, message queues, alarms, time, and reporting.
`src/RVL_SDK/os/OSMutex.c` and `OSMessage.c` are the SDK's own sources,
compiled unchanged. `os/os_sdk_private.h` is force-included to declare
`__OSGetEffectivePriority`, `__OSUnlockAllMutex`, and `__OSActiveThreadQueue`.

- **Uniprocessor scheduler** (`os_thread.cpp`, port of `OSThread.c`).
  - Run queues, priorities 0–31, priority inheritance, suspend counts,
    join/detach/cancel/exit, `OSYieldThread`, and
    `OSDisableScheduler`/`OSEnableScheduler` keep the SDK's rules and
    `OSThread` fields.
  - Every `OSThread` is a host pthread. A CPU baton means only the
    highest-priority ready thread executes game code, as on the Wii's single
    CPU. Unsynchronised game data shared between threads keeps its Wii
    guarantees.
  - `__OSThreadInit()` binds the calling host thread as the default thread
    (priority 16). The native `OSInit` (root, with arenas) must call it.
- **Interrupts** (`os_interrupts.cpp`). `OSDisableInterrupts` takes one global
  lock. Enabled/disabled state is per thread, like MSR[EE] in each Wii context.
  The DVD drive uses the same lock.
  - Host threads that are not OS threads are interrupt sources: the DVD drive,
    the alarm timer, and later VI/audio.
  - Their callbacks run with interrupts disabled and may wake OS threads. An
    idle CPU is dispatched immediately. A running lower-priority thread is
    preempted at its next interrupt-state change.
- **Host blocking** (`petari/platform/os_host.hpp`, C linkage).
  `petari_os_begin_host_blocking()` makes the calling OS thread give up the
  CPU as if it slept, so ready OS threads run while it does host work (the
  app's frame seam: `aurora_end_frame`, present, event waits).
  `petari_os_end_host_blocking()` makes it ready at its priority and returns
  holding the CPU. In between, blocking OS calls on that thread abort, and
  platform waits (GX drain, VI, DVD) treat it as a host thread. Nesting,
  interrupts disabled, and end without begin abort. Requested by the app
  worker.
- **Alarms** (`os_alarm.cpp`, port of `OSAlarm.c`). The alarm queue and
  periodic arithmetic are the SDK's. A timer host thread replaces the
  decrementer and runs handlers with interrupts and the scheduler disabled,
  then reschedules. `OSSleepTicks` is the SDK's alarm-plus-suspend
  implementation.
- **Time** (`os_time.cpp`).
  - `OSGetTime` counts 60.75 MHz ticks since 2000-01-01 local time, as the
    IPL-initialised time base does. It advances with the host monotonic clock.
  - `__OSGetSystemTime` counts ticks since process start.
  - `OSTicksToCalendarTime` is the SDK code.
  - `__OSBusClock` and `OS_BUS_CLOCK_SPEED` are 243 MHz.
- **Reporting.** `OSReport`/`OSVReport` write to stdout. `OSPanic` prints the
  SDK's location line and aborts.
- **Deviations, all explicit.**
  - *Preemption timing.* Preemption by an interrupt waits for the running
    thread's next OS call. A thread in a long computation that makes no OS calls
    delays higher-priority threads natively. The Wii preempts at any
    instruction.
  - *Host start-up latency.* Newly scheduled host threads may start later than
    the emulated timeline implies.
  - *Stacks.* Threads run on host stacks of at least 2 MiB. The game-supplied
    stack only carries the SDK bookkeeping (`stackBase`, `stackEnd`,
    0xDEADBABE).
  - *Exit and cancel.* A cancelled or exited thread's host thread ends with
    `pthread_exit`. Its game frames are abandoned, not unwound, as a cleared Wii
    context is.
  - *Scheduler disabled.* `OSExitThread` with the scheduler disabled resets the
    count and switches. Blocking with the scheduler disabled aborts; on the Wii
    it would spin.
  - *Interrupt context.* Blocking calls from interrupt context (non-OS host
    threads) abort.
  - *Not implemented.* `OSContext` functions (`OSClearContext`,
    `OSSetCurrentContext`, `OSFillFPUContext`, `OSGetCurrentContext`),
    `OSSetSwitchThreadCallback`, `OSCancelAlarms`, `OSCond`, and the exception
    and error handlers are not implemented. Linking code that uses them fails.
- **Rule for native platform code on OS threads:** block only through OS
  primitives (`OSSleepThread`, message queues, `OSSleepTicks`). A host
  blocking wait keeps the CPU baton and stalls every other OS thread.
- **Tests.** `native/tests/platform_os_tests.cpp` (8313 checks), covering: host blocking;
  - interrupt semantics and exclusion of handler threads
  - preemption on resume
  - an exact unsynchronised counter across 4 threads with yields
  - message order including jam, and blocking send/receive
  - recursive mutexes and priority inheritance with immediate hand-off
  - exit from nested calls, cancel, detach, suspend counts, priority changes
  - scheduler disable
  - one-shot, periodic, and cancelled alarms
  - `OSSleepTicks` letting lower priorities run
  - alarm-driven preemption of a busy lower-priority thread
  - calendar conversion
  - DVD on OS threads: callback → message wakeup, a loader blocked in
    `DVDReadPrio` yielding the CPU, `DVDCancel` from an OS thread
  - misuse aborts

  Clean under ASan+UBSan (40 runs), TSan, and Release (20 runs).

### NAND save storage (`petari_platform_nand`)

A host IOS file system (`nand/isfs_host.cpp`) with a port of the synchronous
SDK NAND library on top (`nand/nand_host.cpp`: `NANDCore.c`, `nand.c`,
`NANDOpenClose.c`, `NANDCheck.c`). The SDK sources cannot compile natively
unchanged: they pass pointers through `u32` and use missing ISFS declarations.

- **Root.** No writes happen until a host directory is mounted:
  `PetariNative::Platform::NAND::mount()` or `PETARI_NAND_ROOT`. Without one,
  `NANDInit` fails with `NAND_RESULT_UNKNOWN` and every other call returns
  `NAND_RESULT_FATAL_ERROR`, as the SDK does before a successful init.
- **Boot and title.** The first `NANDInit` on a mount does the console's boot
  setup:
  - creates `/title`, `/title/00010000`, `/shared2`, `/meta`, and `/tmp` as
    system directories;
  - empties `/tmp`, as IOS does at boot.
  ES identifies the running title from the mounted disc ID: RMGE01 gives
  0x00010000'524D4745, home `/title/00010000/524d4745/data`. The data
  directory is owned by the title's uid (0x1000) and gid (maker code, "01" =
  0x3031). With no disc, `NANDInit` prints the SDK's "Failed to set home
  directory." and relative paths resolve against `/`.
- **SDK semantics kept.**
  - Relative paths resolve against the home directory, including `.` and `..`.
  - `/shared2` is private: the public API is refused, the `NANDPrivate*` API
    is allowed.
  - Permissions are packed into owner/group/other, and owner read is required.
  - The ISFS→NAND error table is the SDK's.
  - `NANDMove` moves into a directory, keeping the name.
  - `NANDCheck` applies the home quota (0x400 blocks, 0x21 inodes) and the
    user quota (0x4400 blocks, 0xFA0 inodes).
  - `NANDInitBanner` uses 16-bit comments.
  - `NANDClose` validates the mark.
- **IOS rules enforced.**
  - Absolute paths are under 64 bytes, names at most 12 characters, depth at
    most 8. `.` and `..` components are refused at the file-system level, so
    nothing escapes the root.
  - Access is checked against the caller's uid/gid for each owner/group/other
    class. Creating, deleting, or renaming needs parent write access.
  - At most 15 descriptors; read/write rights are per descriptor.
  - Seeking beyond the end of the file is invalid. Opening does not truncate.
  - Delete is recursive and refused while a descriptor is open (`OPENFD`).
  - Rename replaces a same-type destination. A file moved across directories
    must keep its name.
  - Usage is counted in 16 KiB clusters, plus one inode per node including the
    directory itself.
  - File data is `fsync`ed on close, where IOS commits it.
- **Metadata.** Owner, group, attribute, and access bits are stored in the
  `com.petari.nand` xattr of each host node. Nodes placed in the tree by hand
  default to the running title with owner and group read/write.
- **Not implemented** (linking code that uses them fails):
  - the asynchronous NAND API (`NAND*Async`, safe open/close); the game only
    uses the synchronous calls, from its NAND manager thread;
  - `ISFS_*` as public symbols;
  - NAND logging to a file. Log messages go to `OSReport`.
- **Known limits.**
  - A case-insensitive host volume (default APFS) makes names that differ
    only by case collide. The Wii NAND is case-sensitive.
  - Total NAND capacity is not simulated; only `NANDCheck`'s quotas apply.

### SC system settings (`petari_platform_sc`)

`sc/sc_host.cpp` replaces `src/RVL_SDK/sc/*.c` (SYSCONF over NAND) with
validated native preferences. `include/petari/platform/sc.hpp` provides
`Settings`, `validate`, `current`, `set`, `setStore`, `save`, and `reset`.

- **Defaults.** English, 16:9, progressive, stereo, EuRGB60 off, sensor bar
  below, speaker volume 0x58, rumble on, sensitivity 3.
- **Validation.** Every field is range-checked (language 0–9, horizontal offset
  ±32, volume 0–127, sensitivity 1–5, …). Invalid values are refused, both
  from the API and from `SCSetWpad*`.
- **Persistence.** Only to a store file the application names (`key=value`
  text, replaced atomically). `SCInit` reloads the file. A bad file sets
  `SCCheckStatus()` to `SC_STATUS_ERROR` and keeps the previous values.
  `SCFlushAsync` without a store reports `SC_STATUS_ERROR`. Its callback runs
  before it returns; on the console it runs later.
- **Implemented.** Game calls: `SCGetLanguage`, `SCGetAspectRatio`,
  `SCGetProgressiveMode`, `SCGetSoundMode`. SDK calls: `SCGetEuRgb60Mode`,
  `SCGetScreenSaverMode`, `SCGetDisplayOffsetH`, `SCGetIdleMode`,
  `SCGetWpad*`/`SCSetWpad*`, `SCGetBtDpdSensibility`, `SCInit`,
  `SCCheckStatus`, `SCFlushAsync`.
- **Not implemented.** Bluetooth pairing records (`SCGet/SetBt*DeviceInfoArray`),
  generic item access (`SCFind*Item`, `SCReplace*Item`), and
  `SCGetProductGameRegion`/`SCGetProductArea`/`SCGetCounterBias` have no host
  equivalent yet.

### ARAM (`petari_platform_aram`)

`aram/aram_host.cpp` ports `src/RVL_SDK/aralt/aralt.c`, the RVL SDK's ARAM
emulated in MEM2. `include/petari/platform/aram.hpp` provides `base`, `size`,
`allocated`, and `translate` for the future audio mixer. On the Wii the DSP
reads ARAM samples at the address the game gives `DsetVARAM`
(`JKRHeap::getAltAramStartAdr()`, the same MEM2 base).

- **Behaviour kept from `aralt.c`.**
  - `ARInit` adopts the MEM2 arena `[lo, hi)` as ARAM: offset N is host
    address `lo + N`, and offsets below `ARGetBaseAddress()` (0x4000) are
    reserved.
  - `ARAlloc` is a bump allocator of ARAM offsets.
  - `ARStartDMA(type, source, destination, length)` copies synchronously with
    the scheduler disabled, then runs the AR callback. Type 0 copies main
    memory to an ARAM offset; type 1 copies an ARAM offset to main memory. In
    both, argument 2 is the source.
  - `ARQInit` installs the ARQ service routine. The RVL SDK has no
    `ARQPostRequest`, so the queues stay empty.
- **Native checks. Each one aborts via `OSPanic` instead of silently copying
  or wrapping:**
  - the direction must be 0 or 1;
  - host pointers must be non-null;
  - the ARAM range must lie within ARAM and fit in 32 bits;
  - `ARAlloc` must not pass the end of ARAM;
  - ARAM must be initialised.

  No alignment is required. RVL ARAM is a `memcpy` into MEM2, not GameCube
  ARAM DMA.
- **Game edits** (coordinated with the game worker, `PETARI_NATIVE` only):
  - `HeapMemoryWatcher::createRootHeap` reserves 0x4000 + 0xE00000 bytes of
    MEM2 for ARAM. The Wii reserved 0xE00000, which let the top 16 KiB of the
    audio ARAM heap (offsets 0x4000..0xE04000) overlap the GDDR3 root heap.
    It also made `JKRAram`'s graph size wrap to 0xFFFFC000; natively it is 0.
  - `JKRAramPiece::startDMA` (Overwrite.cpp) calls `ARStartDMA` directly. The
    Wii version screens by Wii address ranges, which completes every
    ARAM→main request without copying.
- **Tests.** `native/tests/platform_aram_tests.cpp` (25 checks), using root's
  real MEM2 arena:
  - the `JKRAram(0xE00000, 0xFFFFFFFF)` arithmetic (reserved 0x4000,
    total 0xE04000, audio at 0x4000, graph size 0 at 0xE04000);
  - the Wii-sized reservation aborting on its wrapped graph size;
  - main↔ARAM round trips, including unaligned, end-of-ARAM, and full 14 MiB
    transfers;
  - abort cases for out-of-range, null, unknown-direction, pre-init, and
    over-allocation.

### VI (`petari_platform_vi`)

`vi/vi_host.cpp` replaces `src/RVL_SDK/vi/*.c`. The hook contract is in
`include/petari/platform/vi.hpp`.

- **Pending, flushed, latched.**
  - `VISetNextFrameBuffer`, `VISetBlack`, `VIConfigure`, `VIConfigurePan`, and
    `VISetTrapFilter` change pending state.
  - `VIFlush` arms it; `VIGetNextFrameBuffer` shows the armed XFB.
  - The next retrace latches it (`VIGetCurrentFrameBuffer`, `VIGetTvFormat`,
    `VIGetScanMode`).
  - VI starts black in the boot TV mode (`setBootTvMode`, default
    `VI_TVMODE_NTSC_INT`).
- **Retrace interrupt.** Order as in `__VIRetraceHandler`:
  1. count
  2. pre-callback
  3. latch, then the `onLatched` hook
  4. post-callback
  5. wake `VIWaitForRetrace` sleepers (`OSSleepThread` on the retrace queue)
  6. dimming bookkeeping

  All of it runs with interrupts disabled on a host thread.
- **Clocks.**
  - `Internal`: a VI host thread at the latched mode's field rate, 60000/1001
    Hz (NTSC, MPAL, EuRGB60, progressive) or 50 Hz (PAL). One interrupt per
    period. After a long stall the timeline restarts instead of firing a
    burst.
  - `External`: `signalRetrace()` from a host thread, for a display link or
    deterministic tests.
- **Renderer bridge.** `setHooks({onConfigure, onLatched, user})` and
  `displayState()`.
  - `onConfigure` runs on the game thread inside `VIConfigure`, for window
    sizing.
  - `onLatched` runs in interrupt context and may only record or signal.
  - The render thread presents `displayState()`: the latched XFB, black,
    dimmed, render mode, and field rate. Nothing on the retrace thread
    touches WebGPU or Aurora. The platform has no Aurora dependency.
- **Other.**
  - `VIGetDTVStatus` returns 1 (digital, progressive-capable display).
  - Dimming counts idle retraces: 18000 NTSC / 15000 PAL. It is disabled when
    SC screen saver is off. `VIResetDimmingCount` marks activity, and the
    dimmed flag is exposed for the renderer.
  - Not implemented: beam position (`VIGetCurrentLine`, position callbacks),
    gamma/video DAC programming beyond the trap-filter flag, DVD motor idle
    stop, and the PAL↔NTSC mode-change panic.
- **Tests.** `native/tests/platform_vi_tests.cpp` (70 checks), covering:
  - the external deterministic clock: callback order and interrupt context,
    unflushed changes not shown, flush → latch between pre and post, hook
    contents, a PAL/progressive mode switch with 50 Hz, pan reset;
  - `VIWaitForRetrace` on OS threads, with lower-priority threads running
    meanwhile, and a JUTVideo-style callback message waking an OS thread;
  - dimming and the SC screen saver;
  - misuse aborts;
  - the realtime internal clock at 59.94 Hz and then 50 Hz.

### Cache operations (`os/os_cache.cpp`, in `petari_platform_os`)

Host memory is coherent, so cache operations order memory rather than write it
back or discard it.

- `DCFlushRange` and `DCStoreRange` are sequentially consistent fences; the
  NoSync forms are release fences.
- `DCInvalidateRange` and `DCInvalidate` are acquire fences. A flush in the
  producer and an invalidate in the consumer give the Wii's DMA visibility
  guarantee.
- `DCZeroRange` zeroes the 32-byte blocks covering the range, like `dcbz`.
- `ICInvalidateRange` calls `sys_icache_invalidate`.
- The locked cache (`LCEnable`/`LCStoreData`/`LCStoreBlocks`/`LCQueueWait`)
  copies whole 32-byte blocks synchronously and reports the SDK's transaction
  counts. It aborts unless enabled.

### Audio (`petari_platform_audio`)

Replaces `src/RVL_SDK/ai/ai.c` and `src/RVL_SDK/dsp/{dsp,dsp_task}.c`.

- **AI** (`audio/ai_host.cpp`, API in `include/petari/platform/audio.hpp`).
  - `AIInitDMA` registers a host stereo s16 block, latched at the next block
    start; `AIStartDMA`/`AIStopDMA` start and stop it.
  - The application's CoreAudio or SDL callback calls
    `PetariNative::Platform::Audio::pull(buffer, frames)`. It is realtime-safe:
    no OS lock, and a spin lock only while latching a block. It outputs silence
    while stopped.
  - Each block start raises the AI DMA interrupt. It is delivered on an AI
    interrupt thread with interrupts disabled. `setSink` notifies the backend
    when DMA first starts, with the rate from `AISetDSPSampleRate`.
- **DSP device** (`audio/dsp_device.cpp`, API in
  `include/petari/platform/dsp.hpp`).
  - The SDK mailbox (`DSPSendMailToDSP`, `DSPReadMailFromDSP`, …) and
    `__DSP_boot_task`. Boot hashes the microcode (HashEctor) and accepts only
    SMG's JAudio2 microcode, `0xD643001F`. Anything else aborts.
  - A DSP worker thread runs the microcode's mail and command state machine:
    - setup 01, render 02, VARAM 0E, and the no-op commands;
    - frame rendering per 16-voice group release;
    - an acknowledgement per subframe; frame end and continue.
  - A DSP interrupt thread calls JAudio2's `__DSPHandler`. It services all
    pending DSP interrupts before thread code resumes, as on the console. The
    last subframe's acknowledgement and the frame-end mail are posted
    together; otherwise the audio thread could release the next frame before
    `MAIL_CONTINUE`.
  - 32-bit mails carry host addresses as registry handles (`addressHandle`,
    `PetariNativeDspAddressHandle`).
- **Renderer** (`audio/dsp_renderer.cpp`): original code. Behaviour follows
  Dolphin's HLE of this microcode family, `UCode_Zelda`/`ZeldaAudioRenderer`
  at dolphin-emu/dolphin `5102a0339c2177575378107b76541e47cc52122d`, used as a
  reference only. No code is copied. It implements:
  - voice parameter blocks (`JASDsp::TChannel`, whose 32-bit fields are
    host-endian natively);
  - PCM8/PCM16 from ARAM (big-endian);
  - 4-bit and 2-bit AFC with the loop predictor restore;
  - square, saw, and constant-pattern oscillators;
  - 4-tap/64-phase resampling (4.12 pitch);
  - low-pass and biquad filters;
  - 6 routed channels with linear volume ramps;
  - Dolby positional mixing (sine law);
  - forced-stop fade;
  - the four reverb blocks (`JASDsp::FxBuf`: circular buffers, 8-tap filter,
    two sends);
  - 4.12 output volume.

  The native `TChannel`/`FxBuf` layouts are pinned by `static_assert` on both
  sides.
- **JAudio2 edits** (`PETARI_NATIVE` only):
  - `dsptask.cpp`: host IRAM/DRAM pointers.
  - `dspproc.cpp`: address handles in mails; acquire/release waits.
  - `osdsp_task.cpp`: no DSP register or context switching in `__DSPHandler`;
    atomic `DspRunningStatus`.
  - `JASDSPInterface.cpp`: layout asserts.

  The mail protocol itself is unchanged.
- **Real-data validation.** `tools/audio_afc_verify.py <disc>` decodes every
  looping 4-bit AFC wave in `AudioRes` up to its loop block. It reproduces the
  predictor history Nintendo stored for all 333 of 333. The C++ renderer's
  decoder state is identical to that reference (600/600 subframes compared).
  Waves rendered at their own rates (22050 and 16000 Hz) have no clipped
  samples.
- **Tests.**
  - `platform_audio_tests.cpp` (36 checks):
    - PCM16/PCM8, pitch, and loops;
    - hand-computed 4-bit and 2-bit AFC;
    - volume ramps, forced stop, and pause;
    - Dolby panning; reverb sends; ARAM faults;
    - AI block latching, interrupts, sink, and silence.
  - `platform_audio_dsp_tests.cpp` (96 checks) runs JAudio2's own
    `dsptask.cpp`, `dspproc.cpp`, `osdsp.cpp`, and `osdsp_task.cpp` against
    the device:
    - boot handshake; table setup and VARAM acks;
    - two voice groups over 8 frames of 7 subframes;
    - per-subframe interrupts in order; frame end and continue; voice finish.
  - Both pass under ASan+UBSan (40 runs), TSan, and Release.
- **Unresolved fidelity** (shared with the reference unless noted):
  1. The variable FIR filter (filter-mode bits 0–4, coefficients at 0x120) is
     not applied.
  2. The back-left and back-right buses (Dolby Y below front) are attenuated
     each frame but never reach the output. SMG's stereo mode sends Y = 0
     (front), so it is unaffected. The surround sound mode would lose rear
     content.
  3. `SEND_TABLE` destinations 0x0DC8/0x0E28/0x0E88/0x0EE8 (8 samples into a
     bus) are mapped as offset sends. This is not verified against the
     microcode (this port).
  4. Mixing into buses saturates. The ucode's overflow behaviour is
     unverified (this port).
  5. Volume ramps are per-sample linear; hardware steps 32 times.
  6. The fourth word of each channel entry (skip flags) is ignored.
  7. `DsyncFrame4ch` (4-channel output) arguments are ignored.
  8. Of SMG's 1796 waves, 1791 are 4-bit AFC (validated) and 5 are PCM16. No
     wave uses 2-bit AFC, so that path is checked only by hand-computed tests.
  9. DSP rendering is instantaneous. AI plays the nominal 32000 Hz; the Wii
     runs at about 32028.5 Hz.

### Power and reset (`os/os_power.cpp`, in `petari_platform_os`)

Replaces the power/reset parts of `OSReset.c` and `OSStateTM.c`. The API is in
`include/petari/platform/power.hpp`.

- **Buttons.** The console's buttons become host events:
  - `pressPowerButton()` for a quit request;
  - `setResetButton(bool)` for a reset key.

  As with the STM device, they run the one-shot `OSSetPowerCallback` or
  `OSSetResetCallback` callback with interrupts disabled. A callback then falls
  back to a no-op default, and a default previous callback is reported as
  NULL. A reset press latches `OSGetResetButtonState()` until it is read.
- **Shutdown functions.** `OSRegisterShutdownFunction` keeps them in priority
  order. `__OSCallShutdownFunctions` stops at the first priority level with a
  failure. Leaving the game repeats non-final passes until all succeed, then
  makes a final pass with interrupts disabled. Events: shutdown 2, restart 4,
  return to menu 5, reboot 1.
- **Exit.** `OSShutdownSystem`, `OSRestart(code)`, `OSReturnToMenu`, and
  `OSRebootSystem` then pass an `Exit{intent, resetCode, event}` to the
  application's `setExitHandler` handler, which must not return (the call
  aborts if it does). Without a handler:
  - shutdown exits 0;
  - return to menu and reboot print that there is no Wii Menu or console,
    then exit 0;
  - restart re-executes the application with `PETARI_RESET_CODE`, and the new
    instance reports `OSGetResetCode() == 0x80000000 | code` and
    `OSIsRestart()`.

  A cold boot reports 0.
- **Not native.** There is no SRAM sync, idle/standby (the SC idle mode is
  ignored), audio/PAD stop, or thread kill before the handler, and no screen
  dimming reset on the reset button. `OSResetSystem` (obsolete in the SDK) and
  `OSReturnToDataManager`/`OSReturnToSetting` are not implemented.
- **Also here.** `PPCSync` (a sequentially consistent fence) and
  `OSRegisterVersion` (`OSReport`, as `OS.c`).
- **Tests.** `platform_power_tests.cpp` (24 checks):
  - one-shot callbacks from another thread; reset latching, holding, and
    re-pressing;
  - shutdown-function priority gating, retries, and the final pass with
    interrupts disabled;
  - every intent and event through a handler;
  - default exits; a handler that returns aborts;
  - a real `OSRestart(5)` relaunch whose new process reports `0x80000005`.

### WiiConnect24 (`petari_platform_nwc24`)

`nwc24/nwc24_host.cpp` replaces `src/RVL_SDK/nwc24/*.c`. WiiConnect24 is
unavailable, as on a console with it disabled.

- `NWC24OpenLib` returns `NWC24_ERR_DISABLED` (`NWC24_ERR_INVALID_VALUE`
  without a work buffer). `NWC24GetErrorCode` is 0, since no daemon error was
  ever recorded.
- Every message call the game makes returns `NWC24_ERR_NOT_READY`.
  `NWC24GetMsgSize` reports 0.
- The game's `NWC24Messenger` treats `DISABLED` as non-restorable. It shows its
  WiiConnect24 notice (WC24_03) for foreground sends and drops background ones.
  Nothing reports success.
- **Tests.** `platform_nwc24_tests.cpp` (11 checks).

### Crash reporting and PowerPC machine state (`os/os_crash.cpp`, in `petari_platform_os`)

The API is in `include/petari/platform/crash.hpp`.

- **Report.** `Crash::install(dir)` catches SIGSEGV, SIGBUS, SIGILL, SIGFPE,
  SIGTRAP, and SIGABRT on an alternate stack. It writes a report to stderr and
  to `dir/petari-crash-<time>-<pid>.txt` containing:
  - the signal, code, and fault address;
  - host arm64 registers, labelled as host;
  - a host backtrace;
  - the running OS thread and whether interrupts were disabled;
  - the last `OSPanic`/`OSFatal` message;
  - the last 32 `OSReport` lines;
  - the game error handlers registered with `OSSetErrorHandler`.

  The signal is then re-raised with its default action.
- **Game error handlers are not called.** JUTException's handler expects a
  PowerPC `OSContext`, and none is fabricated. `OSSetErrorHandler` is a real
  registry that returns the previous handler.
- **Contexts.**
  - `OSGetCurrentContext` returns the per-thread current context, defaulting
    to the running `OSThread`'s context.
  - `OSSetCurrentContext` sets it.
  - `OSClearContext` resets FPU mode and state and leaves registers untouched.
- **PowerPC-only calls.** Anything that would need invented PowerPC state
  aborts with an explanation: `OSFillFPUContext`, `OSProtectRange` (MEM1
  protection channels), `PPCMtmsr` enabling FE0/FE1 (Apple silicon cannot
  trap FP exceptions), and `PPCHalt`.
- **Modelled machine state.**
  - `PPCMfmsr`/`PPCMtmsr` model MSR[EE] (the thread's interrupt state) and
    FE0/FE1 (always off). Other bits read 0.
  - `OSGetStackPointer` returns 0 (no PowerPC stack).
  - `__OSFpscrEnableBits` keeps the SDK default `0xF8` and has no host effect.
- `OSFatal` records its message and aborts (reported).
- **Tests.** `platform_crash_tests.cpp` (27 checks) runs real faults in forked
  children and inspects the report files. It covers SIGSEGV with a
  handler/OSReport/thread report, `OSPanic`, `OSFatal`, the refusing
  PowerPC-only calls, the registry, contexts, and MSR[EE].

### VF (`nwc24/vf_host.cpp`, in `petari_platform_nwc24`)

`VFInitEx` takes the work area, as it does before any drive is mounted on the
console. The game's only VF call is in `NWC24System`, and every other VF use
belongs to the unavailable WiiConnect24 library. No other VF function is
provided, so any real FAT use fails to link.

### GX synchronisation (`petari_platform_gx_sync`, `native/gx/sync_*`)

`gx_sync/gx_sync.cpp`; API in `include/petari/platform/gx_sync.hpp`; design
and the end-of-frame deadlock analysis in `docs/dev/GX_SYNC_PLAN.md`. The platform
worker also owns the FIFO/processor half in native/gx:
`patch_aurora_sync.py`, `sync_backend.{h,cpp}` (C ABI between Aurora and SDK
headers), and `sync_bridge.cpp` (public SDK entry points). Root owns the
token-time EFB capture, `GXPeekZ`/`GXPeekARGB`, frame submission, and the
CMake wiring.

- **Processor.** Patched Aurora `fifo.cpp` never processes at or past the
  breakpoint. It stops each `process()` call at draw-done/token BPs and
  reports them at that command boundary, outside `sBufferMutex`, in stream
  order (an out-of-order report aborts). It applies `GXAbortFrame`'s discard
  at the next command boundary. `fifo::drain` waits through the platform.
- **Queue.** The event queue is unbounded, and events are never dropped under
  pressure. The reporting side takes only a private mutex, never the OS
  interrupt lock, so a game thread with interrupts disabled cannot stall the
  processor.
- **Delivery.** One GP interrupt thread delivers events in report order with
  the OS interrupt lock held: the draw-sync callback (token),
  draw-done callback, and breakpoint callback. Callbacks may use OS calls.
  `GXReadDrawSync` is the token register (0x48 and 0x47 BPs).
- **Token-time snapshots.** With root's snapshot hook installed, each token
  interrupt gets a ticket. The hook runs on the processor thread at the
  token's boundary; the token and every later event wait for
  `snapshot_ready(ticket)`, while the processor continues. Tickets may
  complete out of order; delivery order never changes.
  `delivering_ticket()` is what `GXPeekZ` reads inside the callback. Without
  a hook, token-time depth is **unavailable**, not approximated.
- **Waits.** `GXDrawDone`, `GXWaitDrawDone` and `fifo::drain` put game OS
  threads to sleep on OS thread queues (yielding the CPU baton); host threads
  wait on a condition variable. Waiting in interrupt context aborts.
- **Breakpoint.** `GXEnableBreakPt`/`GXDisableBreakPt` move it and wake the
  processor. Halting raises the breakpoint interrupt once.
- **Abort.** `GXAbortFrame` is safe in interrupt context (handleGXAbortAlarm).
  Draw dones lost with the discarded stream are credited to the next
  delivered one, so the waiting `GXDrawDone` wakes on the recovery draw done
  and later waits count correctly. The GX callbacks, token register and
  stream positions survive `GXAbortFrame` and a later `GXInit`.
- **Hang check.** handleGXAbortAlarm asks `GXSync::checkWait`
  (`petari_gx_wait_check`) instead of the Wii's "no progress for 0.5 s":
  progress is reported only between command batches, and one batch can spend
  seconds in serial first-use pipeline compiles (observatory run 6 aborted a
  good frame between two compiles). It aborts only when the processor is
  halted at the breakpoint or idle with a draw done outstanding for 1 s while
  no OS thread holds, wants, or is away doing host work with the CPU; other
  stalls are logged, with a full platform dump after 10 s and every 10 s
  after. Regressions: `native_platform_gx_sync` (compile burst, halted with a
  runnable thread, halted with nothing runnable plus the lost-draw-done
  recovery).
- **FIFO lock and compile bursts.** The processor runs `process()` on its own
  copy of the published stream (patch_aurora_sync.py), so Aurora's
  `sBufferMutex` is held only while a range is copied out, never across a
  batch that blocks in first-use pipeline compiles. Game threads that still
  find it busy after a 1 ms spin wait with the CPU released
  (`petari_os_try_begin_host_blocking`). Before, a game thread growing the
  FIFO waited out the whole batch holding the CPU, stopping every game thread
  (observatory run 5's audio signature). Regression: `native_gx_fifo_processor`
  (the generated fifo.cpp with a fake processor that blocks like a compile).
- **Hang reports.** `petari/platform/diagnostics.hpp`: every OS thread (state,
  wait queue, entry, host run state, sampled host stack), the CPU baton,
  interrupt-lock holder, pending alarms and GX sync state. The smoke watchdog
  records it, after `/usr/bin/sample` stacks of the whole process (into the
  user directory's `Crashes/`), before exiting 124/125.
- **FIFO objects and pointers.** `GXGetCPUFifo`/`GXGetGPFifo` have the SDK
  ABI (`GXBool f(GXFifoObj*)`). `GXGetFifoPtrs` returns opaque stream
  positions: 4 GiB plus the byte position, never dereferenced (confirmed with
  root), above DrawSyncManager's token range.
- **GP status and metrics.**
  - `GXGetGPStatus` gives actual processor state: read/command idle
    (processed ≥ published) and whether the processor halted at the
    breakpoint. The FIFO overhi/underlow watermarks are not modelled and
    report false.
  - `GXReadXfRasMetric` is **diagnostic, not hardware counters**. `clocks`
    is the low 32 bits of FIFO command bytes processed: a progress indicator,
    not XF or GP clock cycles, which the native renderer does not have. The
    transform-wait and raster-busy counters are not measured and read 0.
- **Resources.** One GP interrupt thread, started on first use. Queue memory
  comes from host allocation.
- **Fidelity notes.**
  - Draw done means processed by the command processor (and submitted),
    not GPU pixel completion.
  - Token callbacks with snapshots arrive after the capture's readback, later
    than on hardware.
  - Abort cannot interrupt a command already being decoded (the hardware
    resets mid-command). The SDK's dropping of dirty shadow state is not
    reproduced.
  - `GXSetDrawDone` outside an Aurora frame is processed with the next frame,
    except while a `GXDrawDone` is waiting.
- **Tests.** `gx_sync_backend_tests.cpp` (about 40 checks) drives the real
  `sync_bridge.cpp`/`sync_backend.cpp` against a fake Aurora FIFO that runs
  the patched loop. It covers SDK command order, the `GXGetCPUFifo` ABI,
  pointers, baton-yielding `GXDrawDone` and drain, gating outside a frame,
  breakpoint and GP status, and snapshot tickets (hook position,
  out-of-order completion, flush hook, delivering/delivered ticket). It also
  runs the DrawSyncManager protocol with mid-frame `GXDrawDone` and
  end-of-frame drains, and handleGXAbortAlarm recovery from interrupt
  context. A bounded stress run (400 frames, about 3 s) covers tickets
  completed out of order or inside the hook, random mid-frame `GXDrawDone`,
  and 10 GP hangs recovered by a real `OSAlarm` abort handler. It checks
  stream order, exactly one ticket and callback per processed token, and none
  for discarded ones. `patch_aurora_sync.py` output was checked with `-fsyntax-only`
  against the pinned Aurora. `platform_gx_sync_tests.cpp` (56 checks, including ticket lifetime:
  completion inside the hook, double and never-issued completion abort,
  shutdown with held tickets, no reuse) uses a fake
  command-processor thread:
  - tokens, then draw done, in order and in interrupt context;
  - `GXReadDrawSync`;
  - `GXDrawDone` yielding to a lower-priority game thread;
  - breakpoint halt with real GP status, a static metric while halted, and
    resume;
  - 10000 interrupts with none dropped;
  - the processor keeps running while a game thread has interrupts disabled;
  - a replay of DrawSyncManager's pointer/token/breakpoint protocol over 30
    frames;
  - misuse aborts.

  Passes ASan (20 runs), TSan, and Release.

### Allocation ownership and the audio boot smoke (tests)

- `native/tests/platform_allocation_tests.cpp` (repository-root build only;
  it needs the JKR heaps) runs the platform under the game's replacement
  operator new/delete:
  - a registered game thread with a child JKR heap current first-touches
    every service;
  - the host workers (DSP mails, async NAND, async DVD, GP tokens) then drain
    and free what it queued, and the child heap must stay unchanged;
  - the child heap is destroyed and poisoned, and everything runs again;
  - an OS thread entry must still allocate from the game heap.

  Rule it pins: host-owned containers are built and mutated under
  `HostAllocationScope`, and scopes end before game callbacks or thread
  entries run on game threads.
- `native/tests/audio_boot_tests.cpp` is opt-in (`-DPETARI_AUDIO_BOOT_SMOKE=ON`,
  repository-root build, extracted disc at build/game-data/RMGE01). It runs
  the game's real audio start-up on the disc:
  - HeapMemoryWatcher (the 3 MiB audio solid heap), FileRipper, JKRAram,
    FileLoader;
  - AudSystemWrapper::requestResourceForInitialize, and createAudioSystem on
    a priority-14 OS thread;
  - a movement() + VIWaitForRetrace frame loop until the system-init and
    static waves load;
  - one real SE through AudSystem::startSound.

  The test itself acts as the audio device. It pulls 32000/60 frames per game
  frame, checks that the SE is audible, and writes a WAV to $TMPDIR. The only
  scaffold is language state: zeroed GameSystem/GameSystemObjHolder storage
  carrying just `mLanguage` from `MR::getDecidedLanguageFromIPL()`, since the
  real objects bring the scene graph. It found the JAudio2 `sendCmdMsg`
  literal-size truncation that kept wave arc 7 at "loading", now fixed by the
  library worker.

## Integration

Add to the root `CMakeLists.txt` after `petari_native_config` is defined:

```cmake
add_subdirectory(native/platform)
```

Link `petari_platform` (DVD + OS + NAND + SC + ARAM + VI + audio + NWC24 + GX sync). ARAM links root's
`petari_memory` for the MEM2 arena. It needs the root's `petari_host_runtime`
target. Do not also link `src/RVL_SDK/os/OSThread.c`, `OSAlarm.c`, `OSTime.c`,
or `OSInterrupt.c`: this layer replaces them. `OSMutex.c` and `OSMessage.c` are
already compiled here. Do not link `src/RVL_SDK/nand/*.c`, `src/RVL_SDK/sc/*.c`,
`src/RVL_SDK/aralt/aralt.c`, `src/RVL_SDK/vi/*.c`, `aurora::vi`,
`src/RVL_SDK/ai/ai.c`, `src/RVL_SDK/dsp/*.c`, `src/RVL_SDK/os/OSCache.c`,
`src/RVL_SDK/os/{OS,OSReset,OSStateTM,OSError,OSContext}.c`, `src/RVL_SDK/base/PPCArch.c`,
`src/RVL_SDK/nwc24/*.c`, `src/RVL_SDK/vf/*.c`, or the ES functions `ESP_InitLib`/`ESP_CloseLib`/`ESP_GetTitleId`/
`ESP_GetDataDir` from `src/RVL_SDK/esp/esp.c`: native versions are here. The
application should mount a NAND root (for example, under Application Support)
and a settings store before calling `NANDInit`/`SCInit`. The test target
`petari_platform_dvd_tests` / ctest `native_platform_dvd` is added when
`BUILD_TESTING` is on. Standalone build:

```sh
cmake -S native/platform -B build/platform -DPETARI_SANITIZERS=ON
cmake --build build/platform && ctest --test-dir build/platform
```

## Services still blocking boot

Counts are distinct call sites in `src/Game`, `src/JSystem`, and `src/nw4r`.

1. **OS threads, messages, mutexes, interrupts, alarms, time.** Done (see
   above). Game code still reaches `OSContext` functions (`OSClearContext`,
   `OSSetCurrentContext`, `OSFillFPUContext`) from `GameSystemException`, and
   `OSSetErrorHandler` (7 sites). These need a host policy: signal handlers
   and a crash report.
2. **Arena and heap bootstrap.** (Root owns this, in `native/memory`.)
   - `HeapMemoryWatcher::createRootHeap` / `JKRHeap::initArena` use
     `OSGetArenaLo`/`OSGetArenaHi`, `OSSetArenaLo`/`OSSetArenaHi`,
     `OSInitAlloc`, `OSGetMEM2ArenaLo`/`OSGetMEM2ArenaHi`, and
     `OSPhysicalToCached(0)` as `OSBootInfo*` (`memorySize`).
   - This needs host-allocated MEM1 and MEM2 arenas and a boot-info block.
   - `OSCachedToPhysical` and `OSProtectRange` need defined host meanings.
3. **ARAM.** Done (see above).
4. **Video timing.** Done (see VI above). Root still has to build the
   renderer bridge, which connects `onConfigure` to Aurora window
   configuration and presents `displayState()` from the render thread.
5. **Audio.** Done at the device level (see above). Still needed:
   - the application audio backend calling `Audio::pull` from CoreAudio or
     SDL;
   - an end-to-end run of the full JAudio2 engine (`JASAudioThread`) inside
     the game, where root links the game audio objects;
   - listening validation of mixed game scenes;
   - the Wii Remote speaker (`WPADSendStreamData`), which belongs to input.
6. **Controllers.** (Handled in `native/input`.) `KPADInit`, `KPADRead`, and `KPADSet*` parameters, plus
   `WPADProbe`, extension and connect callbacks, `WPADControlMotor`,
   `WPADGetSensorBarPosition`, and `WPADRegisterAllocator`. This needs a
   keyboard, mouse, or game-controller mapping that produces KPAD pointer,
   acceleration, and Nunchuk data.
7. **Saves and settings.** Done (see above). The application still has to
   choose the NAND root and the settings store location, and provide a
   settings UI.
8. **Power and reset.** Done (see above). The application should install an
   exit handler and map window close and a reset key to
   `pressPowerButton()`/`setResetButton()`.
9. **Other.** `NWC24*` is done (unavailable, see above). `THP*` movies use
   the DVD async path and the locked cache (implemented). `ARC*` is in-memory
   and portable. Crash reporting is done (see above). The application
   should call `Crash::install(dir)` at start-up.

# Native stubs inventory

Native replacements that return a fixed value, do nothing, or stand in for Wii
behaviour, and whether the game depends on the real behaviour. Classes:

- **(a)** irrelevant natively, or deliberately equivalent (reason given);
- **(b)** relevant and wrong: fixed here, with a test;
- **(c)** unknown: none remain; each earlier (c) was resolved from its game call sites.

Sweep of 2026-09-30. See "Method" to repeat it.

## Fixed: (b)

| What | Wii behaviour | Native before | Fix and test |
| --- | --- | --- | --- |
| `GXInvalidateTexAll` (Aurora no-op) plus `DCStoreRange`/`DCFlushRange` (fences only) | The GPU reads texels from memory. A texture the CPU rewrites in place, stores and then invalidates shows the new texels at its next draw. | Aurora uploads once per texture object (`texObjId`, `texDataVersion`). Only `GXInitTexObj`/`GXInitTexObjData` change those, so a persistent texture rewritten in place kept its first upload. Affected: SnowFloor and SnowFloorTile (snow dug by Mario, enemies and fireballs; SnowCapsuleGalaxy), NormalMapBase `mTextures`, and Mario's dark-mask dissolve (`MarioActor::updateDarkMask`). Fur and Mii textures are written before their first load, so they were not affected. | `native/platform/os/os_cache.cpp` logs every DC store, `DCZeroRange` and locked-cache DMA by 4 KiB page with a generation (`petari_dc_stored_since`). `native/gx/patch_aurora_texture.py` makes `GXLoadTexObj` bump `texDataVersion` when the texture's pages were stored since that object's last load; Aurora then re-hashes, and re-uploads only if the texels changed. Test: `native_texture_store`. It covers stores inside and outside the texture, NoSync and zero stores, fresh objects and double buffers, all on a registered game thread with a 64 KiB current JKR heap, which catches missing `HostAllocationScope`s. Mutation checks: without the store log, 7 checks fail; without either scope, 2 checks fail, or the test aborts with `bad_alloc` as the app did at the logo. |

The asm-equivalence audit found the other (b)s: native_math_util,
native_wii_asm_equivalence, and native/RELIABILITY.md.

## Platform shims (`native/`), linked into the app

Every C-linkage SDK symbol the app links was resolved to its defining object with
a link map (`-Wl,-map`). The short or constant ones are below. Everything else
has a real implementation; most are described in native/platform/STATUS.md.
Unsupported paths call `OSPanic` loudly instead of returning success
(`__DSP_exec_task`, `__DSP_remove_task`, `OSFillFPUContext`, `OSProtectRange`,
`PPCHalt`, `GXSetMisc(GX_MT_ABORT_WAIT_COPYOUT)`).

| Function | Native | Class | Why |
| --- | --- | --- | --- |
| `DCEnable`, `ICEnable`, `ICFlashInvalidate`, `LCEnable`/`LCDisable` state, `LCQueueWait` | nothing / fence | (a) | Host caches are coherent and nothing is generated at run time. Locked-cache DMA completes synchronously. |
| `DCInvalidateRange`, `DCInvalidate` | acquire fence | (a) | They pair with the producer's store fence. |
| `OSGetStackPointer` | 0 | (a) | Used only by JUTException's crash dump (native crash reporting is os_crash.cpp) and by SDK files that aren't compiled natively. |
| `VIGetDTVStatus` | 1 | (a) | Reports a progressive-capable display. `RenderMode.cpp:311` then honours the progressive setting, which is the intended Mac output. |
| `WPADGetWorkMemorySize` | 0 | (a) | There is no Bluetooth stack. HeapMemoryWatcher adds its fixed overhead to 0 (native/input/README.md). |
| `DSPCheckMailToDSP`, `DSPAssertInt` | 0 / no-op | (a) | The DSP device handles mail synchronously, so JAudio's "wait until the mailbox is empty" loops fall through. |
| `ESP_InitLib`, `ESP_CloseLib` | 0 | (a) | No ticket services are needed. `ESP_GetDataDir` is implemented (save paths). |
| `NWC24*` (all message calls) | `NWC24OpenLib` returns `DISABLED`; others return `NOT_READY`; `GetErrorCode` returns 0 | (a) | Models a console with WiiConnect24 turned off; the service is discontinued. NWC24Messenger handles `DISABLED`: background letters such as the staff/Luigi mail are dropped, and foreground sends show the game's WiiConnect24 notice. Tests: platform_nwc24_tests (11). Product note: if a foreground send exists on the story path, that notice appears as on a WC24-off Wii. |
| `DVDCloseDir` | TRUE | (a) | Directory handles hold no host resources. |
| default reset/power callbacks | empty | (a) | These match the SDK defaults; the game installs its own. |
| `JKRSolidHeap::do_free`, `JSU*Stream` destructors | empty | (a) | They are empty on the Wii too. |
| `OSInit` (libpetari_boot) | real native boot | (a) | This is the native entry. |
| HOME Button Menu `HBM*RSO` | native menu | (a) | The RSO module is PowerPC code; native/home_menu serves the entry points. |

## Aurora (third party), linked no-ops

| Function | Class | Why |
| --- | --- | --- |
| `GXSetZTexture` | (b), worked around | Z-texture replace is not implemented. The two clears that use it (`MainLoopFramework` and `DrawUtil`) draw on the orthographic far plane natively instead (PLAYTEST.md "Depth clears"). The other callers (`GXInit`, `ut_CharWriter`, `ImageEffectLocalUtil`) only disable it. |
| `GXInvalidateTexAll` | (b), fixed | See the fix above. |
| `GXInitFifoBase` | (a) | The FIFO is owned by Aurora and the GX sync layer (native/platform/GX_SYNC_PLAN.md). |
| `THPInit` returns TRUE | (a) | The decoder needs no setup. |

The display-copy setters (`GXSetDispCopy*`, `GXCopyDisp`, `GXSetCopyFilter`)
are stubs in upstream Aurora but real in the patched copy
(patch_aurora_present.py).

## `PETARI_NATIVE` branches in `src/` and `libs/`

Every branch whose native side is empty, one statement, or a constant was
listed and read. Most replace Wii asm or layout code with equivalents; the
asm cases are covered by native_wii_asm_equivalence. The ones that change
behaviour:

| Where | Native | Class | Why |
| --- | --- | --- | --- |
| `LayoutManager::getIndexOfPane` | -1 for a missing pane (the Wii returns 0, the root) | (a), hardening | Every caller either guards against -1 or first checks `getPane` on the same tree. Misses are reported (asset_diagnostics), and smoke PASS requires zero (build/asset-reference-report.md). |
| `J3DAnmLoader` | rejects malformed or unsupported (vertex-color) animation files with an OSReport | (a) | The disc has none: all 4753 animations load and evaluate (petari_j3d_anim_load_tests --assets: bck 3232, bpk 55, brk 503, btk 383, btp 422, bva 158; 0 failed). |
| `osdsp_task` | skips `OSClearContext`/`OSSetCurrentContext` around the DSP callback | (a) | There are no PowerPC contexts natively. |
| `db_assert` stack walk | omitted | (a) | Debug output only. |
| `KinopioAstro.cpp` `JKRArchive::getExpandedResSize`, `RFL_System.c` `RFLGetLastReason`, `MessageEditorMessageTag` | Wii copy compiled out | (a) | Each has a native definition elsewhere with the same body (`JKRArchivePri.cpp`, the other branch, `MessageEditorMessageTagNative.cpp`). |

No `PETARI_NATIVE` block returns unconditionally before game logic, except two
complete native replacements (`JKRAramPiece::startDMA` and
`ResFont::SetResource`).

## Method

1. `python3 native/tools/stub_scan.py native` flags empty, constant, `(void)`-only
   and marked bodies (constructors appear too and are skipped by eye). The same
   scan was run on the Aurora Dolphin layer and on the patched Aurora copies in
   `build/macos-gx/native/gx`.
2. Relink the app with `-Wl,-map,<file>` and resolve every C-linkage SDK symbol
   to its defining object, so nothing is missed because it lives outside
   `native/`: the game's own HBM wrapper, Aurora, RVL SDK files compiled
   natively, and test-only stubs (none are linked).
3. List `PETARI_NATIVE` branches in `src/` and `libs/` whose native side is
   short, empty or constant, and native-only blocks that return unconditionally.
4. For each candidate, read the game's call sites and decide (a), (b) or (c).

Not covered: functions with real but possibly incomplete bodies, such as the
defaults in SC settings or WPAD extension handling. Those are behaviour audits,
not stubs.

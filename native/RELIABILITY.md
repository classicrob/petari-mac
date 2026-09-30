# Native reliability coverage

This port is not yet validated as a complete game. A passing opening smoke is
not a release criterion for later stages. This ledger separates observed
behavior, corpus/component checks and remaining integration work.

## Verified in the running game

- Fresh Mario file creation, both saving windows and normal power-off.
- Existing-file loading without modifying the isolated NAND files (SHA-256).
- Five prologue pages and the letter, arrival, grounded idle, jumping and landing,
  opposite movement, pause with no movement, and resumed movement.
- Title background and translucent logo glow; file-select blue irises;
  in-stage Toad cap/spot/clothing colors and Mario colors.
- The corrected audio path over the opening reload route: zero device underruns
  and no unexpected DMA replays. The one initial silent replay is normal
  JAudio2 buffer initialization.
- Native Retina rendering and 1:1 presentation; the original small UI and
  storybook textures remain low resolution.

## Proactive checks, 2026-09-29

- NAND, input, Home menu and movie CTests: 4/4 pass in the root build.
- Found and fixed non-atomic NAND file replacement: the old destination was
  removed before rename. Direct file rename now retains the old destination
  until replacement succeeds. A concurrent host reader observes 256 replacements
  without a missing/partial file. The same test fails against the old
  implementation, compiled from a private source copy. This concerns file moves
  and safe-close replacement; it does not make every in-place game save atomic.
- HeavensDoorGalaxy placement review: all 104 distinct object names across seven
  zones resolve to creators. A bounded scan of 42 implementation files found no
  further instances of the native pointer/endian/initialization bugs checked.
  This is a source audit, not proof that placement or cutscenes run correctly.
- Generated collision regression covers the two-triangle runtime layout and
  22,293 sampled queries over 150 disc files. HeavensDoorGalaxy placement now exercises that path in the app (story run 6).

## Good Egg Bean B landing, 2026-09-30

- Root cause: `MR::PSvecBlend` and `MR::vecScaleAdd` (src/Game/Util/MathUtil.cpp)
  were Wii paired-single asm inside `#ifdef __MWERKS__` with no native branch,
  so they were empty functions. `MR::vecBlend` silently kept its old value (45
  call sites, including Mario, XanimeCore animation blending, DinoPackun,
  SwingRope, Fluff, Tamakoro and BezierSurface). After the Peanut launch star,
  Mario's binder offset (`MarioActor::_2C4`) kept the Peanut's up vector. The
  binder sphere sat 51 units below his feet, so he hovered 80-110 over Bean B.
  Then a blown landing snapped him 2176 units to a stale ground point.
  Bean B's collision itself was correct: placement, scale and native sphere
  queries were checked offline against the logged positions.
- Fixed with native branches. `native_math_util` fails all 5 checks against the
  old MathUtil.cpp (compiled from a private copy). `native_asm_only_bodies` scans
  src/, libs/ and include/ for function bodies that are only `__MWERKS__` asm;
  these two were the only ones.
- Live (build/collision-1.log before, build/collision-2.log after, stage fixture):
  before, the offset stayed (-62.6,-16,-26.9) for 98 frames. After, Mario
  lands on Bean B 9 frames after release, reaches the Piranha Plant and vine,
  and gets to the Fruit Peel. That run then lost its last lives to Fruit Peel
  hazards; this is a separate issue, not investigated here. Full ctest 89/89.

## Broader integration coverage

- Story run 6 traversed Peach's Castle Garden, played both full prologue movies,
  and reached HeavensDoorGalaxy with 60 consecutive ready frames. **The user
  reported intervening manually a couple of times, with IMG_2242.mov in Downloads
  as evidence. This is an assisted integration pass, not an unattended route
  pass.** The driver now detects bound physical key/button edges and reports ASSISTED
  (exit 3) instead of PASS when helped. Story run 7 completed the revised route
  with no physical gameplay inputs, including three automatic recoveries, reached
  the stage for 60 ready frames and exited 0. Pointer movement and focus changes
  are reported separately and do not steer this route.
- Early route failures were driver errors: reading the contact normal as field
  gravity, aiming at a crystal cage, an insufficient movie timeout, and requiring
  a player during scene teardown. Regressions cover these cases. The driver now
  requires HeavensDoorGalaxy to remain ready for 60 consecutive frames.
- Story run 5 completed PrologueB after 7,204 frames and attempted
  HeavensDoorGalaxy at frame 20,425. Loading then hit a real allocation failure:
  the file-cache solid heap had 640,320 bytes free and needed 1,269,376 for the
  native copy of heavensdoormysteriousplanet.bdl. Native image copies consumed space beyond the Wii archive-placement budget.
  A companion heap with the file cache's lifetime fixes this failure. In story
  run 6, the cache had 6,680,160 bytes free and its companion had 9,691,200 free
  after HeavensDoorGalaxy initialization. Root loader-routing and heap-lifecycle
  tests pass; the latter loads 52 archives and 304 model/animation files, but
  does not model the full asynchronous archive collector or holder deduplication.
- Story run 5 audio: 341 per-second reports, zero underrun frames, one expected
  silent startup replay and zero subsequent replays. No reported baton wait
  exceeded 5 ms. THP video decode and validation now release the game CPU baton
  during pure host work. The intermittent loader waits from prior runs did not
  reproduce in that run; later story-7 measurements below provided new evidence.
- Earlier timing magnitudes were confounded by stale CPU-consuming test
  processes, which were stopped. Those figures are not quiet-machine benchmarks.
  The later story run used no stack suspension and no concurrent test jobs.

- Story run 6 had zero underruns across 373 audio reports, but 18 replayed blocks
  clustered in startup/title/picture-book transitions (one is the known silent
  startup repeat). No later replays occurred through either movie or HeavensDoor.
  These intermittent early replays remain unexplained; the clean story-5 result
  does not establish consistently clean startup audio.

- Story run 7 is a route/loading pass, **not a clean-audio result**: 360 reports
  contained 10 replayed blocks (including startup) and one burst of 13,486
  underrun frames near the end of PrologueA. The new audio timing diagnostic
  separates interrupt delivery, DMA registration and DSP work. The intermittent
  audio failures prompted the synchronization changes below. A load-sensitive
  real-time pacing test remains an unresolved limitation.

## Audio synchronization follow-up

Story-7 registration delays matched four CPU-baton waits of 20–53 ms. The
holders received almost no CPU time; available mid-wait samples showed them
runnable. The native scheduler now requests a temporary user-interactive host
QoS override while a higher-priority game thread waits on the running holder.
It detaches the override when the dependency ends and ends it after releasing
the interrupt mutex. Emulated game priorities are unchanged.

The AI registration busy-spin lock is now an owner-aware `os_unfair_lock`.
Diagnostic formatting and stderr writes moved off the real-time producer to a
joined reporter thread. These remove concrete priority-inversion and blocking
hazards, but do not establish the cause of the story-7 underrun. New interval
counters record producer gaps/pull time and device callback gaps/requests.

Fresh Release, ASan and TSan component runs passed: 8,443 OS checks (including
override handoff, withdrawal and host-blocking lifecycle) and 56 AI checks
(including concurrent registration/latching without torn blocks). Timing
diagnostics also have generation rollover and real DSP integration coverage.
These checks establish synchronization behavior, not a latency guarantee.

## Latest integrated result

Story run 8 stopped at file selection after physical Space and T events during
startup; it correctly reported assistance with its failure. It is not a route
or audio acceptance result.

Story run 9 (`build/proactive-story-9.log`), with the synchronization changes,
passed at frame 20,242 and exited 0. Both movies completed and HeavensDoorGalaxy
remained ready for 60 frames. No physical gameplay input and no stuck recoveries
were recorded; 44 pointer movements and two focus changes were reported
separately. Both isolated NAND files retained their original SHA-256 hashes.
The cache and companion had 6,680,160 and 9,691,200 bytes free respectively.

Across 342 audio reports there were zero underrun frames and three repeated
blocks: the known silent startup repeat and **two unexpected repeats**. The
minimum ring level was 2,049 frames. Worst producer tick gap was 1,061 us and
worst pull duration 13 us; the largest reported baton wait was 11.2 ms.
One unexpected repeat followed PrologueA and coincided with an 11.2 ms baton
wait/registration lag. The other, during PrologueB, coincided with 24.8 ms DSP
interrupt delivery and an 81.1 ms DSP frame; subframe computation remained
short. These are interval correlations, not proof of a unique cause.

This is a second unassisted route pass and a no-underrun observation after the
synchronization changes. It does **not** establish consistently clean audio:
the two repeats and the load-sensitive pacing test remain open. Do not infer a
causal improvement solely from comparing these separate runs.

## Repeatable asset and controls coverage

- Full movie validation passes for PrologueA (5,591 frames) and PrologueB
  (7,076 frames), including every video frame and all audio records. Sampled
  decoded video matches the independent decoder above 51 dB PSNR.
- The movie wrapper test covers all nine movies for 150 frames each, comparing
  drawn Y/U/V and PCM. Its OS-thread mode verifies that a ready lower-priority
  thread makes progress during decode; disabling the release in a private copy
  fails that check. The progress count can undercount actual handoffs.
- Root `native_j3d_material_anm_corpus` passes: 524 model archives and 1,287
  BTP/BRK/BPK/BTK bindings, with no failures or unmatched animations. The old
  pointer-range check fails all 1,287 bindings.
- Keyboard regressions cover quick jump-then-spin, a bounded queued shake for
  rapid taps, focus-loss cancellation and Left Alt half-strength walking.
  Root input/smoke/seam tests pass. These do not establish late-game motion-ride
  usability or every aspect of Mario's spin animation timing.

## Still unverified

Complete story progression, all stars/galaxies/bosses, motion activities,
long-session memory behavior, every camera interaction, every rendered effect,
and perceptual audio fidelity. Component tests and asset sweeps reduce specific
risks but do not establish these behaviors. New failures should be fixed and
recorded here with their reproducer and relevant regression, without relying on
the user to report them first.


## Observatory / first-mission work in progress

The explicit isolated observatory fixture supplies tutorial progression; it
must not be counted as earned tutorial completion. Observatory run 3 walked
from spawn into the Terrace without physical gameplay input or stuck recovery,
activated the Pull Star, and advanced three visible lecture pages. It then
crashed in `GalaxyNamePlate::show` / `setAnimFrameAndStopAdjustTextWidth` before
selecting Good Egg. The first-mission integration goal is not yet passed.

An earlier observatory run exposed a separate Aurora shader-generation abort
when an indirect texture order referenced disabled coordinate 1. The native
patch applies coordinate-0 fallback in analysis and generation. Source-generation
regressions and 454 captured configurations pass; startup Metal compilation of
the seeded configurations also completed in the app.

Pipeline preparation is now before game startup: 30.85 seconds for a fresh
cache and 1.32 seconds on the next launch. This covers known configurations
only. Run 3 still encountered 18 new blocking pipeline resolves totaling 27.27
seconds (largest 4.61 seconds) during the newly exercised Terrace sequence.
Cold-path frame drops remain; a warm-cache route comparison is pending.
Run 3 had zero device underrun frames and eight replayed AI blocks, including
the expected silent startup replay. It is not a clean audio result.

### Observatory runs 5 and 6 (2026-09-29)

The galaxy observer now distinguishes a newly available galaxy
(`Galaxy.Unlock<Name>`, first click reveals it) from a selectable open galaxy.
The driver regression covers New → reveal without input → Open → confirmation
and the already-open path (292 driver checks).

Run 5 (`build/observatory-5.log`) unlocked, revealed and selected Good Egg,
reached mission selection and clicked mission 1, then produced no frame for
60 s while loading (watchdog exit 124). During the stall the audio host replayed
~57 blocks/s, so the emulated side had stopped entirely. No stacks were captured.
This is an open, intermittent hang; see run 6.

Run 6 (`build/observatory-6.log`, same fixture, launched with
`build/run-with-hang-sampler.sh`) **passed**: observatory walk, Terrace, Pull
Star, unlock/reveal/select Good Egg, mission 1 selected by the scenario UI,
stage ready, then idle, jump, opposite moves, pause (no movement while paused)
and resume. Pointer targets drove the UI; no physical input was reported.
This is an initial gameplay smoke, not a completed mission or star.

Performance was poor on this cold Good Egg path: 65 blocking pipeline resolves
totalling 31.5 s, 63 of them over 100 ms. Immediately after mission selection,
~30 serial compiles of 200–840 ms produced an 11.1 s frame interval, and
`handleGXAbortAlarm` logged a false-positive "GX abort: no processor progress
and no pipeline compilation" between two compiles (the frame was aborted and
the game recovered). The unrelated host load was high (load average ~30), which
inflates compile times, but serialization is the multiplier. Audio: one replay
(startup) over the run.

Correction: run 5's log contains no "GX abort" or "GX wait extended" lines during
its hang, so the false-positive abort did not cause it (an earlier version of
this note said otherwise). The abort itself is now fixed: the wait check
classifies processor state and never aborts a busy processor or one whose CPU
baton holder is doing host work (`native_platform_gx_sync`). Run 5's cause
remains unknown; the leading hypothesis is a game thread blocked on the GX FIFO
buffer mutex while holding the emulated-CPU baton during a compile. The smoke
watchdog now samples host stacks and dumps OS/GX state before exiting, so the
next occurrence should explain itself.

### Observatory run 7: integrated stall fixes (2026-09-29)

Checkpoint `da36808e5` plus the stage-ABI allocation fix. The first attempt
crashed at boot: `petari_gx_pipeline_stage_wait` allocated through global
`operator new` on the game thread (JKR heap). Stage ABI entry points now open a
`HostAllocationScope`.

Run 7 (`build/observatory-7.log`, CSV `build/observatory-7-frames.csv`) used a
new fixture with empty app pipeline/Dawn caches (the macOS Metal shader cache
was not cleared) and `PETARI_PIPELINE_SEED_DIR=build/pipeline-seeds`
(shared-observed seeds, which already include run 6's Good Egg configurations).
It **passed** the same Good Egg mission 1 route. Blocking pipeline resolves:
**1 (519 ms)**, versus 65 totalling 31.5 s in run 6; every stage-ready gate
completed with <0.05 ms residual wait. Frame intervals: p50 16.61, p95 20.81,
p99 26.03 ms; 309/6421 late (>20.85 ms), 27 over 33.3 ms. Late frames are
dominated by draw-done wait (EFB round trips) and VI timer overshoot; the worst
frame was a 1.59 s draw-done wait at mission entry with no counted pipeline
resolve (under investigation). Audio: 1 replay (startup), 0 underrun frames,
0 DMA waits, 0 DSP holds, shortest block ~16.0 ms. Host load average ~20 from
unrelated applications. Seeds were derived from earlier runs of this same
route, so this does not demonstrate coverage for unvisited stages.

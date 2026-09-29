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

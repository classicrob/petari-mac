# Texture format coverage

The CPU target `petari_texture_format_tests` calls Aurora's actual texture and
palette converters. `native_texture_format` passed after the app build on the
resumed checkpoint (31 conversion calls, 0.31 seconds). No GPU is initialized.
It covers all eleven base GX formats, the three depth-sampling destinations,
all nine C4/C8/C14X2 × IA8/RGB565/RGB5A3 palette combinations, and four PC
formats with the backend direct-upload capability flags both off and on.
Depth cases test the mapped destinations, not execution of the LOAD_TEXOBJ
command itself. The launch-star smoke is the integration evidence for that
command-path normalization. Pixel payloads in this test are synthetic; this is
format/path coverage, not an image-fidelity or exhaustive decoder proof.

## Disc inventory

Run `texture_format_inventory.py` with no concurrent builds (it is CPU heavy). It parses BTI,
J3D TEX1, JPA TEX1, TPL and RFNT/TGLP headers inside nested RARC archives, retains
input SHA-256 hashes and resource offsets, and records source-level GX calls.
BTK animation blocks are not texture blocks. The AudioRes/Info/JaiMe.arc
`metable.bmt` resource is an audio ME table with an extension collision; it is
explicitly listed as ignored rather than silently classified as a material.

The corrected full scan examined 2,232 candidate input files and found 8,120 headers:
7,343 J3D, 221 standalone BTI, 225 JPA, 326 TPL, and five font atlases. It completed with zero parse errors, preserving all texture counts from the
initial scan. One explicitly ignored audio table is recorded separately.
The artifact is `build/pipeline-prep-measure/resume-8c4/texture-formats-final.json`.

| Serialized format | Header count | CPU converter |
| --- | ---: | --- |
| I4 (0) | 739 | Supported |
| I8 (1) | 1,424 | Supported |
| IA4 (2) | 132 | Supported |
| IA8 (3) | 212 | Supported |
| RGB565 (4) | 245 | Supported |
| RGB5A3 (5) | 121 | Supported |
| RGBA8 (6) | 11 | Supported |
| CMPR (14) | 5,236 | Supported |

No indexed textures appeared in the serialized inventory; the generic
CI/TLUT CPU paths remain covered by the test. Observed wrap modes were 0/1/2,
minification filters 0/1/5, magnification filters 0/1, and anisotropy 0, all
supported by Aurora's sampler conversion switches.

## Runtime formats and copy paths

`src/Game/System/MainLoopFramework.cpp:74` and
`src/Game/Util/DrawUtil.cpp:159` initialize the 4×4 Z24X8 depth-clear textures.
`native/gx/patch_aurora_texture.py:24` strips the ZTF flag for sampling of
non-CTF formats, matching the texture-unit format bits. Z8/Z16/Z24X8 therefore
sample as I8/IA8/RGBA8; GXGetTexObjFmt and Z-texture setup retain the original
format. This is the goodegg worker's fix, not a new change in the inventory.

Known runtime copy requests are:

| Copy format | Source examples | Renderer path |
| --- | --- | --- |
| RGB565 | MarioActorSpecialDraw:142, OdhConverter:52, GalaxyMapController:637 | RGB565 copy conversion |
| RGBA8 | ScreenBlurEffect:120, BloomEffect:84 | Passthrough or scaling/alpha blit |
| I8 | BloomEffectSimple:66, BloomEffect:107 | I8 copy conversion |
| Z8 | ScreenBlurEffect:121, DepthOfFieldBlur:55 | Depth-to-I8 conversion |
| CTF_A8 | ScreenAlphaCapture:39, MarioShadow:460, FallOutFieldDraw:118 | Alpha-channel copy conversion |
| CTF_RA8 | NormalMapBase:908 | Red/alpha copy conversion |
| CTF_R8/G8/B8 | BloomEffectSimple:69/72/75, DepthOfFieldBlur:52 | Channel copy conversion |

Paths in the table are under `src/Game`. CTF copy formats are not fed to the
CPU sampling converter: the destination is the registered EFB copy texture.
Their GPU conversion pipelines are defined in Aurora
`lib/gfx/tex_copy_conv.cpp:309`; depth Z8/Z16 entries are at line 326.
`lib/gfx/encoding.cpp:258` selects conversion versus blit/direct copy.
`MR::nonFilteredCapture` forwards a dynamic JUTTexture format (DrawUtil:460),
and its only found caller is FullScreenBlur:36; MarioActorDraw:127 creates
that texture as RGB565. ImageEffectLocalUtil forwards formats selected by its
callers; BloomEffectSimple's TexSpec selects RGB565 at lines 55 and 90.
This is source review, not GPU pixel validation of every copy mode.

## Remaining fatal-path boundaries

Aurora's CPU converter still rejects unknown texture formats; the TLUT helpers
reject unknown palette formats. No additional unsupported serialized format
was found in the corrected scan. Sampler wrap/filter/anisotropy defaults remain
fatal for invalid values, but every inventoried value is supported.

Depth copies from a multisampled EFB remain explicitly fatal in upstream
`lib/gfx/encoding.cpp:264`. The game requests Z8 copies, so enabling MSAA would
require reviewing/fixing this path before claiming support. This is a
conditional capability gap, not a newly reproduced default-settings crash.

GX logic blending supports CLEAR/COPY/NOOP and rejects other logic operations
(`lib/gx/gx.cpp:139`). JPA's resource table can encode all sixteen operations
(`src/JSystem/JParticle/JPABaseShape.cpp:1036`), so the inventory now audits
BSP1 blend/logic indices and bounds as well as texture headers. All 3,327
base-shape records passed: no unsupported logic operation or out-of-range
blend-factor/TEV-color-argument table index was found. This audits static
resource states, not every state a particle callback could mutate.
Unsupported enum defaults in shader generation are covered only for the tested
config corpus: all 8,355 GX configs generated five legal output variants, and
the global-cold run compiled the default runtime layout successfully. That is
not proof against unvisited runtime material mutation or other layouts.

The inventory records dynamic source expressions instead of pretending to
resolve them. Unknown containers can hide resource headers. Other renderer
failures due to resource sizes, malformed data, allocation failures or GPU
validation are outside this format audit; no assertion of full-game safety is
made from these checks.

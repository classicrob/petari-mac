# Display copies and XFB presentation

The window shows what the Wii's video interface would scan out: the external
framebuffer (XFB) that VI latched, blanked while VI output is black and
darkened while VI dimming is on. It does not show the EFB as it happens to be
when Aurora ends its frame.

## How it works

1. **Display copy, in FIFO order.** `patch_aurora_present.py` rewrites
   Aurora's `GXFrameBuffer.cpp`.
   - `GXCopyDisp(xfb, clear)` writes Aurora's copy commands into the FIFO:
     source rectangle, destination size, destination address, then the copy
     trigger (BP 0x52) with the clear bit.
   - When the command processor reaches them, Aurora's `copy_tex` resolves
     the EFB rectangle into an RGBA render texture. That texture is kept in
     `g_gxState.copyTextureCache` and `g_gxState.copyTextures[xfb]`, so each
     XFB address has its own GPU texture, reused frame to frame.
   - The snapshot is the EFB at the copy's place in the stream. Later draws,
     clears and copies do not change it.
   - The clear (color, alpha and depth, per the update masks) covers the
     copied rectangle, as on hardware.
   - The copy is upscaled like texture copies.
   - Display and texture copies have separate registers on hardware, but
     share Aurora's command-processor copy state. After a display copy, the
     game's last `GXSetTexCopySrc`/`GXSetTexCopyDst` are loaded again.
2. **Display-copy state.** These setters now keep real state:
   - `GXSetDispCopySrc` and `GXSetDispCopyDst` (only the width counts, as on
     hardware);
   - `GXSetDispCopyYScale`, which returns the hardware line count through
     `GXGetNumXfbLines`;
   - `GXSetCopyFilter`, `GXSetDispCopyGamma`, and `GXSetCopyClamp` (which
     upstream Aurora lacks).

   The XFB copy has `lines = GXGetNumXfbLines(source height, Y scale)` rows.
3. **Choosing what to present.** At the frame seam, the app calls
   `composeFrame` (`present_host.cpp`, SDK side). It reads `VI::displayState()`:
   the XFB latched at the last retrace, black, configured, dimmed. By default
   it then presents the newest display copy (`GXCopyDisp`) instead of the
   latched XFB: that copy is in the frame's FIFO, drained before Aurora
   presents, and VI would only latch it about two frames later. Measured
   (`PETARI_LATENCY_PROBE=1`, `petari/latency_probe.hpp`): press to present
   p50 81 -> 48 ms with the same frame pacing. `PETARI_PRESENT_XFB=latched`
   restores the Wii's own scan-out timing. VI black and dimming apply either way. It also
   reads the SC aspect (16:9 or 4:3), and hands everything over through
   `present.h` (plain C, so Aurora's and the SDK's GX headers never meet).
4. **Present.** The patched `aurora_end_frame` calls `petari_present::take()`
   after the FIFO is drained.
   - That looks up the latched XFB's copy texture and keeps a reference, so
     the texture lives until presented.
   - The present pass clears to black and then draws that texture into a
     viewport of the display aspect. The game draws 16:9 anamorphically into
     a 640-wide XFB, so the aspect, not the texture size, decides the shape.
   - Nothing is drawn when VI is black, no render mode is latched, or the XFB
     has no GPU copy.
   - While dimmed, black is blended over the image.
   - ImGui (the Home menu) draws afterwards.
   - Until `petari_present_install()` runs, the RmlUi-replaces-game branch
     and upstream EFB presentation are unchanged.
5. **Render-target aspect.** While XFBs are presented, `patch_aurora_present.py`
   makes `aurora::window::get_window_size` fit the EFB render target to the
   displayed aspect (16:9 or 4:3) inside the drawable. The XFB copy then has
   exactly the presented viewport's size and is drawn pixel for pixel, in any
   window shape. Previously the EFB took the window's shape and a mismatch was
   resampled: 16:9 in a 4:3 window at 1.00 x 0.75, a 16:10 fullscreen at about
   1.00 x 0.87. A normal 16:9 window (1280x720 points, 2560x1440 on Retina) was
   already 1:1 and is unchanged. Changing the aspect requests an Aurora
   frame-buffer resize. Aurora drops all copy textures on any EFB resize, as
   for a window resize, so the latched XFB shows black for one frame until the
   game copies it again.
6. **Image rectangle.** `imageRect` returns that viewport in window points,
   for the pointer (`Input::setViewport`) and the overlay.

The XFB on screen is the one VI latched: with the game's triple buffering,
the copy from the previous frame, as on the console.

## Measuring (PETARI_TRACE_BOOT)

- `Petari present: surface WxH, EFB WxH, XFB texture WxH, viewport WxH (a:b),
  scale sx x sy` whenever these change. A scale of 1.00 x 1.00 means the XFB is
  shown pixel for pixel.
- `[gx copy] display|texture WxH -> WxH, format F` once per distinct copy
  (logical GX sizes; the texture is that scaled to the render target). Effect
  copies drawn back over the scene at a reduced size show up here.

Measured (component test, 960x720-point window, 1920x1440 drawable):

- The 640x456 game mode gives XFB 1920x1440 in 4:3 and 1920x1080 in 16:9.
- A drawn edge at logical x 100.4 is a hard 0 -> 255 step at texel 301 (expected
  301.2). A 640-wide image scaled up would show a ramp.

Source art is lower resolution and looks soft when magnified; that is not a
rendering loss:

- the prologue storybook pages (`LayoutData/PrologueDemo.arc`,
  `prologuecomet1-5.tpl`) are 608x224 CMPR, drawn about 608 logical pixels
  wide, roughly 4.2x magnified at 2560 wide;
- UI icons are 24-64 px (`PauseMenu.arc`, `FileSelect.arc`,
  `StarCounter.arc`).

## Fidelity limits

- **Copy filter.** The AA sample pattern and the 7-tap vertical (deflicker)
  filter from `GXSetCopyFilter` are stored but not applied. The copy is a
  plain resolve, so the image is slightly sharper than on a TV.
- **Gamma.** Display gamma other than 1.0 is stored but not applied; a
  one-time warning is printed. SMG uses 1.0.
- **Clamp.** `GXSetCopyClamp` is stored only. It affects filter taps at the
  image edges, and no filter is applied.
- **CPU writes to XFB memory.** `JUTDirectPrint`, the debug console, and XFB
  memory written by the CPU are not shown: an XFB is GPU texture, not memory.
  An XFB with no GPU copy presents black, with a one-time message per
  address.
- **Dimming level.** Black at 50% opacity. The SDK does not document the
  hardware level.
- **Scaling to the window.** Bilinear through Aurora's copy pipeline, not
  Aurora's area resampler. The copy is already at render resolution, so the
  scale factor is near 1.
- **Interlacing and field rendering.** Not modelled: `GXSetDispCopyFrame2Field`
  remains a TODO, and XFBs are presented as progressive frames.

## Files

| File | Side | Role |
| --- | --- | --- |
| `../patch_aurora_present.py` | generator | patches `lib/dolphin/gx/GXFrameBuffer.cpp` (original) and `lib/aurora.cpp` (after `patch_aurora_allocations.py`) |
| `present.h` | C ABI | `petari_present_enable`, `petari_present_set_video`, `petari_present_image_rect` |
| `present_aurora.hpp`, `present_aurora.cpp` | Aurora (`-iquote <aurora>/lib`) | `take`, `bind_image`, `draw_dim`, and the C ABI |
| `present_host.cpp` | SDK/platform | `petari_present_install()`: the app's `PresentHooks` from VI and SC |

## Wiring (root)

- `aurora.cpp`: run `patch_aurora_allocations.py`, then `patch_aurora_present.py`.
- `window.cpp` (`aurora_core`): replace it with the patched original
  (`patch_aurora_present.py`). No other generator touches it.
  The output includes `"present_aurora.hpp"`, so give it
  `-iquote native/gx/present`.
- `GXFrameBuffer.cpp`: replace it in `aurora_gx` with the patched original,
  compiled with `-iquote <aurora>/lib/dolphin/gx` and `-iquote <aurora>/lib`.
- Add `present_aurora.cpp` to `aurora_core`, with `-iquote <aurora>/lib`.
- `present_host.cpp`: a library linked into the app, with the platform VI/SC
  libraries and the app's `setPresentHooks`. `native/app/platform_host.cpp`
  calls `petari_present_install()` right after `petari_attach_vi_renderer()`.

## Tests

- `native/tests/present_patch_tests.py`:
  - both files patch, and `aurora.cpp` composes after the allocation patch;
  - unchanged outputs are not rewritten;
  - moved anchors fail without writing output;
  - unknown files are refused.
- `native/tests/present_gpu_tests.cpp`, on a real Metal device:
  - display copies snapshot the EFB at their FIFO position (red, then green,
    across clears and a texture copy);
  - a texture copy's clear covers only its rectangle;
  - re-copying an address replaces its image, and each address has its own
    texture;
  - texture-copy destinations survive display copies;
  - the Y-scale line counts;
  - presented-XFB selection (latched copy, VI black, no render mode, unknown
    or null XFB, dimming, 16:9 and 4:3);
  - the image rectangle in window points;
  - three full presents through the patched `aurora_end_frame` (dimmed,
    black, missing XFB) with no device error;
  - presenting leaves the XFB copy unchanged.

  Each run starts with an empty pipeline cache.

```sh
cmake -S native/gx/present -B build/present -DCMAKE_BUILD_TYPE=Release
cmake --build build/present -j8 --target petari_present_gpu_tests
ctest --test-dir build/present --output-on-failure
```

The standalone build uses the local `aurora-reference` and the dependency
*sources* in `build/aurora/_deps`, with its own binary directories under
`build/present`. It applies root's `patch_aurora_pipeline.py`, so first-use
copies are not dropped while a pipeline compiles. It also gives render
textures `CopySrc` usage, for test readback only; the product patch doesn't
change texture usage. The GPU test needs a window, so it must run outside a
sandbox that blocks the window server.

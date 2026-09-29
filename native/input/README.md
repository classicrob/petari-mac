# Native keyboard and mouse input

`petari_input` implements the SDK's Wii Remote API (`<revolution/wpad.h>`) for
a virtual Wii Remote with a Nunchuk, driven by the keyboard and mouse. The
SDK's own `src/RVL_SDK/kpad/KPAD.c` is compiled unchanged on top of it. KPAD's
pointer, accelerometer, stick, and button-repeat processing is therefore the
original code, and the game's `WPad*` classes read real KPAD output.

The public API is `native/include/petari/input.hpp`. The optional SDL3 adapter
is `include/petari/input_sdl3.hpp`.

## How game input is produced

The virtual remote sends a report every 5 ms, as a Wii Remote does. Timing
matters: KPAD's repeat delays are counted in reports, and the game detects a
swing by comparing the accelerometer with its value 20 reports earlier.

- **Buttons.** Bound inputs are OR-ed per Wii button. A press or release lasts
  at least `Settings::minimumPulseReports` reports (default 4, 20 ms).
  - KPAD compares only the newest report with the previous `KPADRead`, so a
    shorter tap could fall between two game frames.
  - Presses that arrive between reports are counted, so a double tap is two
    presses.
  - OS key-repeat events are ignored. The game's KPAD repeat (`KPADSetBtnRepeat`)
    applies instead.
- **Nunchuk stick.** WASD produce a unit vector.
  - A diagonal is (0.707, 0.707).
  - When opposite keys are both held, the one pressed last wins.
  - Raw values invert KPAD's cross clamp (15..71). Each axis is within 1/112 of
    the target.
- **Star Pointer.** The mouse position over the game image (`Viewport`) is a
  KPAD position: -1..1 across the image, y down.
  - The layer places two sensor-bar dots in the 1024x768 IR camera image. It
    inverts KPAD's `calc_dpd_variable` using KPAD's live calibration
    (`center_org`, `dpd2pos_scale`, `dist_vv1`), so the game's
    `KPADSetSensorHeight` is honoured.
  - Dot separation gives `dist` = `Settings::pointerDistance` (2 m).
  - Dots outside the camera image are omitted. If the mouse leaves the window
    or the window loses focus, no dots are sent, as when the remote points away.
  - KPAD then applies the game's smoothing (play radius 0.03, sensitivity 0.5).
    A small mouse motion is followed within about 0.01, as a Wii Remote is.
- **Accelerometer.** The remote's orientation gives gravity in KPAD axes. At
  rest it points at the screen: acc (0, -1, 0).
  - **Shake** (F) adds a sideways flick: 3 g for 20 ms, then a return over
    200 ms. With the game's filter (`KPADSetAccParam(0, 0.15)`) and swing test
    (`WPadHVSwing`: 1 g against 20 reports earlier), one flick gives one
    `isCorePadSwing` rising edge and one `mIsTriggerSwing`. Mario's
    `updateControllerSwing` sees it within 2 frames. Holding F is one spin.
    The game's own 10-frame cooldown after A or B still applies.
  - **Tilt** (hold Tab, then WASD) turns the remote up to
    `Settings::maxTiltDegrees` (45 degrees) at 360 degrees per second.
    - Forward tips it toward the screen.
    - Left and right twist it about the axis toward the screen.
  - **Posture** (T, or `setPosture`) switches between pointing at the screen
    and upright (tip up, 10 degrees toward the screen).
  - The IR dots rotate with the twist, so KPAD's check that the dots agree with
    the accelerometer horizon passes, and the pointer stays where the mouse is.
    Above 40 degrees of pitch the camera sees no sensor bar.

Checked against the game's tilt readers, using the real `WPadAcceleration`
output:

- **Ray surfing.** `SurfRayTutorial` accepts level as straight and a 45-degree
  twist as a turn; a turn needs 0.65 g across the remote.
- **Star Ball.** `TamakoroTutorial` accepts upright as raised.
  `SphereAccelSensorController` angles follow tilt: Tab+D rolls right and Tab+W
  rolls forward.

## Default bindings

| Input | Wii input | Status |
| --- | --- | --- |
| W A S D | Nunchuk stick | approved |
| Space | A | approved |
| Right mouse (hold) | A (Pull Stars and pointer grabs use A) | approved |
| Left mouse | B (Star Bits) | approved |
| F | Shake (spin) | approved |
| Left or Right Shift | Nunchuk Z | approved |
| Q / E | +Control Pad left / right (camera rotation) | approved |
| C | Nunchuk C (camera recenter) | approved |
| Escape | Plus (pause) and B (back) | approved; see below |
| Up / Down / Left / Right | +Control Pad (Up: first-person view) | provisional |
| - | Minus | provisional |
| 1 / 2 | 1 / 2 | provisional |
| F1 | HOME | provisional |
| Tab (hold) | WASD tilt the remote | provisional |
| T | Toggle upright posture | provisional |

All bindings are remappable: `Bindings::bind/unbind/clear`, with a text form
(`serialize`/`parse`, one `Action=Key:Name,Mouse:Button` line per action) for
saving. Menus use B to go back (scenario select, galaxy map, file select). The
pause menu closes with Plus. Escape therefore sends both. A known side effect
needs a gameplay check: B on the pausing frame may also fire a Star Bit if the
pointer is on screen.

## WPAD behaviour

- `WPADInit` (called by `KPADInit`) starts the reports. Channel 0 connects
  40 reports (200 ms) later, like a Bluetooth reconnection. This happens after
  the game has registered its callbacks.
  - The connect callback reports `WPAD_ERR_NONE`.
  - Four reports later the extension callback reports `WPAD_DEV_FREESTYLE`.
- KPAD then enables the IR camera and selects `WPAD_FMT_FREESTYLE_ACC_DPD`
  through the normal `WPADControlDpd`/`WPADSetDataFormat` commands.
- Commands queue (24 deep, as in the SDK) and one completes per report. Its
  callback runs then, with interrupts disabled. The same holds for status
  (`WPADGetInfoAsync`), LEDs, speaker, and DPD commands.
- `WPADDisconnect` turns the remote off. Pressing any bound input turns it
  back on. `setConnected` and `setNunchukAttached` let the application model
  other remotes and extension changes.
- `WPADControlMotor` follows the SDK, including the SC rumble setting.
  `rumbleActive(chan)` exposes the motor for host haptics.
- Remote speaker: the enable, mute, and play commands and stream pacing (one
  packet in flight) follow the SDK. Packets go to `setSpeakerSink` when the
  speaker is enabled, playing, and unmuted. Without a sink they are consumed
  and not played.
- `WPADGetSensorBarPosition`, `WPADGetDpdSensitivity`, `WPADGetSpeakerVolume`
  and the motor mode come from the native SC settings at `WPADInit`.
- Report clock: by default a 5 ms periodic `OSAlarm` on the native scheduler,
  like WPAD's manage handler. `setClock(Clock::Manual)` before `WPADInit`
  makes reports come only from `pumpReports(n)`, which the tests use. After
  a host stall, the periodic alarm skips missed periods rather than bursting.
  The remote's time then skips too: a press still lasts its minimum
  reports, but a shake or tilt covers less wall-clock time.
- Threads: host event functions may be called from any thread. They take a
  host mutex and never the interrupt lock. The report tick takes the interrupt
  lock, then the host mutex. Binding changes allocate inside
  `HostAllocationScope`.

### Deviations

- **No work memory.** `WPADGetWorkMemorySize` returns 0: there is no Bluetooth
  stack. `HeapMemoryWatcher` then makes a 0xD0-byte WPad heap, which nothing
  allocates from.
- **Battery.** It is always reported full.
- **Auto-sleep.** It is stored but never turns the remote off.
- **Miis on the remote.** `WPADReadFaceData` returns `WPAD_ERR_INVALID`
  immediately, with no callback, because a keyboard and mouse have no remote
  memory. With no remote it returns `WPAD_ERR_NO_CONTROLLER`.
  `RFL_Controller.c` then reports `RFLErrcode_Controllerfail`.
- **Not implemented.** Linking code that uses these fails:
  - `WPADSaveConfig` and the pairing and sync API (`WPADStartFastSimpleSync`
    and related functions);
  - `WPADGetAddress`;
  - the WUD and BTE internals.
- **Not linked.** Do not link `src/RVL_SDK/wpad/*.c`, `src/RVL_SDK/wud`, or
  `src/RVL_SDK/bte`.
- **GameCube pads.** The game does not use the PAD API. `Pad.c` holds only
  `__PADDisableRecalibration`, which is portable and needs no layer.
- **Classic Controller.** It is not emulated. SMG does not use it.
- **Camera.** The game has no free camera: rotation is +Control Pad left and
  right, and recenter is C. No mouse camera mode is provided.

## Tests

`native/tests/input_tests.cpp` (`ctest -R native_input`, 271 checks) runs the
SDK's KPAD.c and the game's `WPad`, `WPadButton`, `WPadStick`, `WPadPointer`,
`WPadAcceleration`, and `WPadHVSwing`. It pumps 10 reports per 3 frames and
covers:

- **Bindings.** The approved defaults, text round trip, partial remaps, errors
  that leave bindings unchanged, and remapping at run time.
- **Connection.** Status before and after init, the 200 ms connection, callback
  order, the DPD and format negotiation, and representative KPADStatus fields.
  Also covered: disconnection, reconnection by key press, Nunchuk removal, and
  a second remote.
- **Buttons.** Press, release, and OS-repeat edges; KPAD repeat at 5/12 s,
  then every 1/6 s; sub-report taps; double taps; two inputs on one button;
  Escape; and the approved buttons.
- **Stick.** Cardinal directions, diagonal normalization, last pressed wins,
  and raw axis inversion at 100 values.
- **Focus loss.** Release edges, a neutral stick, the pointer hidden, input
  ignored while unfocused, stale releases after refocus.
- **Pointer.**
  - Pillarbox and letterbox mapping to within 0.005.
  - Positions over the bars and beyond the camera.
  - The game's 5-report in-screen delay, smoothing, and motion flag.
  - `KPADSetSensorHeight` honoured.
  - Position while the remote is twisted.
- **Shake.** One swing and one Mario spin request per tap, within 2 frames. A
  held key is one spin. Repeated taps each spin. The game's cooldown after A
  is honoured. Tilting and walking make no swing.
- **Tilt.** The Ray and Star Ball checks above; no pointer while upright.
- **Device.** Rumble; the SC motor setting; status requests; speaker commands,
  pacing, the sink, and mute.
- **SDL3 adapter.** Built when SDL3 headers are found.
- **Alarm clock.** A realtime run with a non-OS host event thread.

The suite passes under ASan+UBSan (15 runs), TSan (6 runs), and Release
(5 runs), both standalone and added to the root build. Mutation checks:
changing the shake return, the minimum pulse, the camera roll inverse, the
twist sign, or diagonal normalization each fails a test.

## Integration (root)

```cmake
add_subdirectory(native/platform)
add_subdirectory(native/input)   # after platform
```

Link `petari_input` into the game. It needs `petari_platform_os`,
`petari_platform_sc`, `petari_core` (`PSMTXMultVec`), and
`petari_host_runtime`. `OSRegisterVersion`, which KPADInit calls, comes from
`petari_platform_os`.

In the application:

1. Call `Input::setViewport` with the game image rectangle whenever the window
   or its letterboxing changes. Use mouse-event units (SDL3: window
   coordinates).
2. Forward events, with `PetariNative::Input::SDL3::handleEvent(event)` from
   `petari_input_sdl3` or directly with `keyEvent`, `mouseButtonEvent`,
   `mouseMoved`, `mouseLeft`, and `focusChanged`.
3. Hide the system cursor over the image. The game draws the Star Pointer.
4. Optionally load and save `Bindings` text, forward `rumbleActive` to haptics,
   and install a speaker sink.

Standalone build:

```sh
cmake -S native/input -B build/input -DPETARI_SANITIZERS=ON
cmake --build build/input && ctest --test-dir build/input
```

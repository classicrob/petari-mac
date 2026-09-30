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
    - Mario ignores a swing that starts within 10 frames of pressing A or B.
      A flick pressed within `Settings::shakeDelayAfterButtonReports`
      (36 reports, 180 ms) of an A or B press starts when that window ends,
      so Space then F spins, at most 13 frames after the jump. Later presses
      do not extend the delay.
    - Flicks start at least 250 ms (50 reports) apart, after the previous
      one has fully returned; overlapping flicks would read as one long
      swing. A press during a flick waits for its turn; at most one waits,
      and further presses are dropped. Mashing F therefore gives a swing
      every 250 ms.
    - Only presses schedule flicks: holding F is one flick, and a tap's
      release does not cancel the flick waiting for it. Losing focus cancels
      a waiting flick, so nothing fires after focus returns. A flick already
      under way finishes its return, because cutting it off would itself read
      as a swing.
- **Walk.** While Walk (Left Alt) is held, the stick has
  `Settings::walkStickScale` (0.5) of its length.
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

## Game controllers

`padButtonEvent`, `padAxisEvent` and `padDisconnected`, fed by the SDL3
adapter from `SDL_EVENT_GAMEPAD_*`. Aurora opens connected controllers. All
controllers act as one.

- **Buttons.** `Binding::Device::Pad`, named by position (`Pad:South` and so
  on), so they remap in the text form.
- **Triggers.** Buttons at 55% travel, released at 45%.
- **Left stick.** The Nunchuk stick, analog.
  - A radial dead zone (`Settings::padStickDeadZone`, 0.2) is rescaled to full
    travel.
  - Stick keys, when held, take precedence.
  - Walk and the ride steering (tilt) apply to it too.
- **Right stick.** A Star Pointer of its own in KPAD space.
  - It moves at up to `Settings::padPointerWidthsPerSecond` (0.9) with a
    squared response, starting from where the mouse last pointed.
  - R3 centers it, and any mouse motion hands the pointer back to the mouse.
- **Release.** Focus loss and disconnection release everything.
- **Activity.** Button events count as activity for the screen saver, and
  so do stick movements past 25%.
- **Physical-input count.** The app counts bound buttons and sticks past half
  travel as physical gameplay input; the right stick counts as pointer motion.

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
| Escape | Plus (pause) | a press lasts 250 ms, so a tap pauses |
| Return, keypad Enter | Start: A; A and B together on the title prompt | approved design (title fix) |
| Backspace | B (back) | also available on left mouse |
| Up / Down / Left / Right | +Control Pad (Up: first-person view) | provisional |
| - | Minus | provisional |
| 1 / 2 | 1 / 2 | provisional |
| F1 | HOME | provisional |
| Tab (hold) | WASD tilt the remote | provisional |
| T | Toggle upright posture | provisional |
| Left Alt (hold) | Walk: half stick | provisional |

All bindings are remappable: `Bindings::bind/unbind/clear`, with a text form
(`serialize`/`parse`, one `Action=Key:Name,Mouse:Button` line per action) for
saving. Menus use B to go back (scenario select, galaxy map, file select). The
pause menu closes with Plus. Escape sends only Plus: the game refuses to open
pause while B is held. Backspace sends B for going back in menus.

## Game hints

Three screens need a Wii Remote move that one key cannot express by itself.
Native builds of the game report them each frame, and each report lapses
after 100 ms (`kGameHintReports`), so a missed "off" cannot leave a mode
stuck:

- **Title prompt** (`titlePromptShown`, from
  `TitleSequenceProduct::exeLogoDisplay`). The title starts when A and B are
  both held. While the prompt is up, the Start action (Return) presses B with
  A. Elsewhere Start is plain A: on scenario select and the galaxy map, B
  (back) is checked before A when both arrive together, so Start must never
  send B there.
- **Star Ball** (`motionControlShown(Steering::Ball)`, from
  `SphereAccelSensorController::getPadAcceleration`). The remote stands
  upright and the stick keys tilt it, as T and Tab would.
- **Ray surfing** (`motionControlShown(Steering::Ray)`, from
  `SurfRay::updateRide`). The remote stays level, whatever T chose. Left and
  right twist it; forward and back are ignored, since pitching it breaks
  `SurfRayTutorial`'s "straight".

**Pause.** `PauseButtonCheckerInGame` pauses on exactly the 12th frame + or -
is held, with A and B up. Plus and Minus presses therefore last at least
`Settings::pauseTapReports` (50 reports, 14 frames). The counter is not
updated while paused, and the pause menu closes on a new press, so the long
press can neither reopen nor close the menu by itself.

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
- **Screen saver.** VI dims the picture after five minutes without input, and
  input undims it at the next retrace.
  - On the Wii, `WPADiCheckContInputs` resets VI's idle count (via
    `__VIResetRFIdle`) whenever a report differs from the previous one.
  - The report tick does the same with `VIResetDimmingCount` for any change in
    buttons, stick, accelerometer or pointer dots. It also resets on any host
    input event since the previous tick: any key, including unbound keys and
    OS repeats of a held key, a mouse button, or mouse motion anywhere in the
    window.
  - Playing therefore never dims the screen. Only true idleness does, and the
    game can still turn dimming off (`GameSystemDimmingWatcher`), as can the SC
    screen-saver setting.
  - `PETARI_VI_DIMMING_SECONDS` shortens the idle time for live checks.
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
  host mutex and never the interrupt lock. KPAD's pointer calibration reaches
  the report tick through a copy made when `KPADRead` probes the remote on the
  thread that called `KPADInit`. The report tick takes the interrupt
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

`native/tests/input_tests.cpp` (`ctest -R native_input`, 496 checks) runs the
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
- **Shake.**
  - One swing and one Mario spin request per tap, within 2 frames.
  - A held key is one spin; taps 0.6 s apart each spin; tilting and walking
    make no swing.
  - Space then F after 0 to 12 frames, at each frame phase: exactly one spin
    by Mario's `updateControllerSwing` rules. With the delay off, all
    seven cases of F within 6 frames lose the spin.
  - Mashing every 50 to 300 ms: swing rising edges at least 250 ms apart,
    about one per 250 ms, each one a spin request under Mario's cooldown,
    and none trailing the last press by more than one wait.
  - Six presses within 200 ms give two flicks. Holding F for 2 s gives one.
  - A released tap still flicks in its turn. Focus loss cancels the waiting
    flick, checked against a control with focus kept.
  - Space then mashed F: the first spin comes after the lockout, then
    further spins every 250 ms.
- **Walk.** Half-length straight and diagonal sticks.
- **Screen saver.** These run the real VI with a test retrace clock, one
  retrace per frame.
  - Six minutes of each input alone never dim the screen: Space taps, W held
    with OS repeats, mouse motion over the game, mouse motion over the
    letterbox bar, and an unbound key.
  - A changing report with no host event (the remote turning upright) resets
    the count.
  - Six idle minutes dim at 5:00, and a key press undims at the next retrace.
  - Mutation checks: each of these fails a test:
    - no reset call;
    - host events ignored;
    - report changes ignored.
- **Start (Return).**
  - Plain A outside the title, even when held.
  - On the title prompt, a replica of `exeLogoDisplay` built on the game's
    `TriggerChecker`:
    - a tap starts it;
    - a key held before the prompt starts it on the next frame;
    - B stays steady while the key is held;
    - B lapses 5 to 8 frames after the prompt stops.
  - Space alone or Backspace alone never starts it; hold Space, then
    Backspace, does.
  - Focus loss releases Start, it counts for the jump-then-spin delay, and it
    can be remapped.
- **Pause tap.** A replica of `PauseButtonCheckerInGame` and the pause menu:
  - an Escape or Minus tap pauses, and a second tap resumes without
    reopening;
  - holding either key pauses once;
  - A taps stay short.
- **Ride steering.**
  - Star Ball: raised without T, D rolls right and W forward without Tab,
    the stick stays neutral, and the posture chosen with T is left alone.
  - When the hint lapses, the remote is level again, the pointer returns,
    and W moves Mario.
  - Ray: level, W held still counts as straight, and A/D turn left/right.
  - A stray T before the Ray still leaves it level.
- **Tilt.** The Ray and Star Ball checks above; no pointer while upright.
- **Device.** Rumble; the SC motor setting; status requests; speaker commands,
  pacing, the sink, and mute.
- **SDL3 adapter.** Built when SDL3 headers are found.
- **Alarm clock.** A realtime run with a non-OS host event thread.

The suite passes under ASan+UBSan and Release. Under TSan, 5 of 5 runs are
clean (2026-09-29). Earlier TSan runs reported, in about one run in six, a race
that is now fixed and has a regression test:

- the report tick read KPAD's pointer calibration from `inside_kpads`;
- `KPADInit` and `KPADSetSensorHeight` write it on the KPAD thread without
  the interrupt lock.

The tick now uses a copy taken in `WPADProbe`, which `KPADRead` calls on that
thread under the lock. `testCalibrationThreads` reported the race in 5 of 5
runs before the fix.

The unchanged `KPAD.c` shares other `inside_kpads` state between its sampling
callback (an interrupt on the Wii, the report tick here) and `KPADRead`
without a lock. TSan reports this once calls such as `KPADSetSensorHeight`
overlap a connected remote's reports. That behaviour is the SDK's, and this
suite does not exercise it. Mutation checks:
changing the shake return, the minimum pulse, the camera roll inverse, the
twist sign, or diagonal normalization each fails a test. Each of these
mutations also fails a test:

- Start never adding B, or always adding it;
- a title prompt that never lapses, or is not re-checked per report;
- no pause minimum;
- no steering tilt;
- the Ray standing upright, or keeping forward tilt;
- the stick not zeroed while steering;
- a steering hint that never lapses.

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

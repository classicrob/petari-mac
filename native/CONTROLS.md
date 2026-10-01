# Native controls

These main defaults were approved for the native Mac port. The native input layer
and text-file remapping are implemented. Opening movement, jumping and menu
selection have been exercised in the full game; later controls remain unverified.

| Input | Action |
| --- | --- |
| WASD | Move |
| Space | Jump / confirm |
| Return | Start / confirm (the title's "press A and B") |
| F | Spin |
| Shift | Crouch / ground pound |
| Mouse motion | Move the Star Pointer / collect Star Bits |
| Left mouse button | Shoot Star Bits |
| Hold right mouse button | Interact with pointer targets / Pull Stars |
| Q / E | Rotate camera |
| C | Recenter camera |
| Escape | Pause; tap again to resume |
| Backspace | Back in menus |

Bindings are loaded from `controls.txt` in the app's user directory (normally
`~/Library/Application Support/Petari`, overridden by `--user`). Mouse motion
controls the Star Pointer by default. In the game, F1 (or `, the key under Escape) then Controls lists
every control as currently bound, remaps included. While the title screen asks
for A and B, a bar at the top of the screen names the keys ("Keyboard: Return
starts | F1: all controls").

Additional development defaults provide the remaining remote controls. These
have input-component tests but have not been approved through gameplay:

| Input | Action |
| --- | --- |
| Arrow keys | D-pad; Up enters first-person view where supported, Down or Space leaves it |
| F1 or ` | Native Home menu: Resume, Controls (every key as currently bound), Mods (with Level Select, native/LEVEL_SELECT.md), Restart from Title, Quit |
| G / V | Optional mods (off by default; F1 → Mods): collect visible Star Bits / fire a Star Bit at the nearest enemy. See native/MODS.md |
| J / L, I / K, Z / X, Command or middle mouse + mouse, trackpad scroll and pinch, right stick | Only with the Odyssey camera mod on (F1 → Mods → Odyssey camera): orbit, pitch, zoom. C recentres. See native/MODS.md |
| Minus | Remote Minus (also pauses) |
| 1 / 2 | Remote 1 / 2 (the game never needs them) |
| Keypad Enter | Same as Return |
| Hold Tab + WASD | Tilt the remote by hand (rarely needed; see Star Ball and Ray) |
| T | Toggle upright remote posture by hand |
| Hold Left Alt | Walk (half-strength stick) |

## Game controllers

A connected game controller works alongside the keyboard and mouse, as
reported by SDL3's standard layout (Xbox, PlayStation, Switch Pro and similar).
Buttons are named by position. Component tests cover it; it has not been
played with a physical controller yet.

| Controller | Action |
| --- | --- |
| Left stick | Move (analog; also tilts on the Star Ball and the Ray) |
| Right stick | Move the Star Pointer; click it (R3) to center it. Moving the mouse takes the pointer back |
| Bottom button | Jump / confirm (A) |
| Right button | Back (B) |
| RT | Shoot Star Bits (B) |
| Left button, RB | Spin |
| Top button | Start (A and B on the title screen) |
| LT | Crouch / ground pound (Z) |
| LB | Recenter camera (C) |
| D-pad | Rotate camera (left/right); first-person view (up, down to leave) |
| Start | Pause |
| Back | Minus |
| Guide | Home menu |
| L3 (left stick click) | Collect visible Star Bits mod (off by default; native/MODS.md) |

Controller buttons remap in `controls.txt` like keys: for example
`Shake=Pad:West,Pad:RightShoulder,Key:F`. The names are:
South, East, West, North, Back, Guide, Start, LeftStick, RightStick,
LeftShoulder, RightShoulder, DpadUp, DpadDown, DpadLeft, DpadRight,
LeftTrigger and RightTrigger.

## Wii Remote moves on a keyboard

Every action the game asks for can be done with a single key or a natural
hold:

| The game asks you to | On the keyboard |
| --- | --- |
| Press A and B (title screen) | Return. Or hold Space, then press Backspace |
| Press any button (strap warning, 60 Hz prompt) | Space |
| Shake the remote (spin; climb vines; swing from plants; jump off the Ray; launch from slingshots) | F |
| Point at and grab a Pull Star or a pointer target | Put the mouse on it and hold the right button; release to let go |
| Hold B and stroke the pointer (turning platforms, pointer-steered rings, snow tiles) | Hold the left button and move the mouse |
| Hold the remote upright and tilt it (Star Ball) | Nothing extra: while you ride, WASD tilt it. Space jumps; holding it brakes |
| Hold the remote level and twist it (Ray surfing) | Nothing extra: while you ride, A and D turn. W and S do nothing. Space speeds up, F jumps |
| Press Z | Shift |
| Hold C and press Z (a developer camera; not reachable in normal play) | Hold C, then press Shift |
| Hold A to skip a movie or speed up text | Hold Space |
| Press + to pause | Tap Escape |

One Wii move is not available: holding Z and moving the remote toward or away
from the screen to zoom (`CameraDPD`).

## How the game treats these, in ways that can surprise

- **Return.**
  - It sends A and B together only while the title screen's "press A and B"
    prompt is showing (the title reports the prompt every frame, and the
    report lapses 100 ms after it stops).
  - Everywhere else it is plain A, like Space. It never sends the B that backs
    out of menus: on star select and the galaxy map, B wins over A when both
    arrive on the same frame.
- **Spin (F).**
  - Mario ignores a swing that starts within 10 frames of pressing A or B. A
    spin pressed that soon after Space, Return, or a Star Bit shot is
    therefore delayed until the window has passed, up to 13 frames after the
    jump.
  - Each F press is one shake, and shakes are at least 250 ms apart:
    - mashing F shakes about four times a second, and Mario's own spin
      timing decides how many of those become spins;
    - a press during a shake waits its turn (at most one waits);
    - holding F does not repeat;
    - switching away from the window cancels a waiting shake.
- **C.** A tap recenters the camera. While C is held, Mario does not move:
  the game zeroes the stick (`Mario` sets a draw state that
  `MarioActor::getStickValue` checks).
- **Q / E.**
  - They rotate the camera one step per press, and only where that area's
    camera allows rotation. Many areas use a fixed camera: there the game
    plays a short "can't" sound and the view stays put, as it does with a Wii
    remote. That sound means the key worked.
  - They are ignored during cutscenes and in first-person view.
  - Their directions swap while the camera is upside down.
- **First-person view (Up arrow).**
  - Up looks through Mario's eyes where the area allows it. Elsewhere the game
    plays the same "can't" sound.
  - W A S D look around (the game reads the stick, not the pointer). Holding
    Left Alt looks more slowly.
  - Down arrow or Space returns to the normal camera. Backspace does not.
- **Collecting Star Bits.** Point at them with the mouse; no click is needed
  (a left click shoots one instead). A newly scattered Star Bit becomes
  collectable after a moment, then flies to Mario before the counter goes up.
- **Right mouse button.** It is the remote's A button, so pressing it while
  the pointer is not on a Pull Star or another target makes Mario jump.
  A Pull Star keeps pulling while the button is held and lets go shortly
  after it is released.
- **Pause (Escape or Minus).**
  - The game pauses only after + or - has been held for 12 frames. Each
    Escape or Minus press is therefore held for at least 250 ms, so a tap
    pauses; holding longer does not pause again.
  - The game refuses to pause while A or B is held, or while the remote is
    moving (just after a spin, or while tilting). Let go of Space and the
    mouse buttons, then tap Escape.
  - Escape sends only Plus, and Backspace sends B separately.
- **Star Ball and Ray surfing.** While Mario rides, the game reports that it
  steers by tilt, and the keys adapt without Tab or T:
  - Star Ball: the remote stands upright and WASD tilt it.
  - Ray: the remote stays level, A and D twist it, and W and S are ignored,
    because tipping it forward would stop the surfing lesson from accepting
    "straight".
  - The Star Pointer is hidden while the remote is upright, as on the Wii.
  - When the ride ends, WASD move Mario again within 100 ms.
- **Tab and T** still work by hand for anything else that reads the remote's
  tilt. T toggles the upright posture; the Star Pointer is hidden while it is
  upright.

Keys pressed while Command is held go to macOS, not the game, so Cmd+Q and
Ctrl+Cmd+F (fullscreen) do not turn the camera or spin. Focus loss releases all held controls and hides the pointer. Key repeats come
from the game's KPAD logic, not from the operating system.

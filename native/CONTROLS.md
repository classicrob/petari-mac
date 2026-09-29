# Native controls

These main defaults were approved for the native Mac port. The native input layer
and text-file remapping are implemented. Opening movement, jumping and menu
selection have been exercised in the full game; later controls remain unverified.

| Input | Action |
| --- | --- |
| WASD | Move |
| Space | Jump / confirm |
| F | Spin |
| Shift | Crouch / ground pound |
| Mouse motion | Move the Star Pointer / collect Star Bits |
| Left mouse button | Shoot Star Bits |
| Hold right mouse button | Interact with pointer targets / Pull Stars |
| Q / E | Rotate camera |
| C | Recenter camera |
| Escape (hold briefly) | Pause; tap again to resume |
| Backspace | Back in menus |

Bindings are loaded from `controls.txt` in the app's user directory (normally
`~/Library/Application Support/Petari`, overridden by `--user`). Mouse motion
controls the Star Pointer by default.

Additional development defaults provide the remaining remote controls. These
have input-component tests but have not been approved through gameplay:

| Input | Action |
| --- | --- |
| Arrow keys | D-pad; Up enables first-person view where supported |
| F1 | Native Home menu (Resume, Restart from Title, Quit) |
| Minus | Remote Minus |
| 1 / 2 | Remote 1 / 2 |
| Hold Tab + WASD | Tilt for motion-controlled activities |
| T | Toggle upright remote posture for Star Ball |
| Hold Left Alt | Walk (half-strength stick) |

How the game treats these, in ways that can surprise:

- **Spin (F).**
  - Mario ignores a swing that starts within 10 frames of pressing A or B. A
    spin pressed that soon after Space (or a Star Bit shot) is therefore
    delayed until the window has passed, up to 13 frames after the jump.
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
    camera allows rotation. Elsewhere they do nothing.
  - They are ignored during cutscenes and in first-person view.
  - Their directions swap while the camera is upside down.
- **Right mouse button.** It is the remote's A button, so pressing it while
  the pointer is not on a Pull Star or another target makes Mario jump.
- **Tilt and posture** are manual for now: press T before Star Ball (it asks
  for the remote to be raised) and again afterwards. The Star Pointer is
  hidden while the remote is upright.

Escape sends only Plus: the game requires a 12-frame hold and refuses to pause
while B is held. Backspace sends B separately. Focus loss releases
all held controls and hides the pointer; key repeats come from the game's KPAD
logic rather than the operating system.

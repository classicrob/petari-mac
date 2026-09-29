# Native controls

These main defaults were approved for the native Mac port. The native input layer
and text-file remapping are implemented; in-game validation is pending. The
graphics probe is not the game.

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
| Escape | Pause / back |

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

Escape currently sends Plus and B together to cover pause and back. Whether that
fires a Star Bit on the pause frame needs a gameplay check. Focus loss releases
all held controls and hides the pointer; key repeats come from the game's KPAD
logic rather than the operating system.

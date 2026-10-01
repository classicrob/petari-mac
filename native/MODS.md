# Native mods

Optional gameplay helpers. Every mod is **off** unless you turn it on, so an
unmodified game and every test run behave exactly as before.

| Mod (`mods.txt` name) | Button | What it does |
| --- | --- | --- |
| Collect visible Star Bits (`CollectStarBits`) | G, controller L3 (left stick click) | Every Star Bit on screen flies to Mario, as if the pointer had touched each one |
| Fire a Star Bit at the nearest enemy (`ShootEnemy`) | V (no controller default) | Shoots one Star Bit at the nearest enemy on screen |

## Turning mods on

- In the game: F1, then **Mods**. Each line shows On or Off; choose it to switch.
  The choice is saved at once in `mods.txt` in the user directory.
- Or edit `mods.txt` (next to `controls.txt`):

  ```
  CollectStarBits=on
  ShootEnemy=off
  ```

- For one run: `PETARI_MODS=CollectStarBits,ShootEnemy` (or `none`) overrides
  the file.

The buttons are remappable in `controls.txt` like any other action:
`ModCollectStarBits=Key:G,Pad:LeftStick` and `ModShootEnemy=Key:V`
(see native/CONTROLS.md for input names). A button press while its mod is
off does nothing and is not remembered.

## How they follow the game's rules

Both mods act only where the game itself would let the pointer collect or
shoot Star Bits: not during cutscenes, not while the Star Pointer is disabled
(for example in the file select's two-player transparency mode), and not while
the game is paused (they run in the Star Bit director's per-frame update,
which stops with the scene).

- **Collect** uses the game's own collection path: for four frames after the
  press, `StarPiece::tryGotJudge` treats every eligible Star Bit as pointed at,
  so `goToPlayer` sends it to Mario with the normal sparkle, sound and counter.
  Eligible means what the pointer could collect (within the game's collection
  distance, past its short delay after appearing) and on screen: not clipped,
  not hidden, inside the camera frustum.
- **Shoot** uses `StarPieceShooter::shoot`'s path: `StarPiece::throwToTarget` from
  the usual shot origin, the usual shot sound, one Star Bit spent. The hit, stun
  and damage are the enemy's normal response to a Star Bit. The target is the
  enemy sensor nearest to Mario that is valid, inside the camera frustum, whose
  actor is not clipped or hidden, and that the camera can see (no map polygon
  between). No target means no shot. With no Star Bits it plays the game's
  "no Star Bits" sound, as a normal shot would.

Diagnostics go to the log as `[mods] ...` lines (what was collected, what was
shot at, what it hit, and "not allowed now" / "no enemy on screen").

## Testing

- `native_input` covers the bindings, press counting, dropped presses while a
  mod is off, and `mods.txt`/`PETARI_MODS` parsing; `native_home_menu` covers the
  Mods page.
- `PETARI_MODS_AUTOPRESS=<frames>` (test driver in `app_main.cpp`) presses the
  collect button every `<frames>` gameplay frames and the shoot button halfway
  between, so background smoke runs can exercise the mods; with the mods off the
  same presses must change nothing.

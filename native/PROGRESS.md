# My Progress

A personal record of the missions *you* cleared, kept **beside** the game save, never inside
it. On an all-unlocked save every star is already owned, so the save cannot tell you which ones
you beat yourself. This file can.

- File: `progress.json` in the user folder (the one with `NAND/`, `controls.txt`, `mods.txt`).
  One record per user folder; another `--user` folder has its own.
- Always on. It only records; it never changes the save, the game or any other file.
- Written when a Power Star is collected (the game's own star-get event), atomically (a
  temporary file, then a rename), so a crash cannot leave half a file.

## What is recorded

Per galaxy and mission (the mission number is the Power Star number: comet and hidden stars
included):

| Field | Meaning |
| --- | --- |
| `first_clear`, `last_clear` | UTC date and time of the first and latest clear |
| `clears` | how many times you collected that star |
| `best_time_s` | fastest time from stage entry to the Power Star, seconds. Time counts only gameplay frames: the pause menu and cutscenes are not timed, and a retry (death, or leaving and re-entering) starts the clock again. 60 frames per second |
| `fewest_deaths` | fewest deaths in one run that cleared it |
| `best_coins`, `best_star_bits`, `best_purple_coins` | the most coins, Star Bits and purple coins you had on a clearing run |
| `last_run` | time, deaths, coins, Star Bits and purple coins of the latest clearing run |
| `grand` | true for a Grand Star |

Purple Coin and comet missions are ordinary missions here: their time is the same
entry-to-star time (the comet's own countdown is the game's, not recorded), and purple coins are
the count on the clearing run.

## The page

F1 (the Home menu), **My Progress**. One galaxy at a time: Left/Right (or A on the galaxy name)
change the galaxy; each mission is a row with **Cleared**, its best time, fewest deaths, best
coins and Star Bits, the number of clears and the first clear date, or **Not cleared yet**. The
message line counts the missions cleared in this galaxy and in all.

## Wiping it

- On the page: **Reset progress...**, then press A again (moving the focus away cancels it).
- Or quit the game and delete `progress.json`.

Both only remove this record. The save is untouched.

## A damaged file

If `progress.json` cannot be read (not valid, or from a newer format), the game says so on the
console (`petari: progress: ...`), leaves the file exactly as it is, and does not record
progress that run. Fix or delete the file and start the game again.

## How it works (for developers)

- `native/include/petari/progress.hpp`, `native/progress/progress.cpp`: the record, the JSON
  file, and the tracker (`Progress::frame()` per game frame, `Progress::starGet()`).
- `native/app/progress_observe.cpp`: reads the game once per frame at the frame seam (the Game
  scene, player, demo and death state, coins, Star Bits, purple coins; the pause menu through its
  milestones) and implements `petari_progress_star_get`, which `PowerStar::exeStageClearDemo`
  calls next to the `PowerStar.Get` milestone.
- `native/home_menu/home_menu.cpp`: the My Progress page.
- Tests: `native_progress` (tracking, bests, pauses, deaths, file round trip, damaged files,
  per-user files, reset), `native_home_menu` (`testProgressPage`).
- Live check: `python3 native/tools/stage_sweep.py run --stages BeltConveyerExGalaxy --scenarios 1
  --warp-placement PowerStar --name progress-live` warps Mario onto the one Power Star that exists
  from the start; the run's `user/progress.json` then holds the clear and the log has a
  `[progress] cleared ...` line.

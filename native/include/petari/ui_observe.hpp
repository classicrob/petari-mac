#pragma once
// What the game shows that a player could point at, for the automated smoke
// run (native/app/smoke.hpp). Game-side native-only hooks publish; the app
// reads once per frame at the frame seam. Observation only: publishing never
// changes game behaviour. Plain C, no SDK or Aurora types.
//
// Coordinates are normalised Star Pointer screen coordinates: u = x /
// MR::getScreenWidth(), v = y / MR::getScreenHeight(), origin top-left, the
// space StarPointerController::calcPastPointingPosOnScreen maps the Wii Remote
// pointer into and the game's hit tests use.
//
// IDs (static strings): "FileSelect.Slot" (index = file number - 1),
// "Prompt.Yes" / "Prompt.No", "MiiSelect.Mario", "FileSelect.Start".

#ifdef __cplusplus
extern "C" {
#endif

#define PETARI_UI_POINTING 1u   /* the game counts the pointer as over it now */
#define PETARI_UI_EMPTY 2u      /* an empty (new) file slot */
#define PETARI_UI_SELECTABLE 4u /* the game would accept A on it now */

/* True while an observer runs (PETARI_SMOKE is set). Hooks may skip their
 * work otherwise. */
int petari_ui_observing(void);

/* A target on screen this frame. Call every frame it is shown; one not
 * published in a frame is gone. Game thread. */
void petari_ui_target(const char* id, int index, float u, float v, unsigned flags);

/* A system window (SysInfoWindow) appears: its message ID (static) and type:
 * 0 key, 1 blocking, 2 yes/no. Game thread. */
void petari_ui_prompt(const char* messageId, int type);

#ifdef __cplusplus
}
#endif

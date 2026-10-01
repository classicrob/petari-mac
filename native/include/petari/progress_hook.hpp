#pragma once
/* Game code reports a collected Power Star to the personal progress record
 * (native/PROGRESS.md). Observation only; plain C. `grand` is nonzero for a Grand Star. */
#ifdef __cplusplus
extern "C" {
#endif
void petari_progress_star_get(int mission, int grand);
/* The mission select shows mission `mission` of `stage` at (u, v) in normalized game-image coordinates, this frame. */
void petari_progress_badge(const char* stage, int mission, float u, float v);
#ifdef __cplusplus
}
#endif

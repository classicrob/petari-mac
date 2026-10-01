#pragma once
/* Game code reports a collected Power Star to the personal progress record
 * (native/PROGRESS.md). Observation only; plain C. `grand` is nonzero for a Grand Star. */
#ifdef __cplusplus
extern "C" {
#endif
void petari_progress_star_get(int mission, int grand);
#ifdef __cplusplus
}
#endif

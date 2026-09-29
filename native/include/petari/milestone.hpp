#pragma once
// Named progress points the native build reports from game code (for example
// FileSelector's title and file-select states), for start-up telemetry and the
// automated smoke run. Observation only: recording a milestone never changes
// game behaviour. Plain C, no SDK or Aurora types; any thread.

#ifdef __cplusplus
extern "C" {
#endif

/* Records that the game reached `name`, a string with static storage (a
 * literal). Printed as "[milestone] name" when PETARI_TRACE_BOOT or
 * PETARI_SMOKE is set. */
void petari_milestone(const char* name);

/* Milestones recorded so far. */
unsigned long petari_milestone_count(void);

/* The milestone with the given 0-based index, or null if it is not recorded
 * yet or has left the history (the last 64 are kept). */
const char* petari_milestone_at(unsigned long index);

#ifdef __cplusplus
}
#endif

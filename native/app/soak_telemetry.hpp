#pragma once
// Long-session (soak) telemetry: periodic, low-overhead samples of what can
// grow or drift over hours of play, written as CSV (PETARI_SOAK_CSV).
//
// - A host thread samples every PETARI_SOAK_INTERVAL seconds (default 10):
//   process footprint (phys_footprint, what Activity Monitor calls Memory;
//   on Apple silicon it includes GPU allocations), resident size, host malloc
//   in use, Metal's currentAllocatedSize, thread and file-descriptor counts,
//   cumulative audio replays and submitted audio frames, VI retraces and
//   OSGetTime against the wall clock (drift), and the frame-time percentiles
//   of the window (from gameFrame).
// - gameFrame, on the game thread at the frame seam, records frame intervals
//   and, once per interval, walks the JKR heap tree (PETARI_SOAK_CSV with
//   ".heaps.csv" appended: one row per heap: type, size, total free, largest
//   free block, depth).
// Rows are flushed as they are written, so a crash or hang keeps the history.

namespace PetariNative::App::Soak {

// From PETARI_SOAK_CSV; false (and nothing started) when unset.
bool startTelemetryFromEnvironment();
bool telemetryActive();

// Game thread, once per frame at the seam. `cycle` and `phase` are the soak
// driver's (or 0 / "" without one).
void gameFrame(unsigned long frame, int cycle, const char* phase);

}  // namespace PetariNative::App::Soak

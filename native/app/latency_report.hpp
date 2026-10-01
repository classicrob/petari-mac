#pragma once
// Input-to-present latency reporting (petari/latency_probe.hpp, PETARI_LATENCY_PROBE=1).

namespace PetariNative::App::LatencyReport {

// After Aurora initialization (main thread): finds the CAMetalLayer, applies
// PETARI_MAX_DRAWABLES, and logs its presentation settings when probing.
void configureLayer(void* window);
void logLayer(const char* when);
// Once per game frame at the seam (phase probe): prints measured presses.
void frame(bool gameplay);
// PETARI_LATENCY_TEST=1: presses A at random times during gameplay (host thread).
void startTestDriver();

}  // namespace PetariNative::App::LatencyReport

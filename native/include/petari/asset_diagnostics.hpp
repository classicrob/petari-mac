#pragma once

#include <cstdint>

namespace PetariNative {
// Required lookups report failures; optional existence probes must not call this.
void reportMissingLayoutReference(const char* layout, const char* name, const char* kind);
void reportMissingSoundReference(const char* name);
}

// Monotonic process-lifetime counters. Smoke PASS requires both to be zero.
extern "C" std::uint64_t petari_layout_missing_reference_count();
extern "C" std::uint64_t petari_sound_missing_reference_count();
extern "C" std::uint64_t petari_layout_missing_reference_unique_count();
extern "C" std::uint64_t petari_sound_missing_reference_unique_count();

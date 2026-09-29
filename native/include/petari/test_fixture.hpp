#pragma once

#include <string>

namespace PetariNative::TestFixture {
// Set by the native CLI only after validating an explicitly marked test directory.
// The fixture changes progression at file load; subsequent travel uses game logic.
inline bool observatory = false;

// Synthetic stage entry (--test-fixture stage, PETARI_STAGE, PETARI_SCENARIO):
// the observatory progression above, then the game's own after-loading galaxy
// move goes to this stage and scenario instead of the observatory. It is a test
// entry, not earned progression: other galaxies' stars and flags are unchanged.
// Empty stage: not in use.
inline std::string stage;
inline int stageScenario = 0;
}

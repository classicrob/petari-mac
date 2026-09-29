#pragma once

namespace PetariNative::TestFixture {
// Set by the native CLI only after validating an explicitly marked test directory.
// The fixture changes progression at file load; subsequent travel uses game logic.
inline bool observatory = false;
}

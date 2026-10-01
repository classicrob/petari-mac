#pragma once
// Normal-play direct galaxy entry (--stage <galaxy> --scenario <n>, native/README.md).
//
// Unlike the test fixture (test_fixture.hpp), this changes no progression: the
// player picks a file as usual, and the first file load of the session goes to
// this galaxy and mission instead of the observatory. Applied once, and only to
// a file past the tutorial (the observatory is open); otherwise the game's own
// destination is kept. Everything after that, including saving stars, is the
// game's own logic.

#include <string>

namespace PetariNative::PlayerLaunch {
// Internal stage name (e.g. "EggStarGalaxy"); empty: not in use. Set by the CLI
// before the game starts, cleared by the game once the jump is taken or refused.
inline std::string stage;
inline int scenario = 0;
}  // namespace PetariNative::PlayerLaunch

#pragma once

namespace PetariNative::App {
// Once per game frame at the frame seam (progress.hpp): reads the game, feeds Progress::frame().
void observeProgressFrame();
}

#pragma once
#include <cstdint>

namespace PetariNative::AudioSDL {
// Install before AIStartDMA. shutdown() must precede SDL_Quit and game-heap teardown.
void install();
void shutdown();
bool active();
bool outputMuted();
std::uint64_t submittedFrames();
}

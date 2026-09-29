#pragma once
// The app's side of petari/actor_observe.hpp: what was published since the
// previous take. Standard headers only.

#include <string>
#include <vector>

namespace PetariNative::App::ActorObserve {

struct Actor {
    std::string kind;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    int state = 0;
    unsigned flags = 0;
};

// Moves out the actors published since the previous call (this frame's).
void take(std::vector<Actor>* actors);

}  // namespace PetariNative::App::ActorObserve

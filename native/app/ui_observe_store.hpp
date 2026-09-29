#pragma once
// The app's side of petari/ui_observe.hpp: what was published since the
// previous take. Standard headers only.

#include <string>
#include <vector>

namespace PetariNative::App::UiObserve {

struct Target {
    std::string id;
    int index = 0;
    float u = 0.0f;
    float v = 0.0f;
    unsigned flags = 0;
};

struct Prompt {
    std::string messageId;
    int type = 0;
};

// Moves out the targets published since the previous call (this frame's) and
// the prompts that appeared.
void take(std::vector<Target>* targets, std::vector<Prompt>* prompts);

}  // namespace PetariNative::App::UiObserve

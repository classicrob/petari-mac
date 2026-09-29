// petari/actor_observe.hpp: a fixed array of plain values, filled by game-side
// hooks (game threads, no operator new: it would take game heap memory) and
// taken by the smoke run at the frame seam.

#include <petari/actor_observe.hpp>
#include <petari/ui_observe.hpp>

#include <algorithm>
#include <mutex>

#include "actor_observe_store.hpp"

namespace {

struct RawActor {
    const char* kind;
    float x, y, z, dx, dy, dz;
    int state;
    unsigned flags;
};

constexpr int kMaxActors = 256;

std::mutex gActorMutex;
RawActor gActors[kMaxActors];
int gActorCount = 0;

}  // namespace

extern "C" void petari_actor(const char* kind, float x, float y, float z, float dx, float dy, float dz, int state,
                             unsigned flags) {
    if (kind == nullptr || !petari_ui_observing()) {
        return;
    }
    std::lock_guard<std::mutex> lock(gActorMutex);
    if (gActorCount < kMaxActors) {
        gActors[gActorCount++] = {kind, x, y, z, dx, dy, dz, state, flags};
    }
}

namespace PetariNative::App::ActorObserve {

void take(std::vector<Actor>* actors) {
    RawActor raw[kMaxActors];
    int count;
    {
        std::lock_guard<std::mutex> lock(gActorMutex);
        count = gActorCount;
        std::copy(gActors, gActors + count, raw);
        gActorCount = 0;
    }
    actors->clear();
    for (int i = 0; i < count; i++) {
        actors->push_back({raw[i].kind, raw[i].x, raw[i].y, raw[i].z, raw[i].dx, raw[i].dy, raw[i].dz, raw[i].state,
                           raw[i].flags});
    }
}

}  // namespace PetariNative::App::ActorObserve

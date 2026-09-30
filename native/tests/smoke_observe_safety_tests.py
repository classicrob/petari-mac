#!/usr/bin/env python3
"""Exercise the production nullable rush observation with transition-shaped objects."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = r'''#include "smoke_observe_safety.hpp"
#include <cassert>
#include <cstring>
struct Actor { const char* mName; };
struct Sensor { Actor* mHost; };
struct Player { Sensor* _924; bool _934 = false; void* core = this; void* getMario() const { return core; } };
int main() {
    using PetariNative::App::Smoke::observedRushActorName;
    using PetariNative::App::Smoke::canObservePlayer;
    const Player* missing = nullptr;
    assert(!canObservePlayer(missing));
    assert(!*observedRushActorName(missing, true));
    Player player{nullptr};
    // Normal warp: isInRush() is true from MarioStatus_Warp, without a sensor.
    assert(!*observedRushActorName(&player, true));
    assert(canObservePlayer(&player));
    player.core = nullptr;
    assert(!canObservePlayer(&player));
    player.core = &player;
    player._934 = true;
    assert(!canObservePlayer(&player));
    Sensor sensor{nullptr}; player._924 = &sensor;
    assert(!canObservePlayer(&player));
    assert(!*observedRushActorName(&player, true));
    Actor host{nullptr}; sensor.mHost = &host;
    assert(canObservePlayer(&player));
    assert(!*observedRushActorName(&player, true));
    host.mName = "ride";
    assert(!std::strcmp(observedRushActorName(&player, true), "ride"));
    assert(!*observedRushActorName(&player, false));
    player._924 = nullptr;
    assert(!*observedRushActorName(&player, true));
}
'''
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / "test.cpp"
    exe = Path(temp) / "test"
    cpp.write_text(source)
    subprocess.run(["c++", "-std=c++17", "-fsanitize=undefined", "-fno-sanitize-recover=all",
                    "-I", str(root / "native/app"), str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("13 player/rush observation checks passed (undefined-behavior sanitizer enabled)")

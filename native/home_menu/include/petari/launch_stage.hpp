#pragma once
// The galaxies --stage accepts: internal stage names, friendly aliases taken from
// the game's English galaxy names, domes and mission counts (native/README.md).

#include <cstdio>
#include <string>

namespace PetariNative::App::LaunchStage {

struct Galaxy {
    const char* stage;    // StageData directory, e.g. "EggStarGalaxy"
    const char* alias;    // "good-egg"
    const char* name;     // "Good Egg Galaxy"
    int dome;             // 1 Terrace .. 6 Garden; 0 elsewhere (Gateway, Grand Finale, later unlocks)
    int missions;         // scenario rows, comet and hidden stars included
    const char* comets;   // comet mission numbers, e.g. "45"
    const char* hidden;   // hidden-star mission numbers
};

const Galaxy* galaxies(int* count);
const char* domeName(int dome);

// A galaxy by alias, internal name, or English name, ignoring case, spaces,
// punctuation and a trailing "galaxy" ("good-egg", "Good Egg", "EggStarGalaxy").
const Galaxy* find(const std::string& name);
// Checks --stage/--scenario; on success *stage is the internal name.
bool resolve(const std::string& name, int scenario, std::string* stage, std::string* error);
// The --stage list output.
void printList(std::FILE* out);

}  // namespace PetariNative::App::LaunchStage

// --stage name resolution and mission range checks (native/app/launch_stage.cpp).
#include "../app/launch_stage.hpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace LS = PetariNative::App::LaunchStage;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    std::string stage, error;
    check(LS::resolve("good-egg", 1, &stage, &error) && stage == "EggStarGalaxy", "alias");
    check(LS::resolve("Good Egg Galaxy", 4, &stage, &error) && stage == "EggStarGalaxy", "English name, comet mission");
    check(LS::resolve("eggstargalaxy", 6, &stage, &error) && stage == "EggStarGalaxy", "internal name, any case, hidden star");
    check(LS::resolve("bowser-jrs-robot-reactor", 1, &stage, &error) && stage == "TriLegLv1Galaxy", "punctuated name");
    check(LS::resolve("Bowser Jr.'s Robot Reactor", 1, &stage, &error) && stage == "TriLegLv1Galaxy", "apostrophes ignored");
    check(LS::resolve("battlerock", 7, &stage, &error) && stage == "BattleShipGalaxy", "seven-mission galaxy");
    check(!LS::resolve("good-egg", 7, &stage, &error) && error.find("missions 1-6") != std::string::npos, "mission past the end");
    check(!LS::resolve("good-egg", 0, &stage, &error), "mission zero");
    check(!LS::resolve("nowhere", 1, &stage, &error) && error.find("--stage list") != std::string::npos, "unknown galaxy");
    check(!LS::resolve("", 1, &stage, &error), "empty name");
    check(LS::find("AstroDome") == nullptr && LS::find("FileSelect") == nullptr, "hubs and menus are not destinations");

    int count = 0;
    const LS::Galaxy* all = LS::galaxies(&count);
    std::set<std::string> keys;
    int perDome[7] = {};
    for (int i = 0; i < count; ++i) {
        check(LS::find(all[i].alias) == &all[i], "every alias resolves to itself");
        check(LS::find(all[i].stage) == &all[i], "every internal name resolves to itself");
        check(keys.insert(all[i].alias).second, "aliases are unique");
        check(all[i].missions >= 1 && all[i].dome >= 0 && all[i].dome <= 6, "sane mission count and dome");
        ++perDome[all[i].dome];
    }
    check(count == 42, "42 galaxies listed");
    for (int dome = 1; dome <= 6; ++dome) check(perDome[dome] >= 4, "every dome has its galaxies");
    check(std::strcmp(LS::domeName(1), "Terrace") == 0 && std::strcmp(LS::domeName(0), "Elsewhere") == 0, "dome names");

    std::FILE* out = std::tmpfile();
    LS::printList(out);
    std::rewind(out);
    char buffer[256];
    std::string text;
    while (std::fgets(buffer, sizeof(buffer), out)) text += buffer;
    std::fclose(out);
    check(text.find("good-egg") != std::string::npos && text.find("[1 2 3 4C 5C 6H]") != std::string::npos,
          "list shows aliases and comet/hidden missions");
    check(text.find("Terrace") < text.find("Garden") && text.find("Garden") < text.find("Elsewhere"), "list is by dome");

    if (failures) return 1;
    std::puts("launch stage: aliases, names, mission ranges and list pass");
    return 0;
}

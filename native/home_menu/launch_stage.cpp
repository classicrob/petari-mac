#include "petari/launch_stage.hpp"

#include <cctype>
#include <iterator>

namespace PetariNative::App::LaunchStage {
namespace {

// From the disc's StageData scenario tables and the US English galaxy names
// (Message.arc GalaxyName_*). Hubs, cutscene-only stages and the prologue are
// not listed.
constexpr Galaxy kGalaxies[] = {
    {"TriLegLv1Galaxy", "bowser-jrs-robot-reactor", "Bowser Jr.'s Robot Reactor", 1, 1, "", ""},
    {"FlipPanelExGalaxy", "flipswitch", "Flipswitch Galaxy", 1, 1, "", ""},
    {"EggStarGalaxy", "good-egg", "Good Egg Galaxy", 1, 6, "45", "6"},
    {"HoneyBeeKingdomGalaxy", "honeyhive", "Honeyhive Galaxy", 1, 6, "45", "6"},
    {"SurfingLv1Galaxy", "loopdeeloop", "Loopdeeloop Galaxy", 1, 1, "", ""},
    {"BeltConveyerExGalaxy", "sweet-sweet", "Sweet Sweet Galaxy", 1, 1, "", ""},
    {"BattleShipGalaxy", "battlerock", "Battlerock Galaxy", 2, 7, "45", "67"},
    {"KoopaBattleVs1Galaxy", "bowsers-star-reactor", "Bowser's Star Reactor", 2, 1, "", ""},
    {"BreakDownPlanetGalaxy", "hurry-scurry", "Hurry-Scurry Galaxy", 2, 1, "", ""},
    {"TamakoroExLv1Galaxy", "rolling-green", "Rolling Green Galaxy", 2, 1, "", ""},
    {"CocoonExGalaxy", "sling-pod", "Sling Pod Galaxy", 2, 1, "", ""},
    {"StarDustGalaxy", "space-junk", "Space Junk Galaxy", 2, 6, "45", "6"},
    {"HeavenlyBeachGalaxy", "beach-bowl", "Beach Bowl Galaxy", 3, 6, "45", "6"},
    {"KoopaJrShipLv1Galaxy", "bowser-jrs-airship-armada", "Bowser Jr.'s Airship Armada", 3, 1, "", ""},
    {"CubeBubbleExLv1Galaxy", "bubble-breeze", "Bubble Breeze Galaxy", 3, 1, "", ""},
    {"OceanFloaterLandGalaxy", "buoy-base", "Buoy Base Galaxy", 3, 2, "", "2"},
    {"TearDropGalaxy", "drip-drop", "Drip Drop Galaxy", 3, 1, "", ""},
    {"PhantomGalaxy", "ghostly", "Ghostly Galaxy", 3, 6, "45", "6"},
    {"FishTunnelGalaxy", "bigmouth", "Bigmouth Galaxy", 4, 1, "", ""},
    {"KoopaBattleVs2Galaxy", "bowsers-dark-matter-plant", "Bowser's Dark Matter Plant", 4, 1, "", ""},
    {"SandClockGalaxy", "dusty-dune", "Dusty Dune Galaxy", 4, 7, "45", "67"},
    {"IceVolcanoGalaxy", "freezeflame", "Freezeflame Galaxy", 4, 6, "45", "6"},
    {"CosmosGardenGalaxy", "gusty-garden", "Gusty Garden Galaxy", 4, 6, "45", "6"},
    {"HoneyBeeExGalaxy", "honeyclimb", "Honeyclimb Galaxy", 4, 1, "", ""},
    {"SkullSharkGalaxy", "bonefin", "Bonefin Galaxy", 5, 1, "", ""},
    {"FloaterOtaKingGalaxy", "bowser-jrs-lava-reactor", "Bowser Jr.'s Lava Reactor", 5, 1, "", ""},
    {"ReverseKingdomGalaxy", "gold-leaf", "Gold Leaf Galaxy", 5, 6, "45", "6"},
    {"TransformationExGalaxy", "sand-spiral", "Sand Spiral Galaxy", 5, 1, "", ""},
    {"OceanRingGalaxy", "sea-slide", "Sea Slide Galaxy", 5, 6, "45", "6"},
    {"FactoryGalaxy", "toy-time", "Toy Time Galaxy", 5, 6, "45", "6"},
    {"TeresaMario2DGalaxy", "boos-boneyard", "Boo's Boneyard Galaxy", 6, 1, "", ""},
    {"OceanPhantomCaveGalaxy", "deep-dark", "Deep Dark Galaxy", 6, 6, "45", "6"},
    {"CannonFleetGalaxy", "dreadnought", "Dreadnought Galaxy", 6, 6, "45", "6"},
    {"DarkRoomGalaxy", "matter-splatter", "Matter Splatter Galaxy", 6, 1, "", ""},
    {"HellProminenceGalaxy", "melty-molten", "Melty Molten Galaxy", 6, 6, "45", "6"},
    {"SnowCapsuleGalaxy", "snow-cap", "Snow Cap Galaxy", 6, 1, "", ""},
    {"KoopaBattleVs3Galaxy", "bowsers-galaxy-reactor", "Bowser's Galaxy Reactor", 0, 1, "", ""},
    {"CubeBubbleExLv2Galaxy", "bubble-blast", "Bubble Blast Galaxy", 0, 1, "", ""},
    {"HeavensDoorGalaxy", "gateway", "Gateway Galaxy", 0, 2, "", ""},
    {"PeachCastleFinalGalaxy", "grand-finale", "Grand Finale Galaxy", 0, 1, "", ""},
    {"SurfingLv2Galaxy", "loopdeeswoop", "Loopdeeswoop Galaxy", 0, 1, "", ""},
    {"TamakoroExLv2Galaxy", "rolling-gizmo", "Rolling Gizmo Galaxy", 0, 1, "", ""},
};

std::string key(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (out.size() > 6 && out.compare(out.size() - 6, 6, "galaxy") == 0) out.resize(out.size() - 6);
    return out;
}

}  // namespace

const Galaxy* galaxies(int* count) {
    *count = static_cast<int>(std::size(kGalaxies));
    return kGalaxies;
}

const char* domeName(int dome) {
    static const char* const kNames[] = {"Elsewhere", "Terrace", "Fountain", "Kitchen", "Bedroom", "Engine Room", "Garden"};
    return dome >= 0 && dome < static_cast<int>(std::size(kNames)) ? kNames[dome] : "Elsewhere";
}

const Galaxy* find(const std::string& name) {
    const std::string wanted = key(name);
    if (wanted.empty()) return nullptr;
    for (const Galaxy& galaxy : kGalaxies) {
        if (key(galaxy.alias) == wanted || key(galaxy.stage) == wanted || key(galaxy.name) == wanted) return &galaxy;
    }
    return nullptr;
}

bool resolve(const std::string& name, int scenario, std::string* stage, std::string* error) {
    const Galaxy* galaxy = find(name);
    if (galaxy == nullptr) {
        *error = "unknown galaxy \"" + name + "\" (petari --stage list shows the names)";
        return false;
    }
    if (scenario < 1 || scenario > galaxy->missions) {
        *error = std::string(galaxy->name) + " has missions 1-" + std::to_string(galaxy->missions) +
                 "; --scenario " + std::to_string(scenario) + " does not exist";
        return false;
    }
    *stage = galaxy->stage;
    return true;
}

void printList(std::FILE* out) {
    std::fputs("Galaxies for --stage (alias, internal name, missions; C = comet, H = hidden star):\n", out);
    for (int dome = 1; dome <= 7; ++dome) {
        const int section = dome == 7 ? 0 : dome;
        std::fprintf(out, "\n%s\n", domeName(section));
        for (const Galaxy& g : kGalaxies) {
            if (g.dome != section) continue;
            std::string missions;
            for (int m = 1; m <= g.missions; ++m) {
                const char digit = static_cast<char>('0' + m);
                missions += std::to_string(m);
                for (const char* c = g.comets; *c; ++c) if (*c == digit) missions += 'C';
                for (const char* h = g.hidden; *h; ++h) if (*h == digit) missions += 'H';
                if (m < g.missions) missions += ' ';
            }
            std::fprintf(out, "  %-28s %-24s %s  [%s]\n", g.alias, g.stage, g.name, missions.c_str());
        }
    }
}

}  // namespace PetariNative::App::LaunchStage

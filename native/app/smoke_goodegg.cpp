// The Good Egg mission 1 script (smoke_goodegg.hpp). No SDK or Aurora headers:
// the frame seam feeds it observations and applies its presses.

#include "smoke_goodegg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kTapFrames = 6;
constexpr unsigned long kSpinInterval = 30;     // a spin lasts about this long; more taps are ignored
constexpr unsigned long kVineSpinInterval = 20;
constexpr unsigned long kTalkTapInterval = 45;
constexpr unsigned long kTalkStartInterval = 60;
constexpr unsigned long kMissionLimit = 54000;  // 15 minutes of play
constexpr unsigned long kPlanetLimit = 14400;   // 4 minutes on one planet
constexpr unsigned long kStuckFrames = 150;
constexpr float kProgressStep = 40.0f;
constexpr int kMaxRecoveries = 8;
constexpr unsigned long kSidestepFrames = 40;
constexpr unsigned long kAirSteerFrames = 60;   // steer in the air this long (own jumps), not in long flights
// SuperSpinDriver sensors: a swing within 300 of a waiting star shoots; within
// 240 it pulls Mario in (Capture) and waits for a swing. Distances here are
// from Mario's feet; his sensor is higher, so a held Mario can read farther.
constexpr float kLaunchReach = 300.0f;
constexpr float kLaunchHeldReach = 450.0f;
constexpr float kEnemyReach = 220.0f;
constexpr unsigned long kStarGetLimit = 3600;
constexpr unsigned long kReturnLimit = 7200;
constexpr unsigned long kSaveIdleFrames = 60;
constexpr unsigned long kDomeReadyFrames = 60;
constexpr unsigned long kPointingFramesToPress = 3;
constexpr unsigned long kAimLimit = 240;
constexpr unsigned long kLogInterval = 120;

struct Vec {
    float x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
Vec normalized(Vec a) {
    const float n = length(a);
    return n > 1e-6f ? a * (1.0f / n) : Vec{0.0f, 0.0f, 0.0f};
}
Vec across(Vec d, Vec u) { return d - u * dot(d, u); }
Vec vec(const Point3& p) { return {p.x, p.y, p.z}; }
Vec position(const Observation& o) { return {o.playerX, o.playerY, o.playerZ}; }
Vec actorPos(const Observation::Actor& a) { return {a.x, a.y, a.z}; }
Vec up(const Observation& o) {
    const Vec g{-o.gravityX, -o.gravityY, -o.gravityZ};
    const float l = length(g);
    return l > 1e-3f ? g * (1.0f / l) : Vec{0.0f, 1.0f, 0.0f};
}
std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", value);
    return text;
}
std::string text(Vec a) { return "(" + number(a.x) + ", " + number(a.y) + ", " + number(a.z) + ")"; }

// Planet surfaces from the stage placement (collision bounding boxes in world
// coordinates, EggStarGalaxy scenario 1 zones): the centre and the largest
// distance of the surface from it, plus a margin for standing and jumping.
struct PlanetInfo {
    Planet planet;
    Vec centre;
    float reach;
};
const PlanetInfo kPlanets[] = {
    {Planet::DiskGarden, {-4517.0f, -13999.0f, -17310.0f}, 3900.0f},
    {Planet::Peanut, {-10559.0f, -15437.0f, -2414.0f}, 1900.0f},
    {Planet::BeanB, {-18188.0f, -16191.0f, -8047.0f}, 1400.0f},
    {Planet::FruitPeel, {-17932.0f, -11998.0f, -9406.0f}, 2600.0f},
    {Planet::BeanC, {-18440.0f, -6307.0f, -10722.0f}, 2400.0f},
    {Planet::Dino, {-8398.0f, -7525.0f, -37797.0f}, 1600.0f},
};

// DiskGardenZone rail 2 (29 points, from the top's centre over the west rim,
// under the disk, down the stem to the bottom), a surface walk laid out by the
// stage. The Luma that becomes the Sling Star (Tico l_id 40) is at the bottom.
const std::vector<Point3> kDiskGardenRoute = {
    {-3768, -12974, -16022}, {-4020, -12959, -16315}, {-4349, -12958, -16458}, {-4717, -12957, -16301},
    {-5005, -12970, -15977}, {-5305, -13024, -15665}, {-5520, -13060, -15450}, {-5699, -13137, -15234},
    {-5945, -13144, -14993}, {-6093, -13194, -14850}, {-6177, -13324, -14758}, {-6218, -13521, -14720},
    {-6226, -13700, -14706}, {-6208, -13859, -14733}, {-6140, -14020, -14801}, {-6005, -14149, -14923},
    {-5791, -14204, -15146}, {-5474, -14216, -15462}, {-5272, -14325, -15656}, {-5215, -14595, -15737},
    {-5210, -14992, -15740}, {-5211, -15271, -15746}, {-5204, -15553, -15744}, {-5204, -15852, -15751},
    {-5202, -16098, -15753}, {-5200, -16307, -15755}, {-5170, -16484, -15784}, {-5062, -16538, -15894},
    {-4820, -16528, -16141}};
const Vec kDiskLuma{-4671.0f, -16693.0f, -16381.0f};
// FruitPeelPlanet (a spiral peel around a black hole) from where the Bean B
// vine throws Mario (under the peel) to the Hammer Head's platform: a shortest
// path over the collision's walkable triangles (normal within 53 degrees of
// the point gravity's up), penalised within 180 units of a peel edge and never
// within 150 of NeedlePlant, PunchingKinoko, Karikari or CollapsePlane
// placements; sampled every 250 units. Edge clearance along it is at least 157.
const std::vector<Point3> kFruitPeelRoute = {
    {-18323, -13640, -9499}, {-18087, -13695, -9387}, {-17818, -13689, -9315}, {-17477, -13695, -9274},
    {-17242, -13592, -9056}, {-16950, -13355, -8903}, {-16767, -13153, -8836}, {-16735, -12969, -8630},
    {-16825, -12783, -8382}, {-17020, -12665, -8198}, {-17273, -12512, -8045}, {-17571, -12324, -7935},
    {-17835, -12329, -7924}, {-18151, -12317, -7958}, {-18450, -12293, -8041}, {-18717, -12268, -8175},
    {-18954, -12194, -8361}, {-19148, -12008, -8597}, {-19273, -12026, -8881}, {-19359, -11904, -9246},
    {-19371, -11906, -9514}, {-19334, -11899, -9834}, {-19243, -11886, -10139}, {-19098, -11873, -10413},
    {-18886, -11618, -10636}, {-18704, -11449, -10738}, {-18432, -11201, -10780}, {-18343, -10967, -10636},
    {-18295, -10801, -10389}, {-18336, -10733, -10060}, {-18176, -10775, -9817}, {-17919, -10812, -9578},
    {-17710, -10818, -9431}};
// BroadBeanPlanetC from under it (where the Fruit Peel vine throws Mario)
// around to the crystal on top, the same way (Takobo and Petari placements
// avoided); a convex bean, so this only keeps the walk on its short side.
// Long chords are split so no two points are more than 300 apart.
const std::vector<Point3> kBeanCRoute = {
    {-18471, -7239, -10405}, {-18444, -7255, -10665}, {-18452, -7260, -10943}, {-18423, -7240, -11205},
    {-18475, -7211, -11476}, {-18527, -7182, -11746}, {-18635, -7158, -11858}, {-18743, -7134, -11971},
    {-18828, -7012, -12100}, {-18912, -6891, -12229}, {-18947, -6738, -12216}, {-18982, -6585, -12203},
    {-18934, -6389, -12061}, {-18926, -6104, -12038}, {-18917, -5820, -12016}, {-18909, -5535, -11993},
    {-18680, -5427, -12054}, {-18452, -5319, -12116}};
// PeanutZone layer A YellowChip l_id 16-20.
const Vec kPeanutChips[] = {{-9208.8f, -15819.9f, -2453.4f},
                            {-10517.5f, -15942.3f, -2441.7f},
                            {-10547.9f, -14915.6f, -2485.1f},
                            {-11150.4f, -15615.0f, -3413.3f},
                            {-10156.9f, -15547.8f, -1449.6f}};
constexpr int kPeanutChipCount = 5;
// PeanutMudPlanet tour from the Disk Garden launch star's landing past the
// five chips (in the order right lobe, top, waist bottom, waist top, left
// lobe) to below the launch star they form: shortest paths over the
// collision's sand (floor code 13; never on SinkDeathMud, code 32, nor within
// 130 of it), penalised within 350 of the three boulder rails (PeanutZone
// rails 1-3, the RockCreators' paths once SwitchCube SW 1006 turns on),
// sampled every 200. The rails cross the planet: boulders are dodged live.
const std::vector<Point3> kPeanutRoute = {
    {-9122, -15504, -2193}, {-9216, -15716, -2408}, {-9261, -15795, -2429}, {-9407, -15982, -2409},
    {-9536, -16091, -2198}, {-9798, -16159, -2140}, {-10019, -16153, -2162}, {-10289, -16011, -1984},
    {-10332, -15851, -1780}, {-10372, -15653, -1686}, {-10248, -15525, -1580}, {-10348, -15776, -1731},
    {-10309, -15944, -1872}, {-10260, -16059, -2107}, {-10430, -15950, -2327}, {-10527, -15879, -2441},
    {-10696, -15952, -2564}, {-10760, -15976, -2796}, {-10720, -15849, -2965}, {-10739, -15630, -3142},
    {-10706, -15365, -3149}, {-10717, -15090, -3056}, {-10710, -14920, -2793}, {-10569, -14970, -2518},
    {-10710, -14920, -2793}, {-10732, -15021, -3010}, {-10729, -15236, -3146}, {-10730, -15456, -3173},
    {-10821, -15641, -3210}, {-11001, -15660, -3300}, {-11156, -15607, -3350}, {-11430, -15747, -3292},
    {-11648, -15679, -3226}, {-11798, -15568, -3128}, {-11923, -15385, -2954}};
// PeanutZone rails l_id 1-3: closed cubic Bezier loops (point, then the
// handles pnt2 out and pnt1 in), evaluated every ~120 units. The boulders
// (RockCreator, default speed 10 units/frame, one boulder each; two creators
// per rail) roll around them for good once SwitchCube SW 1006 turns on;
// observed boulders in goodegg-6 were within 182 units of these curves.
const std::vector<Point3> kPeanutRails[3] = {
    // rail l_id 1 (49 points)
    {{-10193, -15290, -1202}, {-10190, -15151, -1220}, {-10185, -15029, -1252}, {-10176, -14894, -1302},
     {-10162, -14754, -1373}, {-10146, -14651, -1441}, {-10126, -14553, -1523}, {-10101, -14463, -1620},
     {-10071, -14385, -1732}, {-10034, -14323, -1862}, {-9991, -14280, -2009}, {-9940, -14259, -2175},
     {-9903, -14259, -2295}, {-9851, -14284, -2457}, {-9806, -14336, -2598}, {-9767, -14410, -2718},
     {-9734, -14500, -2819}, {-9706, -14603, -2903}, {-9682, -14714, -2971}, {-9664, -14827, -3024},
     {-9645, -14975, -3077}, {-9632, -15109, -3110}, {-9623, -15239, -3132}, {-9620, -15361, -3134},
     {-9623, -15495, -3115}, {-9629, -15642, -3082}, {-9637, -15765, -3046}, {-9649, -15895, -2997},
     {-9665, -16025, -2934}, {-9685, -16150, -2856}, {-9712, -16264, -2760}, {-9745, -16363, -2645},
     {-9785, -16441, -2510}, {-9833, -16491, -2353}, {-9869, -16507, -2235}, {-9908, -16508, -2109},
     {-9945, -16494, -1991}, {-9995, -16450, -1834}, {-10037, -16381, -1698}, {-10073, -16292, -1583},
     {-10104, -16187, -1486}, {-10128, -16071, -1407}, {-10148, -15948, -1343}, {-10164, -15822, -1294},
     {-10176, -15699, -1257}, {-10184, -15581, -1231}, {-10190, -15441, -1210}, {-10193, -15309, -1202},
     {-10193, -15290, -1202}},
    // rail l_id 2 (35 points)
    {{-10542, -14655, -2429}, {-10495, -14666, -2577}, {-10457, -14697, -2700}, {-10417, -14764, -2829},
     {-10388, -14852, -2923}, {-10368, -14952, -2987}, {-10353, -15083, -3034}, {-10347, -15204, -3053},
     {-10347, -15325, -3055}, {-10348, -15452, -3050}, {-10355, -15592, -3027}, {-10370, -15730, -2980},
     {-10389, -15841, -2917}, {-10418, -15943, -2826}, {-10456, -16027, -2702}, {-10493, -16072, -2584},
     {-10538, -16099, -2443}, {-10589, -16103, -2279}, {-10635, -16082, -2132}, {-10672, -16040, -2015},
     {-10708, -15957, -1897}, {-10733, -15852, -1817}, {-10748, -15736, -1769}, {-10755, -15617, -1746},
     {-10757, -15479, -1741}, {-10753, -15356, -1752}, {-10750, -15223, -1763}, {-10743, -15101, -1784},
     {-10730, -14981, -1827}, {-10706, -14861, -1903}, {-10678, -14776, -1995}, {-10639, -14708, -2119},
     {-10602, -14673, -2237}, {-10558, -14656, -2377}, {-10542, -14655, -2429}},
    // rail l_id 3 (47 points)
    {{-11571, -15304, -1654}, {-11564, -15439, -1664}, {-11553, -15569, -1685}, {-11535, -15718, -1721},
     {-11517, -15837, -1761}, {-11495, -15956, -1812}, {-11469, -16072, -1875}, {-11439, -16181, -1954},
     {-11405, -16277, -2047}, {-11366, -16357, -2157}, {-11322, -16417, -2286}, {-11274, -16451, -2434},
     {-11222, -16457, -2600}, {-11174, -16431, -2757}, {-11134, -16379, -2898}, {-11099, -16305, -3022},
     {-11071, -16214, -3131}, {-11048, -16109, -3226}, {-11029, -15997, -3307}, {-11015, -15880, -3375},
     {-11005, -15765, -3430}, {-10995, -15620, -3486}, {-10991, -15495, -3523}, {-10988, -15370, -3545},
     {-10997, -15226, -3529}, {-11010, -15107, -3501}, {-11029, -14969, -3457}, {-11055, -14822, -3395},
     {-11078, -14713, -3335}, {-11106, -14608, -3263}, {-11137, -14513, -3176}, {-11172, -14432, -3075},
     {-11212, -14368, -2958}, {-11256, -14327, -2824}, {-11304, -14312, -2672}, {-11351, -14325, -2518},
     {-11393, -14361, -2378}, {-11429, -14417, -2251}, {-11460, -14489, -2137}, {-11487, -14574, -2036},
     {-11509, -14667, -1948}, {-11527, -14767, -1872}, {-11541, -14869, -1808}, {-11555, -15002, -1740},
     {-11564, -15125, -1692}, {-11570, -15250, -1660}, {-11571, -15304, -1654}}};
constexpr float kRockOffRail = 350.0f;  // farther: not rolling on its rail
constexpr float kRockWatch = 1500.0f;   // boulders this close are predicted
constexpr int kRockSteps = 9;           // prediction samples...
constexpr int kRockStepFrames = 5;      // ...5 frames apart: 45 frames ahead
constexpr float kMarioSpeed = 8.0f;     // running, units per frame
// Mario's position is his feet, a boulder's its centre, about a radius above
// its rail: contact is near 340 feet-to-centre. The margin covers the rail
// fit (observed boulders median 60, at most 235 from the fitted curves) and
// the 8-way stick.
constexpr float kRockSafe = 460.0f;

struct RailSpot {
    int rail = -1;
    size_t index = 0;
    float distance = 1e30f;
};
// The nearest rail point (on one rail, or on any when rail < 0).
RailSpot nearestRail(Vec p, int rail) {
    RailSpot best;
    for (int r = 0; r < 3; ++r) {
        if (rail >= 0 && r != rail) {
            continue;
        }
        for (size_t i = 0; i < kPeanutRails[r].size(); ++i) {
            const float d = length(vec(kPeanutRails[r][i]) - p);
            if (d < best.distance) {
                best = {r, i, d};
            }
        }
    }
    return best;
}
Vec railTangent(const RailSpot& spot) {
    const auto& rail = kPeanutRails[spot.rail];
    const size_t next = (spot.index + 1) % rail.size();
    return normalized(vec(rail[next]) - vec(rail[spot.index]));
}
// +1 when moving toward higher indices.
int railDirection(const RailSpot& spot, Vec velocity) {
    return dot(velocity, railTangent(spot)) >= 0.0f ? 1 : -1;
}
// The rail point `distance` along the rail from `from`, rolling in `direction`.
Vec railAhead(const RailSpot& from, int direction, float distance) {
    const auto& rail = kPeanutRails[from.rail];
    const size_t n = rail.size();
    size_t i = from.index;
    for (size_t steps = 0; steps < n; ++steps) {
        const size_t next = (i + n + direction) % n;
        const float segment = length(vec(rail[next]) - vec(rail[i]));
        if (distance <= segment) {
            return vec(rail[i]) + (vec(rail[next]) - vec(rail[i])) * (distance / std::max(segment, 1.0f));
        }
        distance -= segment;
        i = next;
    }
    return vec(rail[i]);
}
const Vec kPeanutLaunchStar{-12216.7f, -15424.5f, -3022.8f};  // l_id 4, SW_APPEAR 1007 (the chips)
const Vec kBeanBPiranha{-18416.7f, -15888.5f, -8672.8f};      // PackunPetit l_id 62, SW_DEAD 4
const Vec kBeanBVine{-18425.0f, -15878.7f, -8666.7f};         // Plant l_id 21, SW_APPEAR 4
const Vec kHammerHead{-17716.7f, -10726.9f, -9460.3f};       // HammerHeadPackun l_id 3, SW_DEAD 3
const Vec kFruitVine{-17717.1f, -10743.5f, -9460.3f};        // Plant l_id 30, SW_APPEAR 3
const Vec kBeanCCage{-18586.7f, -5200.0f, -12112.8f};        // CrystalCageS l_id 69, SW_DEAD 1020
const Vec kBeanCLaunchStar{-18608.2f, -5031.8f, -12150.0f};  // l_id 9, SW_APPEAR 1020
const Vec kDinoCentre{-8400.0f, -7524.1f, -37800.0f};

const Observation::Actor* nearestActor(const Observation& o, const char* kind, Vec from, float within,
                                       unsigned requiredFlags = 0) {
    const Observation::Actor* best = nullptr;
    float bestDistance = within;
    for (const Observation::Actor& actor : o.actors) {
        if (actor.kind != kind || (actor.flags & requiredFlags) != requiredFlags) {
            continue;
        }
        const float d = length(actorPos(actor) - from);
        if (d < bestDistance) {
            bestDistance = d;
            best = &actor;
        }
    }
    return best;
}

bool targetShown(const Observation& o, const char* id) {
    return std::any_of(o.targets.begin(), o.targets.end(), [id](const Observation::Target& t) {
        return t.id == id && (t.flags & kTargetSelectable);
    });
}

}  // namespace

const char* planetName(Planet planet) {
    switch (planet) {
    case Planet::None: return "none";
    case Planet::DiskGarden: return "Disk Garden";
    case Planet::Peanut: return "Peanut";
    case Planet::BeanB: return "Bean B";
    case Planet::FruitPeel: return "Fruit Peel";
    case Planet::BeanC: return "Bean C";
    case Planet::Dino: return "Dino Piranha's planet";
    case Planet::Other: return "other";
    }
    return "?";
}

Planet planetAt(const Point3& point) {
    Planet best = Planet::None;
    float bestRatio = 1.0f;
    for (const PlanetInfo& info : kPlanets) {
        const float ratio = length(vec(point) - info.centre) / info.reach;
        if (ratio < bestRatio) {
            bestRatio = ratio;
            best = info.planet;
        }
    }
    return best;
}

const std::vector<Point3>& diskGardenRoute() {
    return kDiskGardenRoute;
}

const std::vector<Point3>& fruitPeelRoute() {
    return kFruitPeelRoute;
}

const std::vector<Point3>& beanCRoute() {
    return kBeanCRoute;
}

bool GoodEggDriver::followRoute(const Observation& o, const std::vector<Point3>& route, const char* name, Step& step) {
    const Vec pos = position(o);
    const Point3& current = route[std::min(mWaypoint, route.size() - 1)];
    if (!mWaypointChosen || length(vec(current) - pos) > 900.0f) {
        mWaypointChosen = true;
        float best = 1e30f;
        for (size_t i = 0; i < route.size(); ++i) {
            const float d = length(vec(route[i]) - pos);
            if (d < best) {
                best = d;
                mWaypoint = i;
            }
        }
        note(std::string(name) + ": from point " + std::to_string(mWaypoint) + " of " + std::to_string(route.size()) +
             ", " + number(best) + " away");
    }
    // Skip ahead when the next point is already nearer.
    while (mWaypoint + 1 < route.size() &&
           length(vec(route[mWaypoint + 1]) - pos) < length(vec(route[mWaypoint]) - pos)) {
        ++mWaypoint;
    }
    if (mWaypoint >= route.size()) {
        return true;
    }
    if (goTo(o, route[mWaypoint], 150.0f, name, step)) {
        ++mWaypoint;
        resetStuck();
        if (mWaypoint % 4 == 0 || mWaypoint == route.size()) {
            note(std::string(name) + " point " + std::to_string(mWaypoint) + " at " + text(pos) + ", up " + text(up(o)));
        }
    }
    return mWaypoint >= route.size();
}

StickKeys stickKeysForWorld(const Observation& o, const Point3& direction) {
    const Vec u = up(o);
    const Vec d = across(vec(direction), u);
    const Vec camX{o.camXx, o.camXy, o.camXz};
    const Vec camZ{o.camZx, o.camZy, o.camZz};  // MR::getCamZdir: the view direction
    const Vec camY = cross(camZ * -1.0f, camX);  // the camera's up (X right, Y up, -Z view)
    const Vec right = normalized(across(camX, u));
    // Screen-up on the ground: the view direction when the camera looks along
    // the ground, its up when it looks down on Mario; their sum covers both.
    const Vec forward = normalized(across(camZ, u) + across(camY, u));
    if (length(d) < 1e-3f || length(right) < 0.5f || length(forward) < 0.5f) {
        return {};
    }
    return stickKeysFor(dot(d, right), dot(d, forward));
}

bool goodEggEnabledFromEnvironment(GoodEggConfig* config) {
    const char* value = std::getenv("PETARI_SMOKE");
    if (value == nullptr || std::strcmp(value, "goodegg1") != 0) {
        return false;
    }
    const char* stage = std::getenv("PETARI_STAGE");
    config->synthetic = stage != nullptr && stage[0] != '\0';
    if (config->synthetic) {
        const char* scenario = std::getenv("PETARI_SCENARIO");
        if (std::strcmp(stage, "EggStarGalaxy") != 0 || scenario == nullptr || std::strcmp(scenario, "1") != 0) {
            std::fprintf(stderr, "PETARI SMOKE: goodegg1 with the stage fixture needs PETARI_STAGE=EggStarGalaxy "
                                 "PETARI_SCENARIO=1; not running\n");
            return false;
        }
    }
    return true;
}

GoodEggDriver::GoodEggDriver(unsigned long frameLimit, const GoodEggConfig& config)
    : mBoot(frameLimit + 1, config.synthetic ? Script::Reload : Script::Galaxy), mConfig(config), mFrameLimit(frameLimit) {}

const char* GoodEggDriver::phase() const {
    switch (mPhase) {
    case Phase::Boot:
        return mBoot.phase();
    case Phase::Mission:
        switch (mPlanet) {
        case Planet::DiskGarden: return "Good Egg: Disk Garden";
        case Planet::Peanut: return "Good Egg: Peanut";
        case Planet::BeanB: return "Good Egg: Bean B";
        case Planet::FruitPeel: return "Good Egg: Fruit Peel";
        case Planet::BeanC: return "Good Egg: Bean C";
        case Planet::Dino: return "Good Egg: Dino Piranha";
        default: return "Good Egg: between planets";
        }
    case Phase::StarGet:
        return "Good Egg: star get";
    case Phase::Return:
        return "returning to the observatory";
    case Phase::Done:
        return "done";
    }
    return "?";
}

void GoodEggDriver::steer(const StickKeys& keys, Step& step) {
    const struct {
        bool want, held;
        Button button;
    } keysNow[] = {{keys.up, mHeld.up, Button::StickUp},
                   {keys.down, mHeld.down, Button::StickDown},
                   {keys.left, mHeld.left, Button::StickLeft},
                   {keys.right, mHeld.right, Button::StickRight}};
    for (const auto& key : keysNow) {
        if (key.want != key.held) {
            step.presses.push_back({key.button, key.want});
            step.assertFocus = step.assertFocus || key.want;
        }
    }
    mHeld = keys;
}

void GoodEggDriver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.presses.push_back({button, true});
    step.assertFocus = true;
    mReleases.push_back({mFrame + holdFrames, button});
}

bool GoodEggDriver::spin(Step& step, const char* why) {
    if (mLastSpin != 0 && mFrame - mLastSpin < kSpinInterval) {
        return false;
    }
    mLastSpin = mFrame;
    note(std::string("spin: ") + why);
    tap(Button::Spin, kTapFrames, step);
    return true;
}

void GoodEggDriver::resetStuck() {
    mBestDistance = 1e30f;
    mStuckFrames = 0;
    mRecoveries = 0;
    mSidestepUntil = 0;
}

void GoodEggDriver::finish(Result result, const std::string& reason, Step& step) {
    for (const Release& release : mReleases) {
        step.presses.push_back({release.button, false});
    }
    mReleases.clear();
    steer({}, step);
    mResult = result;
    mReason = reason;
    if (mConfig.synthetic) {
        mReason += " [synthetic stage-fixture entry, not the observatory route]";
    }
    if (mAssistInputs > 0) {
        const std::string assisted = std::to_string(mAssistInputs) + " physical gameplay input" +
                                     (mAssistInputs == 1 ? "" : "s") + ", first " + mFirstAssist;
        if (result == Result::Pass) {
            mResult = Result::Assisted;
            mReason += "; ASSISTED, not unattended: " + assisted;
        } else {
            mReason += " (also assisted: " + assisted + ")";
        }
    } else if (result == Result::Pass && (mPointerInputs > 0 || mFocusInputs > 0)) {
        mReason += " (no physical gameplay input; the pointer moved " + std::to_string(mPointerInputs) +
                   " times and focus changed " + std::to_string(mFocusInputs) + " times)";
    }
    mPhase = Phase::Done;
    step.requestQuit = true;
    note(std::string(resultName(mResult)) + ": " + mReason);
}

void GoodEggDriver::notePhysical(const Observation& o) {
    const PhysicalInputs& now = o.physical;
    if (!mPhysicalBaseSet) {
        mPhysicalSeen = now;
        mPhysicalBaseSet = true;
        return;
    }
    if (now.gameplay > mPhysicalSeen.gameplay) {
        const unsigned long added = now.gameplay - mPhysicalSeen.gameplay;
        const std::string last(now.last, strnlen(now.last, sizeof(now.last)));
        if (mAssistInputs == 0) {
            mFirstAssist = "seen at frame " + std::to_string(mFrame) + " while " + phase() + " (" +
                           std::to_string(added) + " new, latest " + last + ")";
        }
        if (mAssistInputs < 20) {
            note("physical input: " + std::to_string(added) + " new, latest " + last + ", while " + phase());
        }
        mAssistInputs += added;
    }
    if (now.pointer > mPhysicalSeen.pointer) {
        if (mPointerInputs == 0) {
            note(std::string("physical pointer motion while ") + phase() + " (not counted as assistance)");
        }
        mPointerInputs += now.pointer - mPhysicalSeen.pointer;
    }
    if (now.focus > mPhysicalSeen.focus) {
        note(std::string("window focus changed while ") + phase());
        mFocusInputs += now.focus - mPhysicalSeen.focus;
    }
    mPhysicalSeen = now;
}

Step GoodEggDriver::step(const Observation& o) {
    Step step;
    mLog.clear();
    ++mFrame;
    ++mPhaseFrames;
    for (auto it = mReleases.begin(); it != mReleases.end();) {
        if (it->frame <= mFrame) {
            step.presses.push_back({it->button, false});
            it = mReleases.erase(it);
        } else {
            ++it;
        }
    }
    if (mResult != Result::Running) {
        return step;
    }
    notePhysical(o);

    if (mPhase == Phase::Boot) {
        step = mBoot.step(o);
        for (const std::string& line : mBoot.log()) {
            // The boot's physical-input lines are the same inputs this driver counts.
            if (line.rfind("physical ", 0) != 0 && line.rfind("window focus", 0) != 0) {
                mLog.push_back(line);
            }
        }
        if (mBoot.result() != Result::Running) {
            // The route itself failed (or was blocked) before the mission.
            mResult = mBoot.result() == Result::Blocked ? Result::Blocked : Result::Fail;
            mReason = std::string("before the mission: ") + mBoot.reason();
            mPhase = Phase::Done;
            step.requestQuit = true;
            return step;
        }
        const bool handover = mConfig.synthetic
                                  ? (o.scene == "Game" && o.stage == "EggStarGalaxy")
                                  : mBoot.galaxyMissionReady();
        if (!handover) {
            return step;
        }
        // Nothing the boot held stays held; its pending releases are dropped.
        step.presses.clear();
        for (Button button : {Button::A, Button::B, Button::StickUp, Button::StickDown, Button::StickLeft,
                              Button::StickRight, Button::Spin}) {
            step.presses.push_back({button, false});
        }
        step.requestQuit = false;
        mHeld = {};
        mPhase = Phase::Mission;
        mPhaseFrames = 0;
        mLastScene = o.scene;
        mLastStage = o.stage;
        note(std::string("Good Egg mission 1: taking over from the ") +
             (mConfig.synthetic ? "synthetic stage entry" : "galaxy route") + " at frame " + std::to_string(mFrame));
        return step;
    }

    if (o.scene != mLastScene || o.stage != mLastStage) {
        note("scene " + (o.scene.empty() ? std::string("(none)") : o.scene) + (o.stage.empty() ? "" : " stage " + o.stage) +
             " scenario " + std::to_string(o.scenario));
        mLastScene = o.scene;
        mLastStage = o.stage;
    }
    for (const std::string& milestone : o.milestones) {
        note("milestone " + milestone);
        mSeen.push_back(milestone);
    }
    if (mFrame >= mFrameLimit) {
        finish(Result::Fail, "frame limit " + std::to_string(mFrameLimit) + " reached while " + phase(), step);
        return step;
    }
    // Prompts: only the save after the star is expected.
    for (const Observation::Prompt& prompt : o.prompts) {
        note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
        if (mPhase == Phase::Return && prompt.messageId == "System_Save00" && prompt.type == 2) {
            mSaveShown = true;
            mPrompt = "Prompt.Yes";
            mAimFrames = mPointingFrames = 0;
        } else if (mPhase == Phase::Return && prompt.messageId == "System_Save02" && prompt.type == 0) {
            mSaveDone = true;
            mLastA = mFrame;  // read it, then A
        } else if (prompt.messageId == "System_Save01" && prompt.type == 1 && mPhase == Phase::Return) {
            note("saving window System_Save01: no input");
        } else {
            finish(Result::Blocked, "prompt " + prompt.messageId + " (type " + std::to_string(prompt.type) +
                                        ") is not expected while " + phase(), step);
            return step;
        }
    }

    switch (mPhase) {
    case Phase::Mission:
        mission(o, step);
        break;
    case Phase::StarGet:
        starGet(o, step);
        break;
    case Phase::Return:
        returnToDome(o, step);
        break;
    default:
        break;
    }
    return step;
}

bool GoodEggDriver::goTo(const Observation& o, const Point3& target, float radius, const char* what, Step& step,
                         bool flat) {
    const Vec pos = position(o);
    const Vec delta = vec(target) - pos;
    const float distance = flat ? length(across(delta, up(o))) : length(delta);
    mGoalPoint = target;
    mHasGoal = true;
    if (mGoal != what) {
        mGoal = what;
        resetStuck();
        note(std::string("going to ") + what + " " + text(vec(target)) + " from " + text(pos) + ", " + number(distance) +
             " away");
    }
    if (distance < radius) {
        steer({}, step);
        return true;
    }
    if (mFrame >= mLogAt) {
        mLogAt = mFrame + kLogInterval;
        note(std::string(what) + ": " + number(distance) + " away at " + text(pos) + (o.playerOnGround ? "" : " (air)") +
             ", up " + text(up(o)) + ", life " + std::to_string(o.playerLife));
    }
    if (distance < mBestDistance - kProgressStep) {
        mBestDistance = distance;
        mStuckFrames = 0;
    } else if (++mStuckFrames >= kStuckFrames && mFrame >= mSidestepUntil) {
        mStuckFrames = 0;
        mBestDistance = distance;
        if (++mRecoveries > kMaxRecoveries) {
            finish(Result::Fail, std::string("stuck going to ") + what + " at " + text(pos) + ", " + number(distance) +
                                     " away, on " + planetName(mPlanet),
                   step);
            return false;
        }
        if (mRecoveries % 2 == 1) {
            note(std::string("stuck going to ") + what + ": jump at " + text(pos));
            tap(Button::A, kTapFrames, step);
        } else {
            note(std::string("stuck going to ") + what + ": sidestep at " + text(pos));
            mSidestepUntil = mFrame + kSidestepFrames;
        }
    }
    Vec direction = delta;
    if (mFrame < mSidestepUntil) {
        const float side = (mRecoveries / 2) % 2 == 0 ? 1.0f : -1.0f;
        direction = cross(up(o), delta) * side;
    }
    steer(stickKeysForWorld(o, {direction.x, direction.y, direction.z}), step);
    return false;
}

void GoodEggDriver::mission(const Observation& o, Step& step) {
    if (seen("PowerStar.Get")) {
        steer({}, step);
        mPhase = Phase::StarGet;
        mPhaseFrames = 0;
        note("Power Star touched on " + std::string(planetName(mPlanet)) + " after " + std::to_string(mMissionFrames) +
             " mission frames");
        return;
    }
    if (o.scene != "Game") {
        steer({}, step);
        return;
    }
    if (!o.stage.empty() && o.stage != "EggStarGalaxy") {
        finish(Result::Fail, "left Good Egg for " + o.stage + " without the Power Star", step);
        return;
    }
    if (++mMissionFrames > kMissionLimit) {
        finish(Result::Fail, "mission not finished in " + std::to_string(kMissionLimit) + " frames (on " +
                                 planetName(mPlanet) + ")",
               step);
        return;
    }
    if (!o.playerValid) {
        steer({}, step);
        return;
    }
    const Vec pos = position(o);
    // After a bind ends (a launch star's release, a vine's throw): every frame
    // for 150 frames, with the step from the previous frame, so a jump in
    // position (a teleport, a push-out) shows where it happened.
    if (mReleaseTrace > 0 && !o.playerInBind) {
        --mReleaseTrace;
        const Vec moved = pos - vec(mLastPos);
        note("after release: " + text(pos) + " (moved " + number(length(moved)) + "), up " + text(up(o)) +
             (o.playerOnGround ? ", ground" : ", air") + ", demo " + std::to_string(o.demoActive) + ", life " +
             std::to_string(o.playerLife));
    }
    if (o.playerInBind) {
        mReleaseTrace = 150;
    }
    mLastPos = {pos.x, pos.y, pos.z};
    if (o.playerLife != mLastLife) {
        if (mLastLife >= 0) {
            note("life " + std::to_string(mLastLife) + " -> " + std::to_string(o.playerLife) + " at " + text(pos) + " on " +
                 planetName(mPlanet));
        }
        mLastLife = o.playerLife;
    }
    if (o.playerDead) {
        finish(Result::Fail, "Mario died at " + text(pos) + " on " + planetName(mPlanet), step);
        return;
    }
    if (mStarsAtStart < 0 && o.powerStars >= 0) {
        mStarsAtStart = o.powerStars;
        note("stars at mission start: " + std::to_string(mStarsAtStart) + ", Good Egg 1 recorded " +
             (o.starEggStar1 ? "yes" : "no"));
        if (o.starEggStar1) {
            finish(Result::Fail, "the file already has Good Egg mission 1's star; use a fixture without it", step);
            return;
        }
    }
    // Talk pages: A, as a player reads them.
    if (targetShown(o, "Talk.Advance")) {
        steer({}, step);
        ++mTalkFrames;
        if (mFrame - mLastA >= kTalkTapInterval) {
            mLastA = mFrame;
            note("tap A: talk page");
            tap(Button::A, kTapFrames, step);
        }
        return;
    }
    if (o.talkActive) {
        ++mTalkFrames;
        steer({}, step);
        return;
    }
    if (o.demoActive) {
        steer({}, step);
        return;
    }
    // A launch star within reach, ready or holding Mario before the shot: spin.
    for (const Observation::Actor& actor : o.actors) {
        const float d = length(actorPos(actor) - pos);
        if (actor.kind == "LaunchStar" && (((actor.flags & kActorReady) && d < kLaunchReach) ||
                                           (actor.state == 2 && d < kLaunchHeldReach))) {
            steer({}, step);
            spin(step, "launch star");
            return;
        }
    }
    // Hanging on a vine: spin to climb; it throws Mario at the top.
    if (const Observation::Actor* vine = nearestActor(o, "Vine", pos, 600.0f, kActorBound)) {
        steer({}, step);
        if (mFrame - mLastSpin >= kVineSpinInterval) {
            mLastSpin = mFrame;
            tap(Button::Spin, kTapFrames, step);
            if (mFrame >= mLogAt) {
                mLogAt = mFrame + kLogInterval;
                note("climbing the vine at " + text(pos) + " (vine " + text(actorPos(*vine)) + ")");
            }
        }
        return;
    }
    // Airborne trace (every 20 frames past the first 20): where flights and
    // throws go, and in which gravity.
    if (!o.playerOnGround && (mAirTrace++ % 20) == 19) {
        note("in the air at " + text(pos) + ", up " + text(up(o)) + (o.playerInBind ? ", bound" : ""));
    } else if (o.playerOnGround) {
        mAirTrace = 0;
    }
    if (o.playerInBind) {
        // A launch star, a vine, a pipe: hands off until Mario lands again.
        mReleased = true;
        steer({}, step);
        return;
    }
    if (mReleased) {
        if (!o.playerOnGround) {
            steer({}, step);
            return;
        }
        mReleased = false;
        note("landed at " + text(pos) + ", up " + text(up(o)));
    }
    const Planet planet = planetAt({pos.x, pos.y, pos.z});
    if (planet != mPlanet && planet != Planet::None) {
        note(std::string("on ") + planetName(planet) + " at " + text(pos) + " (frame " + std::to_string(mMissionFrames) +
             " of the mission)");
        mPlanet = planet;
        mPlanetFrames = 0;
        mGoal.clear();
        resetStuck();
        mWaypointChosen = false;
    }
    if (planet == Planet::None) {
        // Between planets (a long jump or a throw): no steering.
        steer({}, step);
        return;
    }
    // Airborne for long (a throw, a fall): let the flight finish.
    mAirFrames = o.playerOnGround ? 0 : mAirFrames + 1;
    if (mAirFrames > kAirSteerFrames) {
        steer({}, step);
        return;
    }
    if (++mPlanetFrames > kPlanetLimit) {
        finish(Result::Fail, std::string("objective on ") + planetName(mPlanet) + " not done in " +
                                 std::to_string(kPlanetLimit) + " frames (at " + text(pos) + ", going to " + mGoal + ")",
               step);
        return;
    }
    // Rolling boulders: step off the line of one that is coming.
    if (dodgeRocks(o, step)) {
        return;
    }
    // Small enemies close by: spin (stuns Goombas and Octoombas, knocks down Piranha Plants).
    for (const Observation::Actor& actor : o.actors) {
        if ((actor.flags & kActorHostile) &&
            (actor.kind == "Goomba" || actor.kind == "Octoomba" || actor.kind == "PiranhaPlant" || actor.kind == "Karipon") &&
            length(actorPos(actor) - pos) < kEnemyReach) {
            spin(step, (actor.kind + " close").c_str());
            break;
        }
    }
    switch (mPlanet) {
    case Planet::DiskGarden: diskGarden(o, step); break;
    case Planet::Peanut: peanut(o, step); break;
    case Planet::BeanB: beanB(o, step); break;
    case Planet::FruitPeel: fruitPeel(o, step); break;
    case Planet::BeanC: beanC(o, step); break;
    case Planet::Dino: dino(o, step); break;
    default: steer({}, step); break;
    }
}

bool GoodEggDriver::dodgeRocks(const Observation& o, Step& step) {
    const Vec pos = position(o);
    const Vec u = up(o);
    // Where each boulder will be over the horizon: along its rail, or
    // straight on when it is off the rails.
    std::vector<std::array<Vec, kRockSteps + 1>> rocks;
    for (const Observation::Actor& actor : o.actors) {
        if (actor.kind != "Rock" || length(actorPos(actor) - pos) > kRockWatch) {
            continue;
        }
        const Vec rock = actorPos(actor);
        const Vec velocity{actor.dx, actor.dy, actor.dz};
        const float speed = length(velocity);
        std::array<Vec, kRockSteps + 1> path;
        const RailSpot on = nearestRail(rock, -1);
        for (int k = 0; k <= kRockSteps; ++k) {
            const float t = static_cast< float >(k * kRockStepFrames);
            path[k] = on.rail >= 0 && on.distance < kRockOffRail && speed > 1.0f
                          ? railAhead(on, railDirection(on, velocity), speed * t) + (rock - vec(kPeanutRails[on.rail][on.index]))
                          : rock + velocity * t;
        }
        rocks.push_back(path);
    }
    if (rocks.empty()) {
        return false;
    }
    // Candidate moves across the ground: toward the goal, eight directions
    // around it, and standing still; Mario runs about 8 units per frame.
    const Vec toGoal = mHasGoal ? normalized(across(vec(mGoalPoint) - pos, u)) : Vec{};
    const Vec side = length(toGoal) > 0.5f ? normalized(cross(u, toGoal)) : Vec{};
    auto clearance = [&](Vec dir) {
        float least = 1e30f;
        for (const auto& path : rocks) {
            for (int k = 0; k <= kRockSteps; ++k) {
                const Vec mario = pos + dir * (kMarioSpeed * static_cast< float >(k * kRockStepFrames));
                least = std::min(least, length(path[k] - mario));
            }
        }
        return least;
    };
    if (length(toGoal) < 0.5f || clearance(toGoal) >= kRockSafe) {
        return false;
    }
    Vec best{};
    float bestScore = -1e30f;
    float bestClearance = 0.0f;
    for (int i = 0; i <= 8; ++i) {
        Vec dir{};
        if (i < 8) {
            const float angle = static_cast< float >(i) * 0.785398f;
            dir = toGoal * std::cos(angle) + side * std::sin(angle);
        }
        const float c = clearance(dir);
        // Safe moves win, the nearer the goal's direction the better; else the
        // move that stays farthest from every boulder.
        const float score = c >= kRockSafe ? 10000.0f + (i < 8 ? dot(dir, toGoal) : -0.5f) : c;
        if (score > bestScore) {
            bestScore = score;
            best = dir;
            bestClearance = c;
        }
    }
    if (mFrame >= mRockLogAt) {
        mRockLogAt = mFrame + 60;
        note(std::string(length(best) < 0.5f ? "waiting for a boulder to pass" : "stepping around a boulder") +
             " (clearance " + number(bestClearance) + ", Mario " + text(pos) + ")");
    }
    steer(length(best) < 0.5f ? StickKeys{} : stickKeysForWorld(o, {best.x, best.y, best.z}), step);
    return true;
}

bool GoodEggDriver::seen(const char* milestone) const {
    return std::find(mSeen.begin(), mSeen.end(), milestone) != mSeen.end();
}

void GoodEggDriver::diskGarden(const Observation& o, Step& step) {
    const Vec pos = position(o);
    if (!mTalked) {
        // Past the rail's end: the Luma.
        if (!followRoute(o, kDiskGardenRoute, "Disk Garden rail", step)) {
            return;
        }
        const Observation::Actor* luma = nearestActor(o, "Luma", kDiskLuma, 600.0f);
        const Vec target = luma != nullptr ? actorPos(*luma) : kDiskLuma;
        if (mLumaAsked && mTalkFrames > 0) {
            // The talk ended (no page, no talk): the Luma becomes the Sling Star.
            mTalked = true;
            note("the Luma's talk ended after " + std::to_string(mTalkFrames) + " frames");
            return;
        }
        // It floats above the ground: stand under it until the game offers the
        // talk (the A balloon, Talk.Start), then A.
        if (!goTo(o, {target.x, target.y, target.z}, 90.0f, "the Luma", step, true)) {
            return;
        }
        if (!targetShown(o, "Talk.Start")) {
            if (mFrame >= mLogAt) {
                mLogAt = mFrame + kLogInterval;
                note("under the Luma at " + text(pos) + ", no talk offered yet");
            }
            return;
        }
        if (o.playerOnGround && (mLastA == 0 || mFrame - mLastA >= kTalkStartInterval)) {
            mLastA = mFrame;
            if (!mLumaAsked) {
                mLumaAsked = true;
                mTalkFrames = 0;
            }
            note("tap A: talk to the Luma at " + text(target) + " from " + text(pos));
            tap(Button::A, kTapFrames, step);
        }
        return;
    }
    const Observation::Actor* sling = nearestActor(o, "SlingStar", kDiskLuma, 800.0f);
    if (sling == nullptr) {
        steer({}, step);
        if (mFrame >= mLogAt) {
            mLogAt = mFrame + kLogInterval;
            note("waiting for the Sling Star at " + text(pos));
        }
        return;
    }
    const Vec target = actorPos(*sling);
    if (goTo(o, {target.x, target.y, target.z}, 90.0f, "the Sling Star", step, true) && (sling->flags & kActorReady)) {
        spin(step, "sling star");
    }
}

void GoodEggDriver::peanut(const Observation& o, Step& step) {
    const Vec pos = position(o);
    const Vec u = up(o);
    int got = 0;
    for (const std::string& m : mSeen) {
        got += m == "StarChip.Got";
    }
    // The tour first (it passes every chip); then any chip it missed, directly.
    if (got < kPeanutChipCount && !mPeanutToured) {
        if (!followRoute(o, kPeanutRoute, "Peanut route", step)) {
            return;
        }
        mPeanutToured = true;
        note("Peanut route done with " + std::to_string(got) + " of 5 chips");
    }
    if (got >= kPeanutChipCount) {
        // It floats about 300 above the ground: stand under it (it pulls Mario in).
        goTo(o, {kPeanutLaunchStar.x, kPeanutLaunchStar.y, kPeanutLaunchStar.z}, 60.0f, "the Peanut launch star", step,
             true);
        return;
    }
    // The nearest chip still to collect: published ready chips first, then the
    // placement spots not yet reached.
    Vec target{};
    bool found = false;
    if (const Observation::Actor* chip = nearestActor(o, "StarChip", pos, 3000.0f, kActorReady)) {
        target = actorPos(*chip);
        found = true;
    }
    if (!found) {
        float best = 1e30f;
        for (const Vec& spot : kPeanutChips) {
            const bool done = std::any_of(mChipsDone.begin(), mChipsDone.end(),
                                          [&](const Point3& p) { return length(vec(p) - spot) < 1.0f; });
            if (!done && length(spot - pos) < best) {
                best = length(spot - pos);
                target = spot;
                found = true;
            }
        }
    }
    if (!found) {
        finish(Result::Fail, std::to_string(got) + " of 5 Star Chips collected and none left to find on the Peanut", step);
        return;
    }
    const Vec delta = target - pos;
    const float height = dot(delta, u);
    const float flat = length(across(delta, u));
    if (flat < 110.0f && height > 90.0f && o.playerOnGround && mFrame - mLastA >= 40) {
        mLastA = mFrame;
        note("tap A: jump for the chip " + number(height) + " above at " + text(pos));
        tap(Button::A, 12, step);
    }
    if (goTo(o, {target.x, target.y, target.z}, 60.0f, "a Star Chip", step)) {
        for (const Vec& spot : kPeanutChips) {
            if (length(spot - target) < 150.0f) {
                mChipsDone.push_back({spot.x, spot.y, spot.z});
            }
        }
    }
}

void GoodEggDriver::beanB(const Observation& o, Step& step) {
    if (const Observation::Actor* vine = nearestActor(o, "Vine", kBeanBVine, 1500.0f)) {
        // Grown or growing: spin into its stalk (a spin on the ground grabs it).
        const Vec base = kBeanBVine;
        if (goTo(o, {base.x, base.y, base.z}, 90.0f, "the Bean B vine", step)) {
            spin(step, vine->state == 1 ? "grab the growing vine" : "grab the vine");
        }
        return;
    }
    const Observation::Actor* plant = nearestActor(o, "PiranhaPlant", kBeanBPiranha, 500.0f);
    const Vec target = plant != nullptr ? actorPos(*plant) : kBeanBPiranha;
    if (goTo(o, {target.x, target.y, target.z}, 160.0f, "the Bean B Piranha Plant", step)) {
        spin(step, "the Piranha Plant");
    }
}

void GoodEggDriver::fruitPeel(const Observation& o, Step& step) {
    // Up the spiral first: straight lines toward the top would leave the peel.
    if (!followRoute(o, kFruitPeelRoute, "Fruit Peel route", step)) {
        return;
    }
    if (nearestActor(o, "Vine", kFruitVine, 1500.0f) != nullptr) {
        if (goTo(o, {kFruitVine.x, kFruitVine.y, kFruitVine.z}, 90.0f, "the Fruit Peel vine", step)) {
            spin(step, "grab the vine");
        }
        return;
    }
    const Observation::Actor* head = nearestActor(o, "HammerHead", kHammerHead, 1200.0f);
    if (head != nullptr && (head->flags & kActorReady)) {
        // Its head is down: a spin swoons it (a longer chance), landing on the
        // head kills it (HammerHeadPackun::receiveMsgPlayerAttackChance).
        const Vec target = actorPos(*head);
        const Vec pos = position(o);
        const float flat = length(across(target - pos, up(o)));
        if (flat < 170.0f && mLastSpin + 240 < mFrame) {
            spin(step, "the Hammer Head's head");
        }
        if (flat < 150.0f && o.playerOnGround && mFrame - mLastA >= 40) {
            mLastA = mFrame;
            note("tap A: jump onto the Hammer Head at " + text(pos));
            tap(Button::A, 12, step);
        }
        // Keep steering onto it, in the air too.
        goTo(o, {target.x, target.y, target.z}, 15.0f, "the Hammer Head's head", step, true);
        return;
    }
    // Draw its attack: stand near its base (the stem's foot), where it sees
    // Mario, and let the slam land; then the head lies still (READY).
    goTo(o, {kHammerHead.x, kHammerHead.y, kHammerHead.z}, 450.0f, "near the Hammer Head", step);
}

void GoodEggDriver::beanC(const Observation& o, Step& step) {
    if (const Observation::Actor* star = nearestActor(o, "LaunchStar", kBeanCLaunchStar, 600.0f)) {
        const Vec target = actorPos(*star);
        goTo(o, {target.x, target.y, target.z}, 60.0f, "the Bean C launch star", step, true);
        return;
    }
    if (length(kBeanCCage - position(o)) > 700.0f && !followRoute(o, kBeanCRoute, "Bean C route", step)) {
        return;
    }
    const Observation::Actor* cage = nearestActor(o, "CrystalCage", kBeanCCage, 600.0f);
    const Vec target = cage != nullptr ? actorPos(*cage) : kBeanCCage;
    if (goTo(o, {target.x, target.y, target.z}, 140.0f, "the Bean C crystal", step)) {
        spin(step, "the crystal");
    }
}

void GoodEggDriver::dino(const Observation& o, Step& step) {
    const Vec pos = position(o);
    const Vec u = up(o);
    if (const Observation::Actor* star = nearestActor(o, "PowerStar", pos, 3000.0f, kActorReady)) {
        const Vec target = actorPos(*star);
        const Vec delta = target - pos;
        const float height = dot(delta, u);
        if (length(across(delta, u)) < 90.0f && height > 120.0f && o.playerOnGround && mFrame - mLastA >= 40) {
            mLastA = mFrame;
            note("tap A: jump for the Power Star " + number(height) + " above at " + text(pos));
            tap(Button::A, 12, step);
        }
        goTo(o, {target.x, target.y, target.z}, 40.0f, "the Power Star", step);
        return;
    }
    const Observation::Actor* dino = nearestActor(o, "DinoPiranha", kDinoCentre, 3000.0f);
    const Observation::Actor* ball = nearestActor(o, "DinoBall", kDinoCentre, 3000.0f);
    if (dino == nullptr || ball == nullptr) {
        steer({}, step);
        if (mFrame >= mLogAt) {
            mLogAt = mFrame + kLogInterval;
            note("Dino Piranha not published; waiting at " + text(pos));
        }
        return;
    }
    if (dino->state != mLastDinoPhase) {
        note("Dino Piranha phase " + std::to_string(dino->state) + " at " + text(actorPos(*dino)));
        if (mLastDinoPhase == 1 || mLastDinoPhase == 3 || mLastDinoPhase == 5 || mLastDinoPhase == 6) {
            ++mDinoHits;
        }
        mLastDinoPhase = dino->state;
        mGoal.clear();
    }
    const bool battle = dino->state == 1 || dino->state == 3 || dino->state == 5 || dino->state == 6;
    if (!battle) {
        steer({}, step);
        return;
    }
    const Vec dinoPos = actorPos(*dino);
    const Vec ballPos = actorPos(*ball);
    if (!(ball->flags & kActorReady)) {
        // The ball is flying back at its head: keep away.
        const Vec away = normalized(across(pos - dinoPos, u));
        const Vec target = pos + away * 300.0f;
        goTo(o, {target.x, target.y, target.z}, 50.0f, "away from Dino Piranha", step);
        return;
    }
    const float toBall = length(ballPos - pos);
    if (toBall < 150.0f) {
        spin(step, "Dino Piranha's tail ball");
    }
    // Behind the ball (away from the body), around the head when it is between.
    const Vec behind = ballPos + normalized(across(ballPos - dinoPos, u)) * 120.0f;
    const Vec head = dinoPos + Vec{dino->dx, dino->dy, dino->dz} * 200.0f;
    Vec target = behind;
    const Vec toTarget = behind - pos;
    const Vec toHead = head - pos;
    const float along = dot(toHead, normalized(toTarget));
    if (along > 0.0f && along < length(toTarget) && length(toHead - normalized(toTarget) * along) < 350.0f) {
        // Detour: sideways from the head.
        const Vec side = normalized(cross(u, toTarget));
        const float sign = dot(side, pos - dinoPos) >= 0.0f ? 1.0f : -1.0f;
        target = dinoPos + side * (sign * 500.0f);
    }
    goTo(o, {target.x, target.y, target.z}, 60.0f, "behind Dino Piranha's tail", step);
}

void GoodEggDriver::starGet(const Observation& o, Step& step) {
    steer({}, step);
    if (!mStarGot) {
        mStarGot = true;
    }
    if (o.stage == "AstroDome" || o.stage == "AstroGalaxy") {
        note("back in " + o.stage + " " + std::to_string(mPhaseFrames) + " frames after the star");
        mPhase = Phase::Return;
        mPhaseFrames = 0;
        return;
    }
    if (mPhaseFrames >= kStarGetLimit) {
        finish(Result::Fail, "no return to the observatory within " + std::to_string(kStarGetLimit) +
                                 " frames of the Power Star (scene " + o.scene + ", stage " + o.stage + ")",
               step);
    }
}

void GoodEggDriver::returnToDome(const Observation& o, Step& step) {
    if (o.stage != "AstroDome" && o.stage != "AstroGalaxy" && !o.stage.empty()) {
        finish(Result::Fail, "left the observatory for " + o.stage + " before the save", step);
        return;
    }
    if (mPhaseFrames >= kReturnLimit) {
        finish(Result::Fail, std::string("the return did not finish within ") + std::to_string(kReturnLimit) +
                                 " frames (save prompt " + (mSaveShown ? "shown" : "not shown") + ", answered " +
                                 (mSaveAnswered ? "yes" : "no") + ", saved " + (mSaveDone ? "yes" : "no") +
                                 ", star recorded " + (o.starEggStar1 ? "yes" : "no") + ")",
               step);
        return;
    }
    if (!mPrompt.empty()) {
        // Point at Yes; A once the game reports the pointer over it.
        const Observation::Target* yes = nullptr;
        for (const Observation::Target& t : o.targets) {
            if (t.id == mPrompt && (t.flags & kTargetSelectable)) {
                yes = &t;
            }
        }
        if (yes == nullptr) {
            return;
        }
        step.pointer = true;
        step.pointerU = yes->u;
        step.pointerV = yes->v;
        step.assertFocus = true;
        if (mAimFrames++ == 0) {
            note("point at " + mPrompt + " (" + std::to_string(yes->u) + ", " + std::to_string(yes->v) + ")");
        }
        mPointingFrames = (yes->flags & kTargetPointing) ? mPointingFrames + 1 : 0;
        if (mPointingFrames >= kPointingFramesToPress) {
            note("tap A: " + mPrompt + " (save)");
            tap(Button::A, kTapFrames, step);
            mPrompt.clear();
            mSaveAnswered = true;
        } else if (mAimFrames >= kAimLimit) {
            finish(Result::Fail, "the pointer never got over " + mPrompt, step);
        }
        return;
    }
    if (mSaveDone && mLastA != 0 && mFrame - mLastA == 45) {
        note("tap A: save finished window");
        tap(Button::A, kTapFrames, step);
        return;
    }
    if (targetShown(o, "Talk.Advance")) {
        if (mFrame - mLastA >= kTalkTapInterval) {
            mLastA = mFrame;
            note("tap A: talk page");
            tap(Button::A, kTapFrames, step);
        }
        return;
    }
    mIdleSaveFrames = o.saveSequence ? 0 : mIdleSaveFrames + 1;
    const bool ready = o.scene == "Game" && o.sceneReady && o.playerValid && !o.demoActive && o.pausePermitted &&
                       !o.talkActive;
    mReadyFrames = ready ? mReadyFrames + 1 : 0;
    if (mSaveAnswered && mSaveDone && mIdleSaveFrames >= kSaveIdleFrames && mReadyFrames >= kDomeReadyFrames) {
        if (!o.starEggStar1) {
            finish(Result::Fail, "back in " + o.stage + " and saved, but the file does not record Good Egg mission 1's star",
                   step);
        } else if (mStarsAtStart >= 0 && o.powerStars != mStarsAtStart + 1) {
            finish(Result::Fail, "the file has " + std::to_string(o.powerStars) + " stars, expected " +
                                     std::to_string(mStarsAtStart + 1),
                   step);
        } else {
            finish(Result::Pass,
                   "Good Egg mission 1: Dino Piranha defeated (" + std::to_string(mDinoHits) + " tail hits seen), " +
                       "Power Star collected, returned to " + o.stage + ", saved (System_Save00 yes, System_Save02), " +
                       "file records the star (" + std::to_string(o.powerStars) + " stars, was " +
                       std::to_string(mStarsAtStart) + ")",
                   step);
        }
    }
}

}  // namespace PetariNative::App::Smoke

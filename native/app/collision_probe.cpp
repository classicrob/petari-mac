// Map collision probe (PETARI_COLLISION_PROBE=<csv path>), for planning input
// routes offline (native/tools/observatory_routes.py). Read-only: map ray casts
// through the game's own collision (MR::getFirstPolyOnLineToMap) once the stage
// named by PETARI_COLLISION_PROBE_STAGE (default AstroGalaxy) is ready, so
// animated map parts are measured in their current, not their placed, pose.
// Runs a few milliseconds per frame at the seam, while this thread owns the game.
//
// PETARI_COLLISION_PROBE_NEAR="x,z,r" delays the survey until Mario is within r of
// (x, z): collision of map parts far from the player may be inactive.
//
// CSV rows: x,z,y,nx,ny,nz,clear
//   one row per floor or surface hit on a 50-unit grid, top to bottom;
//   clear: for upward-facing hits (ny > 0.3), bit d set when a 50-unit
//   horizontal ray in direction d (0..7: +x, +x+z, +z, -x+z, -x, -x-z, -z, +x-z)
//   from 40 and from 110 units above the hit meets no map or move-limit collision; else -1.

#include "Game/Map/HitInfo.hpp"
#include "Game/Util/MapUtil.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "smoke.hpp"

namespace PetariNative::App::Smoke {

namespace {

struct Sample {
    float x, z, y, nx, ny, nz;
    int clear;
};

constexpr float kCell = 50.0f;
constexpr float kTop = 12000.0f;
constexpr float kBottom = -12000.0f;

struct Probe {
    bool configured = false;
    bool enabled = false;
    bool done = false;
    std::string path;
    std::string stage;
    float minX = -9000.0f, maxX = 14000.0f, minZ = -7000.0f, maxZ = 9000.0f;
    long column = 0;
    float nearX = 0.0f, nearZ = 0.0f, nearRadius = 0.0f;  // PETARI_COLLISION_PROBE_NEAR
    long budgetMs = 12;  // per frame; PETARI_COLLISION_PROBE_BUDGET_MS (a large value probes in one frame)
    std::vector<Sample> samples;
};

Probe gProbe;

float envFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::strtof(value, nullptr) : fallback;
}

void configure() {
    gProbe.configured = true;
    const char* path = std::getenv("PETARI_COLLISION_PROBE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    const char* stage = std::getenv("PETARI_COLLISION_PROBE_STAGE");
    gProbe.enabled = true;
    gProbe.path = path;
    gProbe.stage = stage != nullptr && stage[0] != '\0' ? stage : "AstroGalaxy";
    gProbe.minX = envFloat("PETARI_COLLISION_PROBE_MIN_X", gProbe.minX);
    gProbe.maxX = envFloat("PETARI_COLLISION_PROBE_MAX_X", gProbe.maxX);
    gProbe.minZ = envFloat("PETARI_COLLISION_PROBE_MIN_Z", gProbe.minZ);
    gProbe.maxZ = envFloat("PETARI_COLLISION_PROBE_MAX_Z", gProbe.maxZ);
    gProbe.budgetMs = static_cast<long>(envFloat("PETARI_COLLISION_PROBE_BUDGET_MS", 12.0f));
    if (const char* near = std::getenv("PETARI_COLLISION_PROBE_NEAR")) {
        std::sscanf(near, "%f,%f,%f", &gProbe.nearX, &gProbe.nearZ, &gProbe.nearRadius);
    }
}

bool hitMap(const TVec3f& from, const TVec3f& segment, TVec3f* pHit, Triangle* pTriangle) {
    return MR::getFirstPolyOnLineToMap(pHit, pTriangle, from, segment);
}

int clearDirections(const TVec3f& floor) {
    static const float kDirections[8][2] = {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {-0.7071f, 0.7071f},
                                            {-1, 0}, {-0.7071f, -0.7071f}, {0, -1}, {0.7071f, -0.7071f}};
    int clear = 0;
    for (int d = 0; d < 8; d++) {
        bool blocked = false;
        for (float height : {40.0f, 110.0f}) {
            TVec3f from(floor.x, floor.y + height, floor.z);
            TVec3f segment(kDirections[d][0] * kCell, 0.0f, kDirections[d][1] * kCell);
            TVec3f hit;
            Triangle triangle;
            // Map collision and invisible move-limit walls (edges Mario cannot drop from).
            if (MR::getFirstPolyOnLineToMapAndMoveLimit(&hit, &triangle, from, segment)) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            clear |= 1 << d;
        }
    }
    return clear;
}

void probeColumn(float x, float z) {
    float top = kTop;
    for (int layer = 0; layer < 16 && top > kBottom; layer++) {
        TVec3f from(x, top, z);
        TVec3f segment(0.0f, kBottom - top, 0.0f);
        TVec3f hit;
        Triangle triangle;
        if (!hitMap(from, segment, &hit, &triangle)) {
            return;
        }
        const TVec3f* pNormal = triangle.getFaceNormal();
        const TVec3f normal = pNormal != nullptr ? *pNormal : TVec3f(0.0f, 1.0f, 0.0f);
        gProbe.samples.push_back({x, z, hit.y, normal.x, normal.y, normal.z, normal.y > 0.3f ? clearDirections(hit) : -1});
        top = hit.y - 20.0f;
    }
}

void write() {
    std::FILE* out = std::fopen(gProbe.path.c_str(), "w");
    if (out == nullptr) {
        std::fprintf(stderr, "PETARI PROBE: cannot write %s\n", gProbe.path.c_str());
        return;
    }
    std::fprintf(out, "x,z,y,nx,ny,nz,clear\n");
    for (const Sample& s : gProbe.samples) {
        std::fprintf(out, "%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%d\n", s.x, s.z, s.y, s.nx, s.ny, s.nz, s.clear);
    }
    std::fclose(out);
}

}  // namespace

bool collisionProbeActive() {
    if (!gProbe.configured) {
        configure();
    }
    return gProbe.enabled && !gProbe.done;
}

void stepCollisionProbe(const std::string& stage, bool sceneReady, bool playerValid, float playerX, float playerZ) {
    if (!collisionProbeActive() || !sceneReady || stage != gProbe.stage) {
        return;
    }
    if (gProbe.column == 0 && gProbe.nearRadius > 0.0f &&
        (!playerValid || std::hypot(playerX - gProbe.nearX, playerZ - gProbe.nearZ) > gProbe.nearRadius)) {
        return;  // not there yet: map parts far from the player may have no active collision
    }
    const long columnsX = static_cast<long>(std::ceil((gProbe.maxX - gProbe.minX) / kCell));
    const long columnsZ = static_cast<long>(std::ceil((gProbe.maxZ - gProbe.minZ) / kCell));
    const long total = columnsX * columnsZ;
    if (gProbe.column == 0) {
        std::fprintf(stderr, "PETARI PROBE: %s ready; probing %ld columns of %.0f units\n", stage.c_str(), total, kCell);
    }
    const auto start = std::chrono::steady_clock::now();
    while (gProbe.column < total &&
           std::chrono::steady_clock::now() - start < std::chrono::milliseconds(gProbe.budgetMs)) {
        const long i = gProbe.column % columnsX;
        const long j = gProbe.column / columnsX;
        probeColumn(gProbe.minX + (i + 0.5f) * kCell, gProbe.minZ + (j + 0.5f) * kCell);
        gProbe.column++;
    }
    if (gProbe.column >= total) {
        write();
        gProbe.done = true;
        std::fprintf(stderr, "PETARI PROBE: done, %zu samples written to %s\n", gProbe.samples.size(), gProbe.path.c_str());
        gProbe.samples.clear();
        gProbe.samples.shrink_to_fit();
    }
}

}  // namespace PetariNative::App::Smoke

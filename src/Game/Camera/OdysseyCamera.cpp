// Native mod: Odyssey-style orbit camera (docs/dev/ODYSSEY_CAMERA.md). Off by
// default; CameraDirector::movement calls petariOdysseyCameraApply between
// calcPose and createViewMtx, and with the mod off it returns at once.
#ifdef PETARI_NATIVE
#include "Game/Camera/CameraDirector.hpp"
#include "Game/Camera/CameraHolder.hpp"
#include "Game/Camera/CameraManGame.hpp"
#include "Game/Camera/CameraParamChunk.hpp"
#include "Game/Camera/CameraPoseParam.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include <petari/camera_input.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
    struct V {
        float x, y, z;
    };
    V vec(const TVec3f& a) { return {a.x, a.y, a.z}; }
    TVec3f tvec(const V& a) { return TVec3f(a.x, a.y, a.z); }
    V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    V mul(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float len(V a) { return std::sqrt(dot(a, a)); }
    bool unit(V* a) {
        const float l = len(*a);
        if (l < 1e-4f) return false;
        *a = mul(*a, 1.0f / l);
        return true;
    }
    // Rodrigues: a rotated by angle (radians) about the unit axis k.
    V rotate(V a, V k, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        return add(add(mul(a, c), mul(cross(k, a), s)), mul(k, dot(k, a) * (1.0f - c)));
    }
    // a rotated by the rotation taking unit `from` to unit `to`, at most maxAngle.
    V transport(V a, V from, V to, float maxAngle, V* reached) {
        V axis = cross(from, to);
        const float s = len(axis);
        const float angle = std::atan2(s, dot(from, to));
        if (s < 1e-6f || angle < 1e-6f) {
            *reached = to;
            return a;
        }
        axis = mul(axis, 1.0f / s);
        const float step = angle < maxAngle ? angle : maxAngle;
        *reached = rotate(from, axis, step);
        return rotate(a, axis, step);
    }
    float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

    constexpr float kPi = 3.14159265f;
    constexpr float kDeg = kPi / 180.0f;
    // Values marked SMO come from MonsterDruide1/OdysseyDecomp (docs/dev/ODYSSEY_CAMERA.md);
    // the rest are tuned for Galaxy (its follow camera classes are not decompiled yet).
    constexpr float kTargetLift = 180.0f;     // SMO: CameraOffsetPreset "Default" (0, 180, 0) above the feet
    constexpr float kAbsorbBelow = 200.0f;    // SMO: CameraVerticalAbsorber AbsorbScreenPosUp -200
    constexpr float kAbsorbAbove = 480.0f;    // SMO: AbsorbScreenPosDown 480
    constexpr float kHighJumpSpeed = 35.0f;   // SMO: HighJumpJudgeSpeedV 35 per frame
    constexpr float kAbsorbDecay = 0.8f;      // SMO: leftover offset x0.8 per frame
    constexpr float kMinDist = 400.0f, kMaxDist = 2400.0f;
    constexpr float kMinElev = -60.0f * kDeg, kMaxElev = 70.0f * kDeg;
    constexpr float kMaxTurn = 20.0f * kDeg;  // per frame; Mario snaps his camera frame above 30
    constexpr float kMaxUpTurn = 4.0f * kDeg; // gravity changes are followed at most this fast
    constexpr float kWallMargin = 60.0f;

    struct Orbit {
        bool active = false;
        V dir{0, 0, 1};  // target -> eye, unit
        V up{0, 1, 0};
        float dist = 1000.0f, wantDist = 1000.0f, shownDist = 1000.0f;
        float idleFrames = 0.0f;
        int recentreFrames = 0;
        float absorb = 0.0f;  // target height held below Mario's (along up) while he jumps
    } gOrbit;

    // SMO speed levels -2..+2 (CameraPoserFunction getStickSensitivityScale), with the
    // middle level at 1.0 rather than the decomp's 1.6.
    float sensitivity(int speed) {
        static const float kScale[5] = {0.44f, 0.72f, 1.0f, 1.27f, 1.55f};
        return kScale[speed < 1 ? 0 : speed > 5 ? 4 : speed - 1];
    }

    // Follow-style cameras the orbit replaces; everything else stays scripted.
    bool isOrbitType(const char* name) {
        static const char* const kTypes[] = {
            "CAM_TYPE_FOLLOW",        "CAM_TYPE_TOWER",         "CAM_TYPE_WONDER_PLANET",  "CAM_TYPE_MEDIAN_PLANET",
            "CAM_TYPE_MEDIAN_TOWER",  "CAM_TYPE_CUBE_PLANET",   "CAM_TYPE_XZ_PARA",        "CAM_TYPE_SLIDER",
            "CAM_TYPE_WATER_FOLLOW",  "CAM_TYPE_WATER_PLANET",  "CAM_TYPE_WATER_PLANET_BOSS", "CAM_TYPE_INWARD_TOWER",
            "CAM_TYPE_INWARD_SPHERE", "CAM_TYPE_TWISTED_PASSAGE", "CAM_TYPE_INNER_CYLINDER", "CAM_TYPE_RACE_FOLLOW",
        };
        for (const char* type : kTypes) {
            if (std::strcmp(name, type) == 0) return true;
        }
        return false;
    }

    const char* orbitBlocker(CameraDirector* pDirector) {
        if (pDirector->getCurrentCameraMan() != reinterpret_cast< CameraMan* >(pDirector->mCameraManGame)) return "camera man";
        if (MR::isEventCameraActive()) return "event camera";
        if (MR::isFirstPersonCamera()) return "first person";
        if (MR::isDemoActive()) return "demo";
        if (MR::isPlayerDead()) return "dead";
        if (MR::isPlayerInBind()) return "bind";
        const CameraManGame* pGame = pDirector->mCameraManGame;
        if (pGame->mChunk == nullptr) return "no chunk";
        const char* type = pDirector->getHolder()->getNameStrOf(pGame->mChunk->mCameraTypeIndex);
        if (type == nullptr || !isOrbitType(type)) return type != nullptr ? type : "no type";
        return nullptr;
    }
}  // namespace

void petariOdysseyCameraApply(CameraDirector* pDirector) {
    PetariCameraInput in;
    petari_camera_take_input(&in);
    if (!in.enabled) {
        gOrbit.active = false;
        return;
    }
    static const char* sLastBlocker = "";
    const char* blocker = orbitBlocker(pDirector);
    if (blocker != nullptr) {
        if (gOrbit.active || std::strcmp(blocker, sLastBlocker) != 0) {
            std::fprintf(stderr, "[odyssey camera] scripted camera (%s)\n", blocker);
        }
        sLastBlocker = blocker;
        gOrbit.active = false;
        return;
    }
    sLastBlocker = "";

    CameraPoseParam* pPose = pDirector->mPoseParam1;
    V gravityUp = mul(vec(*MR::getPlayerGravity()), -1.0f);
    if (!unit(&gravityUp)) gravityUp = vec(pPose->mUpVec);
    unit(&gravityUp);
    const V feet = vec(*MR::getPlayerPos());

    if (!gOrbit.active) {
        // Seed from the game's camera so taking over is seamless.
        gOrbit.up = gravityUp;
        gOrbit.absorb = 0.0f;
        const V target = add(feet, mul(gOrbit.up, kTargetLift));
        V dir = sub(vec(pPose->mPos), target);
        const float d = len(dir);
        if (!unit(&dir)) dir = mul(vec(MR::getCamZdir()), -1.0f);
        gOrbit.dir = dir;
        gOrbit.dist = gOrbit.wantDist = gOrbit.shownDist = clampf(d, kMinDist, kMaxDist);
        gOrbit.idleFrames = 0.0f;
        gOrbit.recentreFrames = 0;
        gOrbit.active = true;
        std::fprintf(stderr, "[odyssey camera] orbit on, distance %.0f\n", gOrbit.dist);
    }

    // Ride the gravity: carry the orbit with Mario's up (bounded per frame).
    V reachedUp;
    gOrbit.dir = transport(gOrbit.dir, gOrbit.up, gravityUp, kMaxUpTurn, &reachedUp);
    gOrbit.up = reachedUp;
    unit(&gOrbit.up);
    unit(&gOrbit.dir);
    const V up = gOrbit.up;
    // Jump absorb (SMO CameraVerticalAbsorber): in the air the target keeps its
    // height until Mario leaves the band or jumps high; on the ground the
    // leftover offset fades.
    const float vertical = dot(vec(*MR::getPlayerVelocity()), up);
    if (MR::isOnGroundPlayer() || vertical > kHighJumpSpeed) {
        gOrbit.absorb *= kAbsorbDecay;
    } else {
        // absorb > 0: the target sits that far below Mario's lifted point.
        gOrbit.absorb = clampf(gOrbit.absorb + vertical, -kAbsorbBelow, kAbsorbAbove);
    }
    const V target = add(feet, mul(up, kTargetLift - gOrbit.absorb));

    // Input: degrees this frame.
    const float rate = 1.2f * sensitivity(in.speed);
    const float sx = in.invertX ? -1.0f : 1.0f, sy = in.invertY ? -1.0f : 1.0f;
    float yaw = (in.stickX * 3.0f + in.yawHold * 2.0f) * rate + in.mouseYaw * rate / 1.2f;
    float pitch = (in.stickY * 2.0f + in.pitchHold * 1.5f) * rate + in.mousePitch * rate / 1.2f;
    yaw *= sx * kDeg;
    pitch *= sy * kDeg;
    yaw = clampf(yaw, -kMaxTurn, kMaxTurn);
    pitch = clampf(pitch, -kMaxTurn, kMaxTurn);
    const bool input = yaw != 0.0f || pitch != 0.0f;

    // Horizontal part and elevation of the orbit direction.
    V horizontal = sub(gOrbit.dir, mul(up, dot(gOrbit.dir, up)));
    float elevation = std::asin(clampf(dot(gOrbit.dir, up), -1.0f, 1.0f));
    if (!unit(&horizontal)) {
        horizontal = sub(mul(vec(MR::getCamZdir()), -1.0f), mul(up, dot(mul(vec(MR::getCamZdir()), -1.0f), up)));
        if (!unit(&horizontal)) horizontal = cross(up, V{1, 0, 0});
        unit(&horizontal);
    }
    // View turns right with positive yaw: the eye goes around the other way.
    horizontal = rotate(horizontal, up, -yaw);
    elevation = clampf(elevation + pitch, kMinElev, kMaxElev);

    if (in.recenter) gOrbit.recentreFrames = 18;
    if (input) {
        gOrbit.idleFrames = 0.0f;
        gOrbit.recentreFrames = 0;
    } else {
        gOrbit.idleFrames += 1.0f;
    }
    V wantHorizontal = horizontal;
    float wantElevation = elevation;
    float maxStep = 0.0f;
    bool recentring = false;
    if (gOrbit.recentreFrames > 0) {
        // C: behind Mario's facing, 15 degrees up, over ~0.3 s.
        TVec3f front;
        MR::getPlayerFrontVec(&front);
        V back = mul(vec(front), -1.0f);
        back = sub(back, mul(up, dot(back, up)));
        if (unit(&back)) wantHorizontal = back;
        wantElevation = 15.0f * kDeg;
        maxStep = kMaxTurn;
        recentring = true;
        --gOrbit.recentreFrames;
    } else if (gOrbit.idleFrames > 120.0f) {
        // Idle for 2 s: drift behind Mario's movement, never toward the camera.
        V velocity = vec(*MR::getPlayerVelocity());
        velocity = sub(velocity, mul(up, dot(velocity, up)));
        if (len(velocity) > 3.0f && dot(velocity, horizontal) < 0.5f * len(velocity)) {
            V back = mul(velocity, -1.0f);
            if (unit(&back)) wantHorizontal = back;
            maxStep = 0.75f * kDeg;
        }
    }
    if (maxStep > 0.0f) {
        const float angle = std::atan2(dot(cross(horizontal, wantHorizontal), up), dot(horizontal, wantHorizontal));
        horizontal = rotate(horizontal, up, clampf(angle, -maxStep, maxStep));
        if (recentring) {
            elevation += clampf(wantElevation - elevation, -maxStep, maxStep);
        }
    }
    gOrbit.dir = add(mul(horizontal, std::cos(elevation)), mul(up, std::sin(elevation)));
    unit(&gOrbit.dir);

    // Zoom (eased) and line of sight to the eye.
    gOrbit.wantDist = clampf(gOrbit.wantDist + in.zoomSteps * 150.0f + in.zoomHold * 25.0f, kMinDist, kMaxDist);
    gOrbit.dist += (gOrbit.wantDist - gOrbit.dist) * 0.15f;
    float allowed = gOrbit.dist;
    TVec3f hit;
    const TVec3f from = tvec(target);
    const TVec3f ray = tvec(mul(gOrbit.dir, gOrbit.dist));
    if (MR::getFirstPolyOnLineToMap(&hit, nullptr, from, ray)) {
        const float wall = len(sub(vec(hit), target)) - kWallMargin;
        allowed = clampf(wall, 120.0f, gOrbit.dist);
    }
    // Pull in at once, ease back out.
    gOrbit.shownDist = allowed < gOrbit.shownDist ? allowed : gOrbit.shownDist + (allowed - gOrbit.shownDist) * 0.1f;

    const V eye = add(target, mul(gOrbit.dir, gOrbit.shownDist));
    static const bool sTrace = std::getenv("PETARI_CAMERA_TRACE") != nullptr;
    static unsigned sTraceFrames = 0;
    if (sTrace && ++sTraceFrames % 60 == 0) {
        std::fprintf(stderr, "[odyssey camera] distance %.0f (wanted %.0f, wall-limited %d), elevation %.1f deg, heading (%.2f, %.2f, %.2f)\n",
                     gOrbit.shownDist, gOrbit.wantDist, allowed < gOrbit.dist ? 1 : 0, elevation / kDeg, horizontal.x,
                     horizontal.y, horizontal.z);
    }
    pPose->mWatchPos.set(target.x, target.y, target.z);
    pPose->mPos.set(eye.x, eye.y, eye.z);
    pPose->mUpVec.set(up.x, up.y, up.z);
}
#endif

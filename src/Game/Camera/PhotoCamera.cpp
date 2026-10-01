#ifdef PETARI_NATIVE
// Photo mode: a free camera over the frozen game (docs/dev/ODYSSEY_CAMERA.md).
#include "Game/Camera/PhotoCamera.hpp"
#include "Game/AudioLib/AudSystem.hpp"
#include "Game/AudioLib/AudWrap.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include <petari/camera_input.h>
#include <petari/screenshot.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace {
    struct V {
        float x, y, z;
    };
    V add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    V mul(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    V normalized(V a, V fallback) {
        const float length = std::sqrt(dot(a, a));
        return length > 1e-6f ? mul(a, 1.0f / length) : fallback;
    }
    // Rodrigues rotation of v about the unit axis k.
    V rotate(V v, V k, float angle) {
        const float c = std::cos(angle), s = std::sin(angle);
        return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1.0f - c)));
    }

    constexpr float kDeg = 3.14159265f / 180.0f;
    constexpr float kSpeed = 20.0f;  // units per frame at normal speed

    struct State {
        bool active = false;
        TPos3f savedView;
        float savedFovy = 45.0f;
        V eye{0, 0, 0}, forward{0, 0, -1}, up{0, 1, 0};  // up: the camera's up on entry, kept while flying
        float fovy = 45.0f;
        int shots = 0;
        unsigned long sceneFrames = 0;
    } gState;

    // The game state photo mode must not change (compared on entry and exit).
    void logGameState(const char* when) {
        const TVec3f* pos = MR::getPlayerPos();
        const TVec3f* vel = MR::getPlayerVelocity();
        const TPos3f& view = MR::getCameraViewMtx();
        unsigned long long hash = 1469598103934665603ULL;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                unsigned int bits;
                const float value = view.mMtx[r][c];
                std::memcpy(&bits, &value, sizeof(bits));
                hash = (hash ^ bits) * 1099511628211ULL;
            }
        }
        std::fprintf(stderr,
                     "[photo] %s: scene frame %lu, Mario (%.3f, %.3f, %.3f) velocity (%.3f, %.3f, %.3f), coins %d, star bits %d, "
                     "view %016llx, fovy %.3f\n",
                     when, gState.sceneFrames, pos->x, pos->y, pos->z, vel->x, vel->y, vel->z, static_cast< int >(MR::getCoinNum()),
                     static_cast< int >(MR::getStarPieceNum()), hash, MR::getFovy());
    }

    void setView() {
        const V right = normalized(cross(gState.forward, gState.up), V{1, 0, 0});
        const V up = cross(right, gState.forward);
        const V back = mul(gState.forward, -1.0f);
        // The view matrix: rows are the camera axes, translation -R * eye.
        TPos3f view;
        const V axes[3] = {right, up, back};
        for (int r = 0; r < 3; r++) {
            view.mMtx[r][0] = axes[r].x;
            view.mMtx[r][1] = axes[r].y;
            view.mMtx[r][2] = axes[r].z;
            view.mMtx[r][3] = -dot(axes[r], gState.eye);
        }
        const TVec3f eye(gState.eye.x, gState.eye.y, gState.eye.z);
        MR::setCameraViewMtx(view, false, false, eye);
        MR::setFovy(gState.fovy);
    }

    void requestScreenshot() {
        char directory[768];
        if (const char* dir = std::getenv("PETARI_PHOTO_DIR"); dir != nullptr && dir[0] != '\0') {
            std::snprintf(directory, sizeof(directory), "%s", dir);
        } else {
            const char* home = std::getenv("HOME");
            std::snprintf(directory, sizeof(directory), "%s/Pictures", home != nullptr ? home : ".");
            ::mkdir(directory, 0755);
            std::snprintf(directory, sizeof(directory), "%s/Pictures/Petari", home != nullptr ? home : ".");
        }
        ::mkdir(directory, 0755);
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/Petari-%04d%02d%02d-%02d%02d%02d-%d.png", directory, local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, ++gState.shots);
        if (PetariNative::Screenshot::request(path)) {
            std::fprintf(stderr, "[photo] screenshot requested: %s\n", path);
        }
    }
}  // namespace

namespace PhotoCamera {
    bool takeEnterRequest(bool canPause) {
        PetariPhotoInput in;
        petari_photo_take_input(&in);
        if (in.toggle == 0) {
            return false;
        }
        if (in.enabled && canPause) {
            return true;
        }
        petari_photo_set_active(0);  // not now (a demo, a talk, the pause menu...): input goes back to the game
        return false;
    }

    void start() {
        gState.active = true;
        gState.savedView = MR::getCameraViewMtx();
        gState.savedFovy = MR::getFovy();
        const TPos3f& inv = MR::getCameraInvViewMtx();
        gState.eye = {inv.mMtx[0][3], inv.mMtx[1][3], inv.mMtx[2][3]};
        gState.up = normalized({inv.mMtx[0][1], inv.mMtx[1][1], inv.mMtx[2][1]}, V{0, 1, 0});
        gState.forward = normalized({-inv.mMtx[0][2], -inv.mMtx[1][2], -inv.mMtx[2][2]}, V{0, 0, -1});
        gState.fovy = gState.savedFovy;
        AudWrap::getSystem()->enterPauseMenu();
        GameSystemFunction::onPauseBeginAllRumble();
        petari_photo_set_active(1);
        logGameState("enter");
    }

    bool update() {
        PetariPhotoInput in;
        petari_photo_take_input(&in);
        if (in.toggle > 0 || in.leave > 0 || !in.enabled) {
            return true;
        }
        const float speed = kSpeed * (in.fast ? 5.0f : 1.0f) * (in.slow ? 0.2f : 1.0f);
        // Look: yaw about the entry up, pitch about the camera's right, never past straight up or down.
        gState.forward = normalized(rotate(gState.forward, gState.up, -in.yaw * kDeg), gState.forward);
        const V right = normalized(cross(gState.forward, gState.up), V{1, 0, 0});
        const V pitched = normalized(rotate(gState.forward, right, in.pitch * kDeg), gState.forward);
        if (std::fabs(dot(pitched, gState.up)) < 0.995f) {
            gState.forward = pitched;
        }
        gState.eye = add(gState.eye, mul(right, in.moveRight * speed));
        gState.eye = add(gState.eye, mul(gState.forward, in.moveForward * speed));
        gState.eye = add(gState.eye, mul(gState.up, in.moveUp * speed));
        gState.fovy += in.fovSteps * 2.5f;
        gState.fovy = gState.fovy < 10.0f ? 10.0f : gState.fovy > 120.0f ? 120.0f : gState.fovy;
        setView();
        for (int i = 0; i < in.shots; i++) {
            requestScreenshot();
        }
        return false;
    }

    void end() {
        const TPos3f& inv = MR::getCameraInvViewMtx();
        const TVec3f eye(inv.mMtx[0][3], inv.mMtx[1][3], inv.mMtx[2][3]);
        MR::setCameraViewMtx(gState.savedView, false, false, eye);
        MR::setFovy(gState.savedFovy);
        GameSystemFunction::onPauseEndAllRumble();
        AudWrap::getSystem()->exitPauseMenu();
        gState.active = false;
        petari_photo_set_active(0);
        logGameState("exit");
    }

    bool isActive() {
        return gState.active;
    }

    void noteSceneFrame() {
        gState.sceneFrames++;
    }
}  // namespace PhotoCamera
#endif

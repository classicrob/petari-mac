#pragma once
#include <petari/input.hpp>
#include <cmath>
#include <cstdio>

namespace PetariNative::App {
struct RouteRecordFrame {
    const char* stage = "";
    int scenario = -1, zone = -1;
    bool valid = false, grounded = false, bound = false, dead = false;
    int powerStars = -1;  // the file's power star count (a rise marks a star), -1 outside the game
    float x = 0, y = 0, z = 0, gx = 0, gy = 0, gz = 0;
    float cx = 0, cy = 0, cz = 0, yaw = 0;
    Input::BoundState input;
};
class RouteRecordWriter {
public:
    ~RouteRecordWriter() { if (mFile) std::fclose(mFile); }
    bool open(const char* path, bool background) {
        if (mAttempted) return mFile != nullptr;
        mAttempted = true;
        if (background) {
            std::fputs("PETARI ROUTE RECORD: disabled in background mode\n", stderr);
            return false;
        }
        mFile = std::fopen(path, "wx");
        if (!mFile) { std::perror("PETARI ROUTE RECORD: create new CSV"); return false; }
        std::setvbuf(mFile, nullptr, _IOFBF, 65536);
        std::fputs("frame,stage,scenario,zone_ground_id,valid,x,y,z,gx,gy,gz,up_x,up_y,up_z,grounded,bound,move_x,move_y,jump,spin,crouch,camera_left,camera_right,camera_center,camera_up,camera_down,camera_yaw_world_y_rad,cam_z_x,cam_z_y,cam_z_z,power_stars,dead\n", mFile);
        std::fprintf(stderr, "PETARI ROUTE RECORD: writing %s (every frame)\n", path);
        return true;
    }
    void write(const RouteRecordFrame& r) {
        if (!mFile) return;
        std::fprintf(mFile, "%lu,\"", mFrame++);
        for (const char* p = r.stage ? r.stage : ""; *p; ++p) {
            if (*p == '"') std::fputc('"', mFile);
            std::fputc(*p, mFile);
        }
        const float norm = std::sqrt(r.gx*r.gx+r.gy*r.gy+r.gz*r.gz);
        const float scale = norm > 1e-6f ? -1.0f/norm : 0;
        const auto& i = r.input;
        std::fprintf(mFile, "\",%d,%d,%d,%.3f,%.3f,%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%.5f,%.5f,%d,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%d,%d\n",
            r.scenario,r.zone,r.valid,r.x,r.y,r.z,r.gx,r.gy,r.gz,r.gx*scale,r.gy*scale,r.gz*scale,
            r.grounded,r.bound,i.moveX,i.moveY,i.jump,i.spin,i.crouch,i.cameraLeft,i.cameraRight,
            i.cameraCenter,i.cameraUp,i.cameraDown,r.yaw,r.cx,r.cy,r.cz,r.powerStars,r.dead);
        if (mFrame % 60 == 0) std::fflush(mFile);
        if (std::ferror(mFile)) {
            std::perror("PETARI ROUTE RECORD: write failed");
            std::fclose(mFile); mFile = nullptr;
        }
    }
private:
    std::FILE* mFile = nullptr;
    bool mAttempted = false;
    unsigned long mFrame = 0;
};
}

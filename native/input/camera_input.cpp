// Odyssey-style orbit camera input (docs/dev/ODYSSEY_CAMERA.md): settings and
// the per-frame input the game's camera takes (petari/camera_input.h). The
// host input functions (wpad_host.cpp) feed it only while the mod is on.
#include "petari/camera_settings.hpp"
#include "petari/camera_input.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "petari/host_allocation.hpp"

namespace PetariNative::CameraSettings {
namespace {
std::atomic<bool> gEnabled{false};
std::atomic<int> gSpeed{3};
std::atomic<bool> gInvertX{false};
std::atomic<bool> gInvertY{false};
std::atomic<int> gScrollMode{0};

// Accumulated since the game's last take (host threads add, the game takes).
std::atomic<float> gStickX{0}, gStickY{0};
std::atomic<float> gMouseYaw{0}, gMousePitch{0};
std::atomic<float> gZoomSteps{0};
std::atomic<int> gYawHold{0}, gPitchHold{0}, gZoomHold{0};
std::atomic<bool> gRecenter{false};

void addFloat(std::atomic<float>& value, float delta) {
    float current = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(current, current + delta, std::memory_order_relaxed)) {
    }
}
}  // namespace

std::mutex gFileMutex;
std::filesystem::path gFile;

std::string serialize() {
    std::string text = "# Odyssey camera mod (docs/dev/ODYSSEY_CAMERA.md).\n";
    text += std::string("OdysseyCamera=") + (enabled() ? "on" : "off") + "\n";
    text += "Speed=" + std::to_string(speed()) + "\n";
    text += std::string("InvertX=") + (invertX() ? "on" : "off") + "\n";
    text += std::string("InvertY=") + (invertY() ? "on" : "off") + "\n";
    static const char* const kModes[] = {"auto", "orbit", "zoom"};
    text += std::string("ScrollMode=") + kModes[static_cast<int>(scrollMode())] + "\n";
    return text;
}

bool parse(const std::string& text, std::string* error) {
    bool on = enabled(), ix = invertX(), iy = invertY();
    int sp = speed();
    ScrollMode mode = scrollMode();
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        const auto eq = line.find('=');
        const std::string key = line.substr(start, eq == std::string::npos ? std::string::npos : eq - start);
        const std::string value = eq == std::string::npos ? "" : line.substr(eq + 1);
        const bool flag = value == "on";
        const bool isFlag = value == "on" || value == "off";
        if (key == "OdysseyCamera" && isFlag) on = flag;
        else if (key == "InvertX" && isFlag) ix = flag;
        else if (key == "InvertY" && isFlag) iy = flag;
        else if (key == "Speed" && value.size() == 1 && value[0] >= '1' && value[0] <= '5') sp = value[0] - '0';
        else if (key == "ScrollMode" && (value == "auto" || value == "orbit" || value == "zoom"))
            mode = value == "auto" ? ScrollMode::Auto : value == "orbit" ? ScrollMode::Orbit : ScrollMode::Zoom;
        else {
            *error = "line " + std::to_string(number) +
                     ": expected OdysseyCamera|InvertX|InvertY=on|off, Speed=1..5 or ScrollMode=auto|orbit|zoom";
            return false;
        }
    }
    setEnabled(on);
    setSpeed(sp);
    setInvertX(ix);
    setInvertY(iy);
    setScrollMode(mode);
    return true;
}

bool load(const std::filesystem::path& file, std::string* error) {
    HostAllocationScope scope;
    std::ifstream in(file);
    if (in) {
        std::stringstream text;
        text << in.rdbuf();
        if (!parse(text.str(), error)) {
            *error = file.string() + ": " + *error;
            return false;
        }
    }
    if (const char* env = std::getenv("PETARI_ODYSSEY_CAMERA"); env != nullptr && env[0] != '\0') {
        setEnabled(env[0] != '0');
    }
    std::lock_guard<std::mutex> lock(gFileMutex);
    gFile = file;
    return true;
}

bool save(std::string* error) {
    HostAllocationScope scope;
    std::lock_guard<std::mutex> lock(gFileMutex);
    if (gFile.empty()) return true;
    std::ofstream out(gFile, std::ios::trunc);
    out << serialize();
    if (!out) {
        *error = "cannot write " + gFile.string();
        return false;
    }
    return true;
}

bool enabled() { return gEnabled.load(std::memory_order_relaxed); }
void setEnabled(bool on) {
    gEnabled.store(on, std::memory_order_relaxed);
    if (!on) resetInput();
}
int speed() { return gSpeed.load(std::memory_order_relaxed); }
void setSpeed(int value) { gSpeed.store(value < 1 ? 1 : value > 5 ? 5 : value, std::memory_order_relaxed); }
bool invertX() { return gInvertX.load(std::memory_order_relaxed); }
void setInvertX(bool on) { gInvertX.store(on, std::memory_order_relaxed); }
bool invertY() { return gInvertY.load(std::memory_order_relaxed); }
ScrollMode scrollMode() { return static_cast<ScrollMode>(gScrollMode.load(std::memory_order_relaxed)); }
void setScrollMode(ScrollMode mode) { gScrollMode.store(static_cast<int>(mode), std::memory_order_relaxed); }
void setInvertY(bool on) { gInvertY.store(on, std::memory_order_relaxed); }

void stick(float x, float y) {
    // Radial dead zone, rescaled so the response starts at zero.
    constexpr float kDead = 0.2f;
    const float length = std::sqrt(x * x + y * y);
    if (length < kDead) {
        x = y = 0.0f;
    } else {
        const float scale = (std::fmin(length, 1.0f) - kDead) / (1.0f - kDead) / length;
        x *= scale;
        y *= scale;
    }
    gStickX.store(x, std::memory_order_relaxed);
    gStickY.store(y, std::memory_order_relaxed);
}
void mouseDrag(float dx, float dy) {
    constexpr float kDegreesPerPoint = 0.25f;
    addFloat(gMouseYaw, dx * kDegreesPerPoint);
    addFloat(gMousePitch, -dy * kDegreesPerPoint);  // drag up: camera rises
}
void zoomSteps(float steps) { addFloat(gZoomSteps, steps); }
void scrollOrbit(float x, float y) {
    constexpr float kDegreesPerUnit = 3.0f;
    addFloat(gMouseYaw, x * kDegreesPerUnit);
    addFloat(gMousePitch, y * kDegreesPerUnit);
}
void yawHold(int direction) { gYawHold.store(direction, std::memory_order_relaxed); }
void zoomHold(int direction) { gZoomHold.store(direction, std::memory_order_relaxed); }
void pitchHold(int direction) { gPitchHold.store(direction, std::memory_order_relaxed); }
void recenter() { gRecenter.store(true, std::memory_order_relaxed); }

void resetInput() {
    gStickX.store(0);
    gStickY.store(0);
    gMouseYaw.store(0);
    gMousePitch.store(0);
    gZoomSteps.store(0);
    gYawHold.store(0);
    gPitchHold.store(0);
    gZoomHold.store(0);
    gRecenter.store(false);
}

void resetForTesting() {
    {
        std::lock_guard<std::mutex> lock(gFileMutex);
        gFile.clear();
    }
    gEnabled.store(false);
    gSpeed.store(3);
    gInvertX.store(false);
    gInvertY.store(false);
    gScrollMode.store(0);
    resetInput();
}

}  // namespace PetariNative::CameraSettings

extern "C" void petari_camera_take_input(PetariCameraInput* out) {
    namespace S = PetariNative::CameraSettings;
    *out = PetariCameraInput{};
    out->enabled = S::enabled() ? 1 : 0;
    if (!out->enabled) return;
    out->stickX = S::gStickX.load(std::memory_order_relaxed);
    out->stickY = S::gStickY.load(std::memory_order_relaxed);
    out->mouseYaw = S::gMouseYaw.exchange(0.0f, std::memory_order_relaxed);
    out->mousePitch = S::gMousePitch.exchange(0.0f, std::memory_order_relaxed);
    out->zoomSteps = S::gZoomSteps.exchange(0.0f, std::memory_order_relaxed);
    out->yawHold = static_cast<float>(S::gYawHold.load(std::memory_order_relaxed));
    out->pitchHold = static_cast<float>(S::gPitchHold.load(std::memory_order_relaxed));
    out->zoomHold = static_cast<float>(S::gZoomHold.load(std::memory_order_relaxed));
    out->recenter = S::gRecenter.exchange(false, std::memory_order_relaxed) ? 1 : 0;
    out->speed = S::speed();
    out->invertX = S::invertX() ? 1 : 0;
    out->invertY = S::invertY() ? 1 : 0;
}

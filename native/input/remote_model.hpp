#pragma once
// The virtual Wii Remote and Nunchuk: host input state in, one 5 ms device
// report out. Pure and deterministic; the WPAD layer (wpad_host.cpp) owns
// locking and timing.

#include <array>
#include <cstdint>

#include "petari/input.hpp"

namespace PetariNative::Input::Detail {

constexpr double kReportSeconds = 0.005;  // 200 Hz, the Wii Remote report rate
constexpr int kCoreGravityCounts = 100;   // WPADGetAccGravityUnit, Wii Remote
constexpr int kNunchukGravityCounts = 200;  // WPADGetAccGravityUnit, Nunchuk
constexpr int kStickMin = 15;             // KPAD's Nunchuk stick dead zone (kp_fs_fstick_min)
constexpr int kStickMax = 71;             // KPAD's Nunchuk stick full scale (kp_fs_fstick_max)
constexpr float kUprightPitchDegrees = 80.0f;
// Above this pitch the IR camera no longer sees the sensor bar. KPAD itself
// rejects the pointer beyond acos(0.7) (kp_err_up_inpr).
constexpr float kSensorVisiblePitchDegrees = 40.0f;

// Output bits. 0..15 are WPAD button bits, including the Nunchuk's C and Z as
// they appear in WPADFSStatus::button. The rest are internal.
enum : std::uint32_t {
    kBitStickUp = 1u << 16,
    kBitStickDown = 1u << 17,
    kBitStickLeft = 1u << 18,
    kBitStickRight = 1u << 19,
    kBitShake = 1u << 20,
    kBitTiltHold = 1u << 21,
    kBitPostureToggle = 1u << 22,
    kBitCount = 23,
};
std::uint32_t actionBits(Action action);

// KPAD's pointer calibration, read from inside_kpads[chan] when a report is
// made (KPAD owns these; the game changes them with KPADSetSensorHeight).
struct PointerCalibration {
    float centerX = 0.0f;       // center_org
    float centerY = 0.0f;
    float dpdToPosScale = 1.0f;  // dpd2pos_scale
    float distanceFactor = 0.0f;  // dist_vv1: dist = distanceFactor / dot separation
};

struct Dot {
    std::int16_t x = 0;  // IR camera pixels, 0..1023
    std::int16_t y = 0;  // 0..767
};

struct Report {
    std::uint16_t buttons = 0;  // WPAD_BUTTON_* | Nunchuk C/Z
    float acc[3] = {0.0f, -1.0f, 0.0f};  // Wii Remote, KPAD axes, in g
    float nunchukAcc[3] = {0.0f, -1.0f, 0.0f};
    std::int8_t stickX = 0;  // raw Nunchuk stick, KPAD clamps 15..71
    std::int8_t stickY = 0;
    int dotCount = 0;
    Dot dots[2];
};

// Converts a KPAD pointer position (x right, y down, the screen spans -1..1)
// to the IR camera dots KPAD resolves to that position.
int pointerDots(const PointerCalibration& calibration, float posX, float posY, float rollRadians, float distance, Dot out[2]);

// Mouse position to KPAD pointer position through the viewport. False when
// the viewport is empty.
bool pointerPosition(const Viewport& viewport, float mouseX, float mouseY, float* posX, float* posY);

// Raw stick value for one axis of an analog value in -1..1, inverting KPAD's
// cross clamp.
std::int8_t rawStickAxis(float value);

class RemoteModel {
public:
    RemoteModel();

    void setBindings(const Bindings& bindings);
    const Bindings& bindings() const { return mBindings; }
    void setSettings(const Settings& settings) { mSettings = settings; }
    const Settings& settings() const { return mSettings; }

    void keyEvent(KeyCode code, bool down, bool repeat);
    void mouseButtonEvent(MouseButton button, bool down);
    void mouseMoved(float x, float y);
    void mouseLeft();
    void setViewport(const Viewport& viewport) { mViewport = viewport; }
    void focusChanged(bool focused);
    void setPosture(Posture posture) { mPosture = posture; }
    Posture posture() const { return mPosture; }

    // A bound input went down since the last call (reconnects a remote).
    bool takeActivity();

    Report nextReport(const PointerCalibration& calibration);

    // Current output (after minimum pulse lengths), for tests.
    std::uint32_t outputBits() const { return mOutput; }

private:
    std::uint32_t heldBits() const;
    void inputChanged(Binding input, bool down);
    void releaseAll();
    void updateOutput();
    void updateMotion(float stickX, float stickY);
    void stickVector(float* x, float* y) const;

    Bindings mBindings;
    Settings mSettings;
    std::array<bool, Key::Max> mKeys{};
    std::array<bool, static_cast<int>(MouseButton::Count)> mMouse{};
    bool mFocused = true;
    bool mMouseInWindow = false;
    float mMouseX = 0.0f;
    float mMouseY = 0.0f;
    Viewport mViewport;
    Posture mPosture = Posture::Pointing;
    bool mActivity = false;

    // Per output bit: held raw state, presses not yet reported, the reported
    // state, and how many reports it has lasted.
    std::uint32_t mRaw = 0;
    std::array<std::uint8_t, kBitCount> mPendingPresses{};
    std::array<std::uint32_t, kBitCount> mPressSequence{};
    std::uint32_t mSequence = 0;
    std::uint32_t mOutput = 0;
    std::array<int, kBitCount> mOutputAge{};

    // Motion.
    float mPitch = 0.0f;  // radians, tip up
    float mRoll = 0.0f;   // radians about the axis toward the screen, counterclockwise from behind
    int mShakeReport = -1;  // report index within the current shake, or -1
};

}  // namespace PetariNative::Input::Detail

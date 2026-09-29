#include "remote_model.hpp"

#include <algorithm>
#include <cmath>

namespace PetariNative::Input::Detail {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDiagonal = 0.70710678f;

float radians(float degrees) {
    return degrees * (kPi / 180.0f);
}

// Lateral acceleration of a shake, in g, added to gravity for report n of the
// burst. A 20 ms flick at 3 g, then a 200 ms return. The game filters the
// accelerometer (KPADSetAccParam 0, 0.15) and calls it a swing while the
// filtered value differs by 1 g from 20 reports earlier (WPadHVSwing). The
// flick crosses that within 3 reports; the slow return keeps every later
// 20-report difference below 1 g, so one flick is one swing.
constexpr int kShakeFlickReports = 4;
constexpr float kShakeFlickG = 3.0f;
constexpr float kShakeReturnStartG = 1.5f;
constexpr int kShakeReturnReports = 40;

float shakeAcceleration(int report) {
    if (report < kShakeFlickReports) {
        return kShakeFlickG;
    }
    const int n = report - kShakeFlickReports + 1;
    if (n >= kShakeReturnReports) {
        return 0.0f;
    }
    return kShakeReturnStartG * (1.0f - static_cast<float>(n) / kShakeReturnReports);
}

int shakeReports() {
    return kShakeFlickReports + kShakeReturnReports - 1;
}

float approach(float value, float target, float step) {
    if (value < target) {
        return std::min(value + step, target);
    }
    return std::max(value - step, target);
}

int bitIndex(std::uint32_t bit) {
    int index = 0;
    while ((bit >>= 1) != 0) {
        ++index;
    }
    return index;
}

}  // namespace

std::uint32_t actionBits(Action action) {
    switch (action) {
    case Action::StickUp: return kBitStickUp;
    case Action::StickDown: return kBitStickDown;
    case Action::StickLeft: return kBitStickLeft;
    case Action::StickRight: return kBitStickRight;
    case Action::A: return 0x0800;
    case Action::B: return 0x0400;
    case Action::Plus: return 0x0010;
    case Action::Minus: return 0x1000;
    case Action::Home: return 0x8000;
    case Action::One: return 0x0200;
    case Action::Two: return 0x0100;
    case Action::DpadUp: return 0x0008;
    case Action::DpadDown: return 0x0004;
    case Action::DpadLeft: return 0x0001;
    case Action::DpadRight: return 0x0002;
    case Action::NunchukC: return 0x4000;
    case Action::NunchukZ: return 0x2000;
    case Action::Shake: return kBitShake;
    case Action::TiltHold: return kBitTiltHold;
    case Action::PostureToggle: return kBitPostureToggle;
    case Action::Count: break;
    }
    return 0;
}

bool pointerPosition(const Viewport& viewport, float mouseX, float mouseY, float* posX, float* posY) {
    if (!(viewport.imageWidth > 0.0f) || !(viewport.imageHeight > 0.0f)) {
        return false;
    }
    *posX = (mouseX - viewport.imageX) / viewport.imageWidth * 2.0f - 1.0f;
    *posY = (mouseY - viewport.imageY) / viewport.imageHeight * 2.0f - 1.0f;
    return true;
}

// KPAD (calc_dpd_variable) takes the midpoint m of the two dots, rotates it
// by the negative camera roll, and reports pos = (center - R(-roll) m) * scale.
// The dots' separation gives dist = distanceFactor / separation. Inverted here.
int pointerDots(const PointerCalibration& calibration, float posX, float posY, float roll, float distance, Dot out[2]) {
    if (!(calibration.dpdToPosScale > 0.0f) || !(distance > 0.0f)) {
        return 0;
    }
    const float c = std::cos(roll);
    const float s = std::sin(roll);
    const float vx = calibration.centerX - posX / calibration.dpdToPosScale;
    const float vy = calibration.centerY - posY / calibration.dpdToPosScale;
    const float midX = c * vx - s * vy;
    const float midY = s * vx + c * vy;
    const float half = calibration.distanceFactor / distance * 0.5f;

    int count = 0;
    for (int side = -1; side <= 1; side += 2) {
        // KPAD get_kobj: center = pixel * 2/1024 - (resolution - 1)/1024.
        const float x = (midX + side * half * c) * 512.0f + 511.5f;
        const float y = (midY + side * half * s) * 512.0f + 383.5f;
        const long px = std::lround(x);
        const long py = std::lround(y);
        if (px < 0 || px > 1023 || py < 0 || py > 767) {
            continue;  // outside the IR camera image
        }
        out[count].x = static_cast<std::int16_t>(px);
        out[count].y = static_cast<std::int16_t>(py);
        ++count;
    }
    return count;
}

std::int8_t rawStickAxis(float value) {
    const float magnitude = std::min(std::fabs(value), 1.0f);
    if (magnitude <= 0.0f) {
        return 0;
    }
    const long raw = std::lround(kStickMin + magnitude * (kStickMax - kStickMin));
    return static_cast<std::int8_t>(value < 0.0f ? -raw : raw);
}

RemoteModel::RemoteModel() : mBindings(Bindings::defaults()) {
    mOutputAge.fill(1 << 20);
}

void RemoteModel::setBindings(const Bindings& bindings) {
    // Inputs held across a remap keep their physical state; only what they
    // drive changes.
    mBindings = bindings;
    const std::uint32_t before = mRaw;
    mRaw = heldBits();
    const std::uint32_t rising = mRaw & ~before;
    for (int bit = 0; bit < kBitCount; ++bit) {
        if ((rising >> bit) & 1u) {
            mPendingPresses[bit] = std::min<std::uint8_t>(mPendingPresses[bit] + 1, 3);
            mPressSequence[bit] = ++mSequence;
        }
    }
}

std::uint32_t RemoteModel::heldBits() const {
    if (!mFocused) {
        return 0;
    }
    std::uint32_t bits = 0;
    for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
        const Action action = static_cast<Action>(i);
        for (const Binding& input : mBindings.inputs(action)) {
            const bool held = input.device == Binding::Device::Key
                                  ? input.code < Key::Max && mKeys[input.code]
                                  : input.code < mMouse.size() && mMouse[input.code];
            if (held) {
                bits |= actionBits(action);
                break;
            }
        }
    }
    return bits;
}

void RemoteModel::inputChanged(Binding input, bool down) {
    if (input.device == Binding::Device::Key) {
        mKeys[input.code] = down;
    } else {
        mMouse[input.code] = down;
    }
    const std::uint32_t before = mRaw;
    mRaw = heldBits();
    const std::uint32_t rising = mRaw & ~before;
    if (rising != 0) {
        mActivity = true;
    }
    for (int bit = 0; bit < kBitCount; ++bit) {
        if ((rising >> bit) & 1u) {
            mPendingPresses[bit] = std::min<std::uint8_t>(mPendingPresses[bit] + 1, 3);
            mPressSequence[bit] = ++mSequence;
        }
    }
}

void RemoteModel::keyEvent(KeyCode code, bool down, bool repeat) {
    if (code >= Key::Max || repeat || mKeys[code] == down || (down && !mFocused)) {
        return;
    }
    inputChanged(Binding::key(code), down);
}

void RemoteModel::mouseButtonEvent(MouseButton button, bool down) {
    const int index = static_cast<int>(button);
    if (index < 0 || index >= static_cast<int>(mMouse.size()) || mMouse[index] == down || (down && !mFocused)) {
        return;
    }
    inputChanged(Binding::mouse(button), down);
}

void RemoteModel::mouseMoved(float x, float y) {
    mMouseX = x;
    mMouseY = y;
    mMouseInWindow = true;
}

void RemoteModel::mouseLeft() {
    mMouseInWindow = false;
}

void RemoteModel::releaseAll() {
    mKeys.fill(false);
    mMouse.fill(false);
    mPendingPresses.fill(0);
    mRaw = 0;
    mMouseInWindow = false;
    mShakeReport = -1;
}

void RemoteModel::focusChanged(bool focused) {
    if (!focused) {
        releaseAll();
    }
    mFocused = focused;
}

bool RemoteModel::takeActivity() {
    const bool activity = mActivity;
    mActivity = false;
    return activity;
}

void RemoteModel::updateOutput() {
    const int minimum = std::max(mSettings.minimumPulseReports, 1);
    for (int bit = 0; bit < kBitCount; ++bit) {
        const std::uint32_t mask = 1u << bit;
        int& age = mOutputAge[bit];
        if ((mOutput & mask) != 0) {
            if ((mRaw & mask) == 0 && age >= minimum) {
                mOutput &= ~mask;
                age = 0;
            }
        } else if (((mRaw & mask) != 0 || mPendingPresses[bit] > 0) && age >= minimum) {
            mOutput |= mask;
            age = 0;
            if (mPendingPresses[bit] > 0) {
                --mPendingPresses[bit];
            }
        }
        if (age < (1 << 20)) {
            ++age;
        }
    }
}

void RemoteModel::stickVector(float* x, float* y) const {
    auto axis = [this](std::uint32_t positive, std::uint32_t negative) {
        const bool p = (mOutput & positive) != 0;
        const bool n = (mOutput & negative) != 0;
        if (p && n) {
            // Last pressed wins.
            return mPressSequence[bitIndex(positive)] > mPressSequence[bitIndex(negative)] ? 1.0f : -1.0f;
        }
        return p ? 1.0f : n ? -1.0f : 0.0f;
    };
    *x = axis(kBitStickRight, kBitStickLeft);
    *y = axis(kBitStickUp, kBitStickDown);
    if (*x != 0.0f && *y != 0.0f) {
        *x *= kDiagonal;
        *y *= kDiagonal;
    }
}

void RemoteModel::updateMotion(float stickX, float stickY) {
    const float base = mPosture == Posture::Upright ? radians(kUprightPitchDegrees) : 0.0f;
    float targetPitch = base;
    float targetRoll = 0.0f;
    if ((mOutput & kBitTiltHold) != 0) {
        // Forward tilts the tip toward the screen; right turns the remote
        // clockwise as seen from behind.
        const float range = radians(mSettings.maxTiltDegrees);
        targetPitch = base - stickY * range;
        targetRoll = -stickX * range;
    }
    const float step = radians(mSettings.tiltRateDegreesPerSecond) * static_cast<float>(kReportSeconds);
    mPitch = approach(mPitch, targetPitch, step);
    mRoll = approach(mRoll, targetRoll, step);
}

Report RemoteModel::nextReport(const PointerCalibration& calibration) {
    const std::uint32_t previous = mOutput;
    updateOutput();
    const std::uint32_t rising = mOutput & ~previous;
    if ((rising & kBitPostureToggle) != 0) {
        mPosture = mPosture == Posture::Pointing ? Posture::Upright : Posture::Pointing;
    }
    if ((rising & kBitShake) != 0) {
        mShakeReport = 0;
    }

    float stickX;
    float stickY;
    stickVector(&stickX, &stickY);
    updateMotion(stickX, stickY);

    Report report;
    report.buttons = static_cast<std::uint16_t>(mOutput & 0xFFFFu);
    if ((mOutput & kBitTiltHold) == 0) {
        report.stickX = rawStickAxis(stickX);
        report.stickY = rawStickAxis(stickY);
    }

    // Accelerometer: the world's up direction in KPAD axes (x across the
    // remote, y out of its underside, z toward its tail).
    const float cp = std::cos(mPitch);
    const float sp = std::sin(mPitch);
    const float cr = std::cos(mRoll);
    const float sr = std::sin(mRoll);
    report.acc[0] = sr;
    report.acc[1] = -cp * cr;
    report.acc[2] = -sp * cr;
    if (mShakeReport >= 0) {
        report.acc[0] += shakeAcceleration(mShakeReport);
        if (++mShakeReport >= shakeReports()) {
            mShakeReport = -1;
        }
    }

    // IR camera. The camera's roll is the remote's roll about the pointing
    // axis, which KPAD also derives from gravity (acc_horizon).
    float posX;
    float posY;
    if (mFocused && mMouseInWindow && std::fabs(mPitch) < radians(kSensorVisiblePitchDegrees) &&
        pointerPosition(mViewport, mMouseX, mMouseY, &posX, &posY)) {
        const float cameraRoll = std::atan2(sr, cp * cr);
        report.dotCount = pointerDots(calibration, posX, posY, cameraRoll, mSettings.pointerDistance, report.dots);
    }
    return report;
}

}  // namespace PetariNative::Input::Detail

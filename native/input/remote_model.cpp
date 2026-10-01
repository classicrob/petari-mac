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

// Shakes start at least 250 ms apart, after the previous flick has fully
// returned (43 reports). A flick that starts while the previous one is still
// returning keeps the filtered value high, and the game sees one long swing.
constexpr std::uint64_t kShakeSpacingReports = 50;
static_assert(kShakeSpacingReports >= kShakeFlickReports + kShakeReturnReports - 1, "flicks must not overlap");

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
    case Action::Walk: return kBitWalk;
    case Action::Start: return 0x0800;  // plus B while the title prompt is up (RemoteModel::heldBits)
    case Action::ModCollectStarBits:
    case Action::ModShootEnemy:
    case Action::CameraOrbitHold:
    case Action::CameraZoomIn:
    case Action::CameraZoomOut:
    case Action::CameraOrbitLeft:
    case Action::CameraOrbitRight:
    case Action::CameraPitchUp:
    case Action::CameraPitchDown: return 0;  // not remote buttons (mods, camera_settings)
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
    refreshRaw();
}

std::uint32_t RemoteModel::heldBits() const {
    if (!mFocused) {
        return 0;
    }
    std::uint32_t bits = 0;
    for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
        const Action action = static_cast<Action>(i);
        for (const Binding& input : mBindings.inputs(action)) {
            bool held = false;
            switch (input.device) {
            case Binding::Device::Key: held = input.code < Key::Max && mKeys[input.code]; break;
            case Binding::Device::Mouse: held = input.code < mMouse.size() && mMouse[input.code]; break;
            case Binding::Device::Pad: held = input.code < mPad.size() && mPad[input.code]; break;
            }
            if (held) {
                bits |= actionBits(action);
                if (action == Action::Start && titlePromptActive()) {
                    bits |= actionBits(Action::B);
                }
                break;
            }
        }
    }
    return bits;
}

std::uint32_t RemoteModel::refreshRaw() {
    const std::uint32_t before = mRaw;
    mRaw = heldBits();
    const std::uint32_t rising = mRaw & ~before;
    for (int bit = 0; bit < kBitCount; ++bit) {
        if ((rising >> bit) & 1u) {
            mPendingPresses[bit] = std::min<std::uint8_t>(mPendingPresses[bit] + 1, 3);
            mPressSequence[bit] = ++mSequence;
        }
    }
    return rising;
}

void RemoteModel::inputChanged(Binding input, bool down) {
    switch (input.device) {
    case Binding::Device::Key: mKeys[input.code] = down; break;
    case Binding::Device::Mouse: mMouse[input.code] = down; break;
    case Binding::Device::Pad: mPad[input.code] = down; break;
    }
    if (refreshRaw() != 0) {
        mActivity = true;
    }
}

bool RemoteModel::titlePromptActive() const {
    return mTitlePromptReport != 0 && mReportIndex <= mTitlePromptReport + kGameHintReports;
}

bool RemoteModel::motionControlActive() const {
    return mMotionReport != 0 && mReportIndex <= mMotionReport + kGameHintReports;
}

void RemoteModel::titlePromptShown() {
    // Stamped with the next report, so a stamp before any report still counts.
    mTitlePromptReport = mReportIndex + 1;
    refreshRaw();
}

void RemoteModel::keyEvent(KeyCode code, bool down, bool repeat) {
    if (code >= Key::Max || repeat || mKeys[code] == down || (down && !mFocused)) {
        return;
    }
    // Command shortcuts belong to macOS (Cmd+Q, Ctrl+Cmd+F fullscreen,
    // Cmd+Tab): a key pressed while Command is held does not reach the game,
    // so quitting does not turn the camera and fullscreen does not spin.
    // Releases always pass, so nothing stays held.
    const bool modifier = code >= Key::LeftCtrl && code <= Key::RightGui;
    if (down && !modifier && (mKeys[Key::LeftGui] || mKeys[Key::RightGui])) {
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

void RemoteModel::padButtonEvent(PadButton button, bool down) {
    const int index = static_cast<int>(button);
    if (index < 0 || index >= static_cast<int>(mPad.size()) || mPad[index] == down || (down && !mFocused)) {
        return;
    }
    if (button == PadButton::RightStick && down) {
        // R3 brings the controller's Star Pointer to the middle of the image.
        mPadPointer = true;
        mPadPointerX = 0.0f;
        mPadPointerY = 0.0f;
    }
    inputChanged(Binding::pad(button), down);
}

void RemoteModel::padAxisEvent(PadAxis axis, float value) {
    const int index = static_cast<int>(axis);
    if (index < 0 || index >= static_cast<int>(mPadAxes.size()) || !(value == value)) {
        return;
    }
    const bool trigger = axis == PadAxis::LeftTrigger || axis == PadAxis::RightTrigger;
    value = trigger ? std::clamp(value, 0.0f, 1.0f) : std::clamp(value, -1.0f, 1.0f);
    if (!mFocused) {
        return;
    }
    mPadAxes[index] = value;
    if (trigger) {
        // A button past half travel, with hysteresis so a resting finger does
        // not chatter.
        const PadButton button = axis == PadAxis::LeftTrigger ? PadButton::LeftTrigger : PadButton::RightTrigger;
        const bool held = mPad[static_cast<int>(button)];
        if (!held && value >= 0.55f) {
            padButtonEvent(button, true);
        } else if (held && value <= 0.45f) {
            padButtonEvent(button, false);
        }
    }
}

void RemoteModel::padDisconnected() {
    for (int i = 0; i < static_cast<int>(PadButton::Count); ++i) {
        if (mPad[i]) {
            inputChanged(Binding::pad(static_cast<PadButton>(i)), false);
        }
    }
    mPadAxes.fill(0.0f);
    mPadPointer = false;
}

bool RemoteModel::padStick(PadAxis xAxis, PadAxis yAxis, float* x, float* y) const {
    const float px = mPadAxes[static_cast<int>(xAxis)];
    const float py = -mPadAxes[static_cast<int>(yAxis)];  // SDL: y down
    const float length = std::sqrt(px * px + py * py);
    const float dead = std::clamp(mSettings.padStickDeadZone, 0.0f, 0.95f);
    if (!(length > dead)) {
        *x = *y = 0.0f;
        return false;
    }
    const float scaled = std::min((length - dead) / (1.0f - dead), 1.0f);
    *x = px / length * scaled;
    *y = py / length * scaled;
    return true;
}

void RemoteModel::updatePadPointer() {
    float x;
    float y;
    if (!mFocused || !padStick(PadAxis::RightX, PadAxis::RightY, &x, &y)) {
        return;
    }
    if (!mPadPointer) {
        // Take over from wherever the mouse left the pointer.
        float posX = 0.0f;
        float posY = 0.0f;
        if (mMouseInWindow && pointerPosition(mViewport, mMouseX, mMouseY, &posX, &posY)) {
            mPadPointerX = std::clamp(posX, -1.0f, 1.0f);
            mPadPointerY = std::clamp(posY, -1.0f, 1.0f);
        } else {
            mPadPointerX = mPadPointerY = 0.0f;
        }
        mPadPointer = true;
    }
    // A squared response: fine aiming near the centre, full speed at the rim.
    const float speed = std::max(mSettings.padPointerWidthsPerSecond, 0.0f) * 2.0f * static_cast<float>(kReportSeconds);
    const float length = std::sqrt(x * x + y * y);
    mPadPointerX = std::clamp(mPadPointerX + x * length * speed, -1.0f, 1.0f);
    mPadPointerY = std::clamp(mPadPointerY - y * length * speed, -1.0f, 1.0f);  // KPAD: y down
}

void RemoteModel::mouseMoved(float x, float y) {
    mMouseX = x;
    mMouseY = y;
    mMouseInWindow = true;
    mPadPointer = false;  // the mouse takes the pointer back
}

void RemoteModel::mouseLeft() {
    mMouseInWindow = false;
}

void RemoteModel::releaseAll() {
    mKeys.fill(false);
    mMouse.fill(false);
    mPad.fill(false);
    mPadAxes.fill(0.0f);
    mPadPointer = false;
    mPendingPresses.fill(0);
    mRaw = 0;
    mMouseInWindow = false;
    // A flick in progress finishes its slow return: cutting it off would drop
    // the acceleration by over 1 g, which the game reads as another swing.
    // A flick still waiting is cancelled.
    mShakeStart = 0;
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
    const int pauseMinimum = std::max(mSettings.pauseTapReports, minimum);
    constexpr std::uint32_t kPauseButtons = 0x0010 | 0x1000;  // Plus, Minus
    for (int bit = 0; bit < kBitCount; ++bit) {
        const std::uint32_t mask = 1u << bit;
        int& age = mOutputAge[bit];
        if ((mOutput & mask) != 0) {
            const int held = (mask & kPauseButtons) != 0 ? pauseMinimum : minimum;
            if ((mRaw & mask) == 0 && age >= held) {
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
    if (*x == 0.0f && *y == 0.0f && mFocused) {
        // No stick keys: a controller's left stick, analog.
        padStick(PadAxis::LeftX, PadAxis::LeftY, x, y);
    }
    if ((mOutput & kBitWalk) != 0) {
        const float scale = std::clamp(mSettings.walkStickScale, 0.0f, 1.0f);
        *x *= scale;
        *y *= scale;
    }
}

void RemoteModel::updateMotion(float stickX, float stickY) {
    const bool steering = motionControlActive();
    bool upright = mPosture == Posture::Upright;
    if (steering) {
        // The rides' tutorials check the posture: the Star Ball wants the
        // remote raised (TamakoroTutorial), the Ray level (SurfRayTutorial's
        // "straight"), whatever T last chose. Forward on the Ray would break
        // "straight", so there only the twist follows the keys.
        upright = mSteering == Steering::Ball;
        if (mSteering == Steering::Ray) {
            stickY = 0.0f;
        }
    }
    const float base = upright ? radians(kUprightPitchDegrees) : 0.0f;
    float targetPitch = base;
    float targetRoll = 0.0f;
    if ((mOutput & kBitTiltHold) != 0 || steering) {
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
    ++mReportIndex;
    refreshRaw();  // the title prompt may have lapsed
    const std::uint32_t previous = mOutput;
    updateOutput();
    const std::uint32_t rising = mOutput & ~previous;
    if ((rising & kBitPostureToggle) != 0) {
        mPosture = mPosture == Posture::Pointing ? Posture::Upright : Posture::Pointing;
    }
    constexpr std::uint32_t kButtonAorB = 0x0800 | 0x0400;
    if ((rising & kButtonAorB) != 0) {
        mLastButtonReport = mReportIndex;
    }
    if ((rising & kBitShake) != 0 && mShakeStart == 0) {
        // At most one shake waits: presses while one is pending are dropped.
        // It starts after Mario's post-A/B swing lockout (the delay is fixed
        // at the press, so later presses do not extend it) and after the
        // previous flick has returned.
        const int delay = std::max(mSettings.shakeDelayAfterButtonReports, 0);
        std::uint64_t start = mReportIndex;
        if (mLastButtonReport != 0) {
            start = std::max(start, mLastButtonReport + delay);
        }
        if (mLastShakeStart != 0) {
            start = std::max(start, mLastShakeStart + kShakeSpacingReports);
        }
        mShakeStart = start;
    }
    if (mShakeStart != 0 && mReportIndex >= mShakeStart) {
        mShakeStart = 0;
        mShakeReport = 0;
        mLastShakeStart = mReportIndex;
    }

    float stickX;
    float stickY;
    stickVector(&stickX, &stickY);
    updateMotion(stickX, stickY);

    Report report;
    report.buttons = static_cast<std::uint16_t>(mOutput & 0xFFFFu);
    if ((mOutput & kBitTiltHold) == 0 && !motionControlActive()) {
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
    updatePadPointer();
    float posX = mPadPointerX;
    float posY = mPadPointerY;
    const bool pointing = mPadPointer || (mMouseInWindow && pointerPosition(mViewport, mMouseX, mMouseY, &posX, &posY));
    if (mFocused && pointing && std::fabs(mPitch) < radians(kSensorVisiblePitchDegrees)) {
        const float cameraRoll = std::atan2(sr, cp * cr);
        report.dotCount = pointerDots(calibration, posX, posY, cameraRoll, mSettings.pointerDistance, report.dots);
    }
    return report;
}

}  // namespace PetariNative::Input::Detail

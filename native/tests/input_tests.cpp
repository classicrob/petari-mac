// Tests for native keyboard and mouse input: the emulated WPAD layer, the
// SDK's KPAD.c on top of it, and the game's own Wii Remote classes (WPad,
// WPadButton, WPadStick, WPadPointer, WPadAcceleration, WPadHVSwing) reading
// the result, so the checks are on what gameplay sees.
//
// Reports are pumped by hand (Clock::Manual): 10 reports per 3 frames, the
// Wii's 200 Hz reports against a 60 Hz game loop.

#include <revolution/kpad.h>
#include <revolution/os.h>
#include <revolution/wpad.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "Game/System/WPad.hpp"
#include "Game/System/WPadAcceleration.hpp"
#include "Game/System/WPadButton.hpp"
#include "Game/System/WPadHVSwing.hpp"
#include "Game/System/WPadHolder.hpp"
#include "Game/System/WPadPointer.hpp"
#include "Game/System/WPadStick.hpp"
#include "petari/input.hpp"
#include "petari/platform/sc.hpp"
#include "../input/remote_model.hpp"

#ifdef PETARI_INPUT_TEST_SDL3
#include "petari/input_sdl3.hpp"
#endif

extern "C" void __OSThreadInit(void);

namespace In = PetariNative::Input;
namespace PSC = PetariNative::Platform::SC;
using In::Key::Max;

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

bool aborts(const std::function<void()>& fn) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::freopen("/dev/null", "w", stderr);
        fn();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

std::vector<std::string> gCallbacks;
void connectCallback(s32 chan, s32 reason) {
    gCallbacks.push_back("connect" + std::to_string(chan) + ":" + std::to_string(reason));
}
void extensionCallback(s32 chan, s32 type) {
    gCallbacks.push_back("extension" + std::to_string(chan) + ":" + std::to_string(type));
}

// One game: KPAD, the game's WPad for channel 0, frame stepping.
struct Rig {
    WPadReadDataInfo* info;
    WPad* pad;
    long frames = 0;
    long reports = 0;

    explicit Rig(bool connect = true) {
        In::resetForTesting();
        PSC::reset();
        In::setClock(In::Clock::Manual);
        gCallbacks.clear();
        // As WPadHolder's constructor: allocator, KPADInit, pads, callbacks,
        // sensor bar level.
        WPADRegisterAllocator(nullptr, nullptr);
        KPADInit();
        info = new WPadReadDataInfo[WPAD_MAX_CONTROLLERS];
        pad = new WPad(0);
        pad->setReadInfo(&info[0]);
        for (s32 chan = 0; chan < WPAD_MAX_CONTROLLERS; ++chan) {
            WPADSetConnectCallback(chan, connectCallback);
            WPADSetExtensionCallback(chan, extensionCallback);
        }
        pad->mPointer->setSensorBarLevel(-0.15f);
        In::setViewport(In::Viewport::letterbox(1280.0f, 720.0f, 16.0f / 9.0f));
        if (connect) {
            for (int i = 0; i < 30; ++i) {
                frame();
            }
            check(WPADProbe(0, nullptr) == WPAD_ERR_NONE, "remote connected by the rig");
        }
    }

    // One 60 Hz frame: the reports produced meanwhile, then the game's
    // WPadHolder::update order (KPADRead every channel, then WPad::update).
    s32 frame() {
        const long target = (frames + 1) * 10 / 3;
        In::pumpReports(static_cast<int>(target - reports));
        reports = target;
        ++frames;
        for (s32 chan = 0; chan < WPAD_MAX_CONTROLLERS; ++chan) {
            info[chan].mValidStatusCount = KPADRead(chan, info[chan].mStatusArray, 120);
        }
        pad->update();
        return static_cast<s32>(info[0].mValidStatusCount);
    }

    void frames_(int count) {
        for (int i = 0; i < count; ++i) {
            frame();
        }
    }

    const KPADStatus& latest() const { return info[0].mStatusArray[0]; }
    u32 hold() const { return latest().hold & KPAD_BUTTON_MASK; }
    u32 trig() const { return latest().trig; }
    u32 release() const { return latest().release; }
};

void press(In::KeyCode key) {
    In::keyEvent(key, true, false);
}
void lift(In::KeyCode key) {
    In::keyEvent(key, false, false);
}

// --- Bindings -------------------------------------------------------------

bool hasBinding(const In::Bindings& b, In::Action action, In::Binding input) {
    for (const In::Binding& bound : b.inputs(action)) {
        if (bound == input) {
            return true;
        }
    }
    return false;
}

void testBindings() {
    using In::Action;
    using In::Binding;
    const In::Bindings d = In::Bindings::defaults();
    // native/CONTROLS.md
    check(hasBinding(d, Action::StickUp, Binding::key(In::Key::W)), "W moves forward");
    check(hasBinding(d, Action::StickLeft, Binding::key(In::Key::A)), "A moves left");
    check(hasBinding(d, Action::StickDown, Binding::key(In::Key::S)), "S moves back");
    check(hasBinding(d, Action::StickRight, Binding::key(In::Key::D)), "D moves right");
    check(hasBinding(d, Action::A, Binding::key(In::Key::Space)), "Space is A (jump, confirm)");
    check(hasBinding(d, Action::Shake, Binding::key(In::Key::F)), "F shakes (spin)");
    check(hasBinding(d, Action::NunchukZ, Binding::key(In::Key::LeftShift)), "Shift is Z (crouch, ground pound)");
    check(hasBinding(d, Action::B, Binding::mouse(In::MouseButton::Left)), "left mouse is B (Star Bits)");
    check(hasBinding(d, Action::A, Binding::mouse(In::MouseButton::Right)), "right mouse is A (pointer grab)");
    check(hasBinding(d, Action::DpadLeft, Binding::key(In::Key::Q)), "Q rotates the camera left");
    check(hasBinding(d, Action::DpadRight, Binding::key(In::Key::E)), "E rotates the camera right");
    check(hasBinding(d, Action::NunchukC, Binding::key(In::Key::C)), "C recenters the camera");
    check(hasBinding(d, Action::Plus, Binding::key(In::Key::Escape)), "Escape pauses");
    check(hasBinding(d, Action::B, Binding::key(In::Key::Escape)), "Escape backs out of menus");

    const std::string text = d.serialize();
    In::Bindings parsed;
    std::string error;
    check(parsed.parse(text, &error), "defaults parse: " + error);
    check(parsed.serialize() == text, "bindings round-trip through text");
    check(text.find("A=Key:Space,Mouse:Right\n") != std::string::npos, "text form names keys and mouse buttons");

    In::Bindings edited = d;
    check(edited.parse("# comment\nA=Key:J, Mouse:Middle\nShake=\n", &error), "partial remap parses");
    check(hasBinding(edited, Action::A, Binding::key(In::Key::J)) && !hasBinding(edited, Action::A, Binding::key(In::Key::Space)),
          "remap replaces an action's inputs");
    check(edited.inputs(Action::Shake).empty(), "an action can be left unbound");
    check(hasBinding(edited, Action::B, Binding::mouse(In::MouseButton::Left)), "actions not mentioned keep bindings");
    const std::string before = edited.serialize();
    check(!edited.parse("A=Key:NoSuchKey\n", &error) && error.find("NoSuchKey") != std::string::npos, "unknown key is an error");
    check(!edited.parse("Jump=Key:Space\n", &error), "unknown action is an error");
    check(edited.serialize() == before, "a failed parse changes nothing");
    In::KeyCode code = 0;
    check(In::parseKeyName("Usage100", &code) && code == 100 && In::keyName(100) == "Usage100", "unnamed usages round-trip");
}

// --- Connection -----------------------------------------------------------

void testConnection() {
    In::resetForTesting();
    check(WPADGetStatus() == WPAD_STATE_DISABLED, "WPAD disabled before WPADInit");
    check(aborts([] { In::pumpReports(1); }), "pumping before WPADInit aborts");

    Rig rig(false);
    check(WPADGetStatus() == WPAD_STATE_SETUP, "KPADInit starts WPAD");
    check(WPADGetWorkMemorySize() == 0, "no Bluetooth work memory natively");
    check(aborts([] { In::setClock(In::Clock::Alarm); }), "changing the clock after WPADInit aborts");
    check(aborts([] { WPADProbe(4, nullptr); }), "channel out of range aborts");
    u32 type = 0;
    check(WPADProbe(0, &type) == WPAD_ERR_NO_CONTROLLER && type == WPAD_DEV_NOT_FOUND, "not connected inside WPADInit");
    check(rig.frame() == 0, "KPADRead returns nothing before the remote connects");

    int connectFrame = -1;
    for (int f = 1; f < 30 && connectFrame < 0; ++f) {
        rig.frame();
        if (WPADProbe(0, nullptr) == WPAD_ERR_NONE) {
            connectFrame = f;
        }
    }
    check(connectFrame > 0 && rig.reports >= 40 && rig.reports <= 44, "connects after the 200 ms reconnection time");
    rig.frames_(5);
    check(gCallbacks.size() == 2 && gCallbacks[0] == "connect0:0" && gCallbacks[1] == "extension0:1",
          "connect callback, then the Nunchuk extension callback");
    check(WPADProbe(0, &type) == WPAD_ERR_NONE && type == WPAD_DEV_FREESTYLE, "probe reports a Nunchuk");
    for (s32 chan = 1; chan < WPAD_MAX_CONTROLLERS; ++chan) {
        check(WPADProbe(chan, nullptr) == WPAD_ERR_NO_CONTROLLER && rig.info[chan].mValidStatusCount == 0,
              "channels 1-3 have no remote");
    }

    // KPAD negotiated the IR camera and report format for a Nunchuk.
    check(WPADIsDpdEnabled(0) && WPADGetDataFormat(0) == WPAD_FMT_FREESTYLE_ACC_DPD, "KPAD enabled DPD and the Nunchuk format");
    const s32 count = rig.frame();
    check(count == 3 || count == 4, "3 or 4 reports per 60 Hz frame");
    const KPADStatus& s = rig.latest();
    check(s.wpad_err == WPAD_ERR_NONE && s.dev_type == WPAD_DEV_FREESTYLE && s.data_format == WPAD_FMT_FREESTYLE_ACC_DPD,
          "KPADStatus identifies a Wii Remote with Nunchuk");
    check(near(s.acc.x, 0.0f, 0.02f) && near(s.acc.y, -1.0f, 0.02f) && near(s.acc.z, 0.0f, 0.02f) && near(s.acc_value, 1.0f, 0.02f),
          "remote at rest, level: acc (0, -1, 0)");
    check(near(s.ex_status.fs.acc.y, -1.0f, 0.02f) && near(s.ex_status.fs.acc_value, 1.0f, 0.02f), "Nunchuk at rest");
    check(near(s.acc_vertical.x, 1.0f, 0.01f) && near(s.horizon.x, 1.0f, 0.01f), "level remote: vertical and horizon (1, 0)");
    check(s.hold == 0 && s.ex_status.fs.stick.x == 0.0f && s.ex_status.fs.stick.y == 0.0f, "nothing pressed");
    check(rig.pad->mIsSubPadConnected == false, "game connection state comes from its own callbacks, not registered here");

    // WPADDisconnect turns the remote off; pressing a bound key turns it on.
    gCallbacks.clear();
    WPADDisconnect(0);
    rig.frame();
    check(WPADProbe(0, nullptr) == WPAD_ERR_NO_CONTROLLER && gCallbacks.size() == 1 && gCallbacks[0] == "connect0:-1",
          "WPADDisconnect reports the remote gone");
    check(rig.frame() == 0, "no reports while disconnected");
    rig.frames_(30);
    check(WPADProbe(0, nullptr) == WPAD_ERR_NO_CONTROLLER, "stays off without input");
    press(In::Key::Space);
    lift(In::Key::Space);
    rig.frames_(20);
    check(WPADProbe(0, nullptr) == WPAD_ERR_NONE && gCallbacks.back() == "extension0:1", "a key press reconnects it");

    In::setNunchukAttached(0, false);
    rig.frames_(3);
    check(gCallbacks.back() == "extension0:0" && WPADGetDataFormat(0) == WPAD_FMT_CORE_ACC_DPD, "Nunchuk removed: core format");
    press(In::Key::LeftShift);
    rig.frames_(2);
    check((rig.hold() & WPAD_BUTTON_Z) == 0, "no Z without a Nunchuk");
    lift(In::Key::LeftShift);
    In::setNunchukAttached(0, true);
    rig.frames_(3);
    check(gCallbacks.back() == "extension0:1", "Nunchuk reattached");

    In::setConnected(1, true);
    rig.frames_(14);
    check(WPADProbe(1, &type) == WPAD_ERR_NONE && rig.info[1].mValidStatusCount > 0, "a second remote can connect");
    In::setConnected(1, false);
    rig.frame();
    check(WPADProbe(1, nullptr) == WPAD_ERR_NO_CONTROLLER, "and disconnect");
}

// --- Buttons --------------------------------------------------------------

void testButtons() {
    Rig rig;
    WPadButton& button = *rig.pad->mButton;

    press(In::Key::Space);
    rig.frame();
    check((rig.hold() & WPAD_BUTTON_A) && (rig.trig() & WPAD_BUTTON_A), "Space: A held and triggered");
    check(button.testTriggerA() && button.testButtonA(), "game sees the A trigger");
    check((rig.latest().hold & KPAD_BUTTON_RPT) != 0, "KPAD marks a new press as a repeat pulse");
    In::keyEvent(In::Key::Space, true, true);  // OS auto-repeat
    rig.frame();
    check(!button.testTriggerA() && button.testButtonA() && rig.trig() == 0, "trigger lasts one frame; OS repeats ignored");

    // KPAD's repeat: the game asks for 1/2.4 s delay, 1/6 s pulse.
    int firstRepeat = -1;
    int secondRepeat = -1;
    for (int f = 2; f < 60; ++f) {
        rig.frame();
        if ((rig.latest().hold & KPAD_BUTTON_RPT) != 0) {
            if (firstRepeat < 0) {
                firstRepeat = f;
            } else if (secondRepeat < 0) {
                secondRepeat = f;
            }
        }
    }
    check(firstRepeat >= 24 && firstRepeat <= 26, "first KPAD repeat after 5/12 s (" + std::to_string(firstRepeat) + ")");
    check(secondRepeat - firstRepeat >= 9 && secondRepeat - firstRepeat <= 11, "then every 1/6 s");

    lift(In::Key::Space);
    rig.frame();
    check((rig.release() & WPAD_BUTTON_A) && !button.testButtonA(), "release edge");
    rig.frame();
    check(rig.release() == 0, "release lasts one frame");

    // A tap inside one report still spans a frame.
    press(In::Key::Space);
    lift(In::Key::Space);
    rig.frame();
    check(button.testTriggerA(), "sub-report tap triggers");
    rig.frames_(2);
    check(!button.testButtonA(), "and releases");

    // Double tap between two reads: two presses.
    press(In::Key::Space);
    lift(In::Key::Space);
    press(In::Key::Space);
    lift(In::Key::Space);
    int triggers = 0;
    for (int f = 0; f < 8; ++f) {
        rig.frame();
        triggers += button.testTriggerA() ? 1 : 0;
    }
    check(triggers == 2, "double tap is two triggers");

    // Two inputs on one button.
    press(In::Key::Space);
    rig.frame();
    In::mouseButtonEvent(In::MouseButton::Right, true);
    lift(In::Key::Space);
    rig.frames_(3);
    check(button.testButtonA() && rig.release() == 0, "right mouse keeps A held after Space is released");
    In::mouseButtonEvent(In::MouseButton::Right, false);
    rig.frames_(2);
    check(!button.testButtonA(), "A released with the last input");

    press(In::Key::Escape);
    rig.frame();
    check(button.testTriggerPlus() && button.testTriggerB(), "Escape: Plus (pause) and B (back)");
    lift(In::Key::Escape);
    press(In::Key::Q);
    press(In::Key::C);
    press(In::Key::LeftShift);
    rig.frame();
    check(button.testTriggerLeft() && button.testTriggerC() && button.testTriggerZ(), "Q, C, Shift: left, C, Z");
    lift(In::Key::Q);
    lift(In::Key::C);
    lift(In::Key::LeftShift);
    rig.frames_(2);
    check(button.testReleaseZ() || !button.testButtonZ(), "Z released");
    In::mouseButtonEvent(In::MouseButton::Left, true);
    rig.frame();
    check(button.testTriggerB(), "left mouse: B");
    In::mouseButtonEvent(In::MouseButton::Left, false);
    rig.frames_(2);

    // Remapping takes effect at once.
    In::Bindings remapped = In::bindings();
    remapped.clear(In::Action::A);
    remapped.bind(In::Action::A, In::Binding::key(In::Key::J));
    In::setBindings(remapped);
    press(In::Key::Space);
    rig.frames_(2);
    check(!button.testButtonA(), "Space unbound");
    lift(In::Key::Space);
    press(In::Key::J);
    rig.frame();
    check(button.testTriggerA(), "J bound to A");
    lift(In::Key::J);
    rig.frames_(2);
}

// --- Stick ----------------------------------------------------------------

void testStick() {
    Rig rig;
    const WPadStick& stick = *rig.pad->mStick;

    press(In::Key::W);
    rig.frame();
    check(near(stick.mStick.x, 0.0f, 1e-6f) && near(stick.mStick.y, 1.0f, 1e-6f), "W: stick up, full");
    check(stick.mTrigger == 1 && MR::isDeviceFreeStyle(&rig.latest()), "stick up trigger");
    press(In::Key::D);
    rig.frame();
    const float magnitude = std::hypot(stick.mStick.x, stick.mStick.y);
    check(near(stick.mStick.x, stick.mStick.y, 1e-6f) && near(magnitude, 1.0f, 0.02f),
          "W+D: diagonal, equal axes, unit length (" + std::to_string(magnitude) + ")");
    press(In::Key::A);
    rig.frame();
    check(near(stick.mStick.x, -stick.mStick.y, 1e-6f) && stick.mStick.x < -0.7f, "A after D: last pressed wins (still diagonal with W)");
    lift(In::Key::A);
    rig.frame();
    check(stick.mStick.x > 0.0f, "D again when A is released");
    lift(In::Key::W);
    lift(In::Key::D);
    rig.frame();
    check(stick.mStick.x == 0.0f && stick.mStick.y == 0.0f && stick.mRelease != 0, "released: neutral");
    press(In::Key::A);
    press(In::Key::D);
    rig.frame();
    check(stick.mStick.x > 0.99f, "D pressed after A wins");
    lift(In::Key::A);
    lift(In::Key::D);
    rig.frame();

    // Raw stick values invert KPAD's cross clamp (15..71).
    namespace Det = PetariNative::Input::Detail;
    for (int i = 1; i <= 100; ++i) {
        const float v = i / 100.0f;
        const int raw = Det::rawStickAxis(v);
        const float back = static_cast<float>(raw - Det::kStickMin) / (Det::kStickMax - Det::kStickMin);
        check(near(back, v, 0.5f / (Det::kStickMax - Det::kStickMin) + 1e-6f) && Det::rawStickAxis(-v) == -raw,
              "stick axis " + std::to_string(v));
    }
    check(Det::rawStickAxis(0.0f) == 0, "zero stick");
}

// --- Focus ----------------------------------------------------------------

void testFocusLoss() {
    Rig rig;
    In::mouseMoved(640.0f, 360.0f);
    press(In::Key::W);
    press(In::Key::Space);
    In::mouseButtonEvent(In::MouseButton::Left, true);
    rig.frames_(10);
    check(rig.pad->mButton->testButtonA() && rig.pad->mButton->testButtonB() && rig.pad->mStick->mStick.y > 0.99f &&
              rig.pad->mPointer->mIsPointInScreen,
          "holding A, B, forward, pointer on screen");

    In::focusChanged(false);
    rig.frame();
    check((rig.release() & (WPAD_BUTTON_A | WPAD_BUTTON_B)) == (WPAD_BUTTON_A | WPAD_BUTTON_B) && rig.hold() == 0,
          "focus loss: release edges for A and B");
    check(rig.pad->mStick->mStick.y == 0.0f && rig.latest().dpd_valid_fg == 0, "stick neutral, no sensor bar dots");
    press(In::Key::Space);
    In::mouseButtonEvent(In::MouseButton::Right, true);
    In::mouseMoved(100.0f, 100.0f);
    rig.frames_(15);
    check(rig.hold() == 0 && !rig.pad->mPointer->mIsPointInScreen, "input while unfocused is ignored; pointer leaves the screen");

    In::focusChanged(true);
    lift(In::Key::Space);
    In::mouseButtonEvent(In::MouseButton::Right, false);
    lift(In::Key::W);
    rig.frames_(3);
    check(rig.hold() == 0 && rig.trig() == 0, "stale releases after refocus change nothing");
    press(In::Key::Space);
    rig.frame();
    check(rig.pad->mButton->testTriggerA(), "new presses work after refocus");
    lift(In::Key::Space);
    rig.frames_(2);
}

// --- Pointer --------------------------------------------------------------

// Moves the pointer onto the screen from off it, so KPAD takes the first
// position without smoothing, and returns the frame's newest status.
const KPADStatus& pointAt(Rig& rig, float x, float y) {
    In::mouseLeft();
    rig.frames_(2);
    In::mouseMoved(x, y);
    rig.frame();
    return rig.latest();
}

void testPointer() {
    Rig rig;
    const float tolerance = 0.005f;

    // 16:9 window, 4:3 image: pillarbox bars 160 wide.
    In::setViewport(In::Viewport::letterbox(1280.0f, 720.0f, 4.0f / 3.0f));
    struct Point {
        float x, y, posX, posY;
    } points[] = {
        {640.0f, 360.0f, 0.0f, 0.0f},    {160.0f, 360.0f, -1.0f, 0.0f}, {1120.0f, 360.0f, 1.0f, 0.0f},
        {640.0f, 0.0f, 0.0f, -1.0f},     {640.0f, 719.0f, 0.0f, 0.99722f}, {1120.0f, 0.0f, 1.0f, -1.0f},
        {400.0f, 540.0f, -0.5f, 0.5f},
    };
    for (const Point& p : points) {
        const KPADStatus& s = pointAt(rig, p.x, p.y);
        check(s.dpd_valid_fg == 2 && near(s.pos.x, p.posX, tolerance) && near(s.pos.y, p.posY, tolerance),
              "pillarbox pointer " + std::to_string(p.x) + "," + std::to_string(p.y) + " -> " + std::to_string(s.pos.x) + "," +
                  std::to_string(s.pos.y));
    }
    const KPADStatus& bar = pointAt(rig, 80.0f, 360.0f);
    check(bar.dpd_valid_fg == 2 && near(bar.pos.x, -7.0f / 6.0f, tolerance), "over the pillarbox bar: off the image, still seen");

    // 4:3 window, 16:9 image: letterbox bars 150 high.
    In::setViewport(In::Viewport::letterbox(800.0f, 600.0f, 16.0f / 9.0f));
    const KPADStatus& top = pointAt(rig, 800.0f, 75.0f);
    check(near(top.pos.x, 1.0f, tolerance) && near(top.pos.y, -1.0f, tolerance), "letterbox: image corner");
    check(near(top.dist, 2.0f, 0.02f), "dist is the configured 2 m");
    // Beyond what the IR camera can see, as on the Wii.
    const KPADStatus& far = pointAt(rig, 800.0f, 0.0f);
    check(far.dpd_valid_fg == 0, "far above the image: sensor bar out of view");

    // The game's pointer: valid after 5 on-screen reports, then tracks.
    In::setViewport(In::Viewport::letterbox(1280.0f, 720.0f, 16.0f / 9.0f));
    In::mouseLeft();
    rig.frames_(6);
    WPadPointer& pointer = *rig.pad->mPointer;
    check(!pointer.mIsPointInScreen, "pointer off screen");
    In::mouseMoved(320.0f, 180.0f);
    rig.frame();
    check(!pointer.mIsPointInScreen, "not yet in screen after 3 or 4 reports");
    rig.frame();
    TVec2f pos;
    pointer.getPointingPos(&pos);
    check(pointer.mIsPointInScreen && near(pos.x, -0.5f, tolerance) && near(pos.y, -0.5f, tolerance), "in screen after 5 reports");

    // Small mouse motion: KPAD's smoothing (the game's play radius 0.03)
    // follows within a few hundredths.
    In::mouseMoved(640.0f, 360.0f);
    rig.frames_(20);
    pointer.getPointingPos(&pos);
    check(near(pos.x, 0.0f, 0.02f) && near(pos.y, 0.0f, 0.02f), "pointer follows a large move");
    check(pointer.mIsPointerMoved == false, "and settles");
    In::mouseMoved(700.0f, 360.0f);
    rig.frame();
    check(pointer.mIsPointerMoved, "motion reported to the game");
    In::mouseLeft();
    rig.frames_(3);
    check(!pointer.mIsPointInScreen, "mouse leaves the window: pointer off screen");

    // The game's sensor bar height is honoured (KPADSetSensorHeight).
    KPADSetSensorHeight(0, 0.35f);
    const KPADStatus& high = pointAt(rig, 960.0f, 540.0f);
    check(near(high.pos.x, 0.5f, tolerance) && near(high.pos.y, 0.5f, tolerance), "sensor bar above the screen");
}

// --- Shake (spin) ---------------------------------------------------------

// MarioActor::updateControllerSwing: a spin request is a rising edge of
// isCorePadSwing after the 10-frame cooldown A and B start.
struct SpinDetector {
    bool previous = false;
    int cooldown = 10;
    int requests = 0;
    int lastRequestFrame = -1;

    void update(const WPad& pad, int frame) {
        if (pad.mButton->testTriggerA() || pad.mButton->testTriggerB()) {
            cooldown = 10;
        }
        const bool swing = pad.mCorePadSwing->mIsSwing;
        const bool was = previous;
        previous = swing;
        if (cooldown != 0) {
            --cooldown;
        } else if (swing && !was) {
            ++requests;
            lastRequestFrame = frame;
        }
    }
};

struct SwingCounter {
    int swingEdges = 0;
    int triggers = 0;
    bool previous = false;
    void update(const WPad& pad) {
        const bool swing = pad.mCorePadSwing->mIsSwing;
        swingEdges += swing && !previous ? 1 : 0;
        previous = swing;
        triggers += pad.mCorePadSwing->mIsTriggerSwing ? 1 : 0;
    }
};

void testShake() {
    Rig rig;
    rig.frames_(30);
    SpinDetector spin;
    SwingCounter counter;
    int frame = 0;
    auto run = [&](int count) {
        for (int i = 0; i < count; ++i, ++frame) {
            rig.frame();
            spin.update(*rig.pad, frame);
            counter.update(*rig.pad);
        }
    };
    run(20);
    check(spin.requests == 0 && counter.swingEdges == 0, "no swing at rest");

    press(In::Key::F);
    const int pressFrame = frame;
    run(1);
    lift(In::Key::F);
    run(119);
    check(counter.swingEdges == 1 && counter.triggers == 1, "one F tap: one swing and one swing trigger (" +
                                                                 std::to_string(counter.swingEdges) + ", " +
                                                                 std::to_string(counter.triggers) + ")");
    check(spin.requests == 1 && spin.lastRequestFrame - pressFrame <= 2, "Mario's spin request within 2 frames");

    press(In::Key::F);
    run(120);
    lift(In::Key::F);
    run(30);
    check(spin.requests == 2 && counter.swingEdges == 2, "holding F is one spin");

    for (int i = 0; i < 3; ++i) {
        press(In::Key::F);
        run(1);
        lift(In::Key::F);
        run(35);
    }
    check(spin.requests == 5 && counter.swingEdges == 5, "taps 0.6 s apart: one spin each");

    // Jump then spin within 10 frames: the game's own cooldown drops it.
    press(In::Key::Space);
    run(1);
    lift(In::Key::Space);
    press(In::Key::F);
    run(1);
    lift(In::Key::F);
    run(60);
    check(spin.requests == 5, "shake right after A is ignored by Mario's cooldown");

    // Walking and tilting are not swings.
    press(In::Key::Tab);
    for (In::KeyCode key : {In::Key::W, In::Key::D, In::Key::S, In::Key::A}) {
        press(key);
        run(30);
        lift(key);
        run(30);
    }
    lift(In::Key::Tab);
    press(In::Key::W);
    run(60);
    lift(In::Key::W);
    run(60);
    check(spin.requests == 5 && counter.swingEdges == 6, "full tilts and walking make no swing");
}

// --- Tilt -----------------------------------------------------------------

// SphereAccelSensorController::clacXY's angles (Star Ball), before scaling.
void sphereAngles(const TVec3f& a, float* angleXY, float* angleYZ) {
    const float n = std::hypot(a.x, std::fabs(a.y));
    *angleXY = std::asin(a.x / n);
    const float ny = std::hypot(-a.y, a.z);
    const float v1x = -a.y / ny;
    const float v1y = a.z / ny;
    const float base = 10.0f * 3.14159265f / 180.0f;
    const float v2x = std::cos(base);
    const float v2y = std::sin(base);
    float angle = std::acos(std::fmin(1.0f, v1x * v2x + v1y * v2y));
    if (v1y * v2x - v1x * v2y < 0.0f) {
        angle = -angle;
    }
    *angleYZ = angle;
}

bool nearAngleDegrees(const TVec3f& a, const TVec3f& b, float degrees) {
    const float cosine = (a.x * b.x + a.y * b.y + a.z * b.z) / (std::sqrt(a.dot(a)) * std::sqrt(b.dot(b)));
    return cosine >= std::cos(degrees * 3.14159265f / 180.0f);
}

void testTilt() {
    Rig rig;
    In::mouseMoved(640.0f, 360.0f);
    TVec3f acc;

    // Level: the Ray tutorial's "straight".
    rig.frames_(30);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(std::fabs(acc.x) < 0.25f && std::fabs(acc.y) < 0.45f, "level remote: SurfRayTutorial straight");

    // Ray turns: twist 45 degrees; the tutorial wants 0.65 g across.
    press(In::Key::Tab);
    press(In::Key::A);
    rig.frames_(40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(acc.x <= -0.65f && acc.y >= -0.5f, "Tab+A twists left: SurfRayTutorial turn left, not standing");
    check(rig.latest().dpd_valid_fg == 2, "pointer still tracks while twisted");
    check(near(rig.latest().horizon.x, std::cos(0.7854f), 0.05f) && rig.latest().horizon.y < -0.6f,
          "KPAD horizon shows the twist");
    const KPADStatus& twisted = pointAt(rig, 960.0f, 180.0f);
    check(twisted.dpd_valid_fg == 2 && near(twisted.pos.x, 0.5f, 0.005f) && near(twisted.pos.y, -0.5f, 0.005f),
          "twisted remote still points where the mouse is (" + std::to_string(twisted.pos.x) + ", " +
              std::to_string(twisted.pos.y) + ")");
    lift(In::Key::A);
    press(In::Key::D);
    rig.frames_(40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(acc.x >= 0.65f, "Tab+D twists right");
    lift(In::Key::D);
    rig.frames_(30);
    check(rig.pad->mStick->mStick.x == 0.0f, "stick stays neutral while tilting");
    lift(In::Key::Tab);

    // Star Ball: upright posture, then tilt.
    press(In::Key::T);
    rig.frame();
    lift(In::Key::T);
    check(In::posture() == In::Posture::Upright, "T toggles the upright posture");
    rig.frames_(60);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(nearAngleDegrees(acc, TVec3f(0.0f, -1.0f, 0.0f), 30.0f), "upright: TamakoroTutorial raise check passes");
    float xy;
    float yz;
    sphereAngles(acc, &xy, &yz);
    check(std::fabs(xy) < 0.01f && std::fabs(yz) < 0.02f, "upright rest: Star Ball neutral");
    check(rig.latest().dpd_valid_fg == 0, "upright: no sensor bar, as on the Wii");
    check(rig.latest().acc_vertical.x < 0.7f, "KPAD sees the remote pointing up");

    press(In::Key::Tab);
    press(In::Key::D);
    rig.frames_(40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    sphereAngles(acc, &xy, &yz);
    check(xy > 0.7f, "upright, Tab+D: Star Ball rolls right (" + std::to_string(xy) + ")");
    lift(In::Key::D);
    press(In::Key::W);
    rig.frames_(40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    sphereAngles(acc, &xy, &yz);
    check(yz > 0.7f && std::fabs(xy) < 0.01f, "upright, Tab+W: Star Ball rolls forward (" + std::to_string(yz) + ")");
    lift(In::Key::W);
    press(In::Key::S);
    rig.frames_(40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    sphereAngles(acc, &xy, &yz);
    check(yz < -0.3f, "upright, Tab+S: back");
    lift(In::Key::S);
    lift(In::Key::Tab);

    In::setPosture(In::Posture::Pointing);
    In::mouseMoved(640.0f, 360.0f);
    rig.frames_(60);
    check(rig.latest().dpd_valid_fg == 2, "back to pointing: pointer returns");
}

// --- Rumble, speaker, status ----------------------------------------------

std::vector<std::string> gDeviceEvents;
void deviceCallback(s32 chan, s32 result) {
    gDeviceEvents.push_back(std::to_string(chan) + ":" + std::to_string(result));
}
std::vector<std::vector<std::uint8_t>> gSpeakerPackets;
void speakerSink(int chan, const std::uint8_t* data, std::uint16_t length, void* user) {
    check(chan == 0 && user == &gSpeakerPackets, "speaker sink context");
    gSpeakerPackets.emplace_back(data, data + length);
}

void testDevice() {
    {
        Rig rig;
        WPADControlMotor(0, WPAD_MOTOR_RUMBLE);
        check(In::rumbleActive(0), "rumble on");
        WPADControlMotor(0, WPAD_MOTOR_STOP);
        check(!In::rumbleActive(0), "rumble off");
        WPADControlMotor(1, WPAD_MOTOR_RUMBLE);
        check(!In::rumbleActive(1), "no rumble without a remote");

        WPADInfo info{};
        gDeviceEvents.clear();
        check(WPADGetInfoAsync(0, &info, deviceCallback) == WPAD_ERR_NONE && gDeviceEvents.empty(), "status request queued");
        check(WPADGetInfoAsync(0, &info, deviceCallback) == WPAD_ERR_BUSY && gDeviceEvents.back() == "0:-2",
              "one status request at a time");
        rig.frame();
        check(gDeviceEvents.back() == "0:0" && info.battery == WPAD_BATTERY_LEVEL_MAX && info.attach && info.dpd,
              "status: full battery, Nunchuk, DPD");
        check(WPADGetInfoAsync(1, &info, deviceCallback) == WPAD_ERR_NO_CONTROLLER && gDeviceEvents.back() == "1:-1",
              "status of an absent remote");

        // Speaker, as SpkSpeakerCtrl drives it.
        gDeviceEvents.clear();
        gSpeakerPackets.clear();
        In::setSpeakerSink(speakerSink, &gSpeakerPackets);
        check(!WPADIsSpeakerEnabled(0), "speaker off");
        check(WPADControlSpeaker(0, 1, deviceCallback) == WPAD_ERR_NONE, "speaker on queued");
        rig.frame();
        check(WPADIsSpeakerEnabled(0) && gDeviceEvents.back() == "0:0", "speaker on completes");
        check(WPADControlSpeaker(0, 4, deviceCallback) == WPAD_ERR_NONE, "play queued");
        u8 packet[20];
        for (int i = 0; i < 20; ++i) {
            packet[i] = static_cast<u8>(i);
        }
        rig.frame();
        check(WPADCanSendStreamData(0), "can stream");
        check(WPADSendStreamData(0, packet, 20) == WPAD_ERR_NONE, "packet accepted");
        check(!WPADCanSendStreamData(0) && WPADSendStreamData(0, packet, 20) == WPAD_ERR_BUSY, "one packet in flight");
        In::pumpReports(1);
        check(gSpeakerPackets.size() == 1 && gSpeakerPackets[0].size() == 20 && gSpeakerPackets[0][19] == 19,
              "packet delivered to the sink");
        WPADControlSpeaker(0, 2, nullptr);
        In::pumpReports(1);
        WPADSendStreamData(0, packet, 20);
        In::pumpReports(1);
        check(gSpeakerPackets.size() == 1, "muted speaker plays nothing");
        check(WPADGetSpeakerVolume() == 0x58, "speaker volume from settings");
        check(aborts([] {
                  u8 big[21] = {};
                  WPADSendStreamData(0, big, 21);
              }),
              "oversized packet aborts");
        WPADControlSpeaker(0, 0, deviceCallback);
        rig.frame();
        check(!WPADIsSpeakerEnabled(0), "speaker off again");
        In::setSpeakerSink(nullptr, nullptr);

        // Miis stored on a remote (RFL_Controller.c): a keyboard remote has none.
        u8 faceData[64];
        gDeviceEvents.clear();
        check(WPADReadFaceData(0, faceData, sizeof(faceData), 0, deviceCallback) == WPAD_ERR_INVALID,
              "no Mii memory on a keyboard remote");
        check(WPADReadFaceData(1, faceData, sizeof(faceData), 0, deviceCallback) == WPAD_ERR_NO_CONTROLLER,
              "face data from an absent remote");
        rig.frame();
        check(gDeviceEvents.empty(), "a refused face-data read has no callback");

        WPADControlMotor(0, WPAD_MOTOR_RUMBLE);
        WPADDisconnect(0);
        rig.frame();
        check(!In::rumbleActive(0), "disconnect stops rumble");
    }
    {
        In::resetForTesting();
        PSC::Settings settings = PSC::current();
        settings.motorMode = 0;
        settings.sensorBarPosition = 1;
        check(PSC::set(settings, nullptr), "settings accepted");
        In::setClock(In::Clock::Manual);
        KPADInit();
        In::pumpReports(45);
        check(WPADGetSensorBarPosition() == WPAD_SENSOR_BAR_POS_TOP, "sensor bar position from settings");
        WPADControlMotor(0, WPAD_MOTOR_RUMBLE);
        check(!In::rumbleActive(0), "rumble disabled in settings");
        PSC::reset();
    }
}

// --- Alarm clock and threads ----------------------------------------------

void testAlarmClock() {
    In::resetForTesting();
    PSC::reset();
    gCallbacks.clear();
    KPADInit();
    WPADSetConnectCallback(0, connectCallback);
    In::setViewport(In::Viewport::letterbox(1280.0f, 720.0f, 16.0f / 9.0f));

    std::atomic<bool> stop{false};
    std::thread host([&] {
        // An event thread that is not an OS thread.
        float x = 0.0f;
        while (!stop) {
            In::mouseMoved(320.0f + x, 360.0f);
            In::keyEvent(In::Key::W, true, false);
            In::keyEvent(In::Key::W, false, false);
            x = x > 600.0f ? 0.0f : x + 7.0f;
            std::this_thread::sleep_for(std::chrono::microseconds(700));
        }
    });
    KPADStatus statuses[120];
    long total = 0;
    for (int f = 0; f < 30; ++f) {
        OSSleepTicks(OSMillisecondsToTicks(16));
        total += KPADRead(0, statuses, 120);
    }
    stop = true;
    host.join();
    check(!gCallbacks.empty() && gCallbacks[0] == "connect0:0", "alarm clock connects the remote");
    check(total > 30, "alarm clock produces reports (" + std::to_string(total) + ")");
    In::resetForTesting();
}

#ifdef PETARI_INPUT_TEST_SDL3
void testSdl3() {
    Rig rig;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_SPACE;
    event.key.down = true;
    check(In::SDL3::handleEvent(event), "key event consumed");
    event.key.repeat = true;
    In::SDL3::handleEvent(event);
    rig.frame();
    check(rig.pad->mButton->testTriggerA(), "SDL Space: A");
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    event.key.repeat = false;
    In::SDL3::handleEvent(event);

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 640.0f;
    motion.motion.y = 360.0f;
    check(In::SDL3::handleEvent(motion), "motion consumed");
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    click.button.button = SDL_BUTTON_LEFT;
    click.button.down = true;
    click.button.x = 640.0f;
    click.button.y = 360.0f;
    In::SDL3::handleEvent(click);
    rig.frames_(3);
    check(rig.pad->mButton->testButtonB() && rig.pad->mPointer->mIsPointInScreen, "SDL click: B with the pointer on screen");

    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    In::SDL3::handleEvent(focus);
    rig.frames_(2);
    check(rig.hold() == 0, "SDL focus loss releases everything");
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    In::SDL3::handleEvent(focus);
    SDL_Event other{};
    other.type = SDL_EVENT_QUIT;
    check(!In::SDL3::handleEvent(other), "unrelated events are not consumed");
}
#endif

}  // namespace

int main() {
    __OSThreadInit();
    testBindings();
    testConnection();
    testButtons();
    testStick();
    testFocusLoss();
    testPointer();
    testShake();
    testTilt();
    testDevice();
#ifdef PETARI_INPUT_TEST_SDL3
    testSdl3();
#endif
    testAlarmClock();
    std::printf("native input tests passed (%d checks)\n", checks);
    return 0;
}

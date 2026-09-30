// Tests for native keyboard and mouse input: the emulated WPAD layer, the
// SDK's KPAD.c on top of it, and the game's own Wii Remote classes (WPad,
// WPadButton, WPadStick, WPadPointer, WPadAcceleration, WPadHVSwing) reading
// the result, so the checks are on what gameplay sees.
//
// Reports are pumped by hand (Clock::Manual): 10 reports per 3 frames, the
// Wii's 200 Hz reports against a 60 Hz game loop.

#include <revolution/kpad.h>
#include <revolution/os.h>
#include <revolution/sc.h>
#include <revolution/vi.h>
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
#include "Game/Util/TriggerChecker.hpp"
#include "petari/input.hpp"
#include "petari/platform/sc.hpp"
#include "petari/platform/vi.hpp"
#include "../input/remote_model.hpp"

#ifdef PETARI_INPUT_TEST_SDL3
#include "petari/input_sdl3.hpp"
#endif

extern "C" void __OSThreadInit(void);

namespace In = PetariNative::Input;
namespace PSC = PetariNative::Platform::SC;
namespace PVI = PetariNative::Platform::VI;
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
    check(!hasBinding(d, Action::B, Binding::key(In::Key::Escape)), "Escape does not hold B, which blocks pause");
    check(hasBinding(d, Action::B, Binding::key(In::Key::Backspace)), "Backspace backs out of menus");
    check(hasBinding(d, Action::Walk, Binding::key(In::Key::LeftAlt)), "Left Alt walks");
    check(hasBinding(d, Action::Start, Binding::key(In::Key::Return)), "Return is Start (confirm; A and B on the title)");
    check(hasBinding(d, Action::Start, Binding::key(In::Key::KeypadEnter)), "keypad Enter is Start");
    check(!hasBinding(d, Action::A, Binding::key(In::Key::Return)) && !hasBinding(d, Action::B, Binding::key(In::Key::Return)),
          "Return is bound only to Start");

    const std::string text = d.serialize();
    In::Bindings parsed;
    std::string error;
    check(parsed.parse(text, &error), "defaults parse: " + error);
    check(parsed.serialize() == text, "bindings round-trip through text");
    check(text.find("A=Key:Space,Mouse:Right,Pad:South\n") != std::string::npos,
          "text form names keys, mouse buttons and controller buttons");

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

// --- Controls summary (the Home menu's Controls page) ---------------------

std::string summaryOf(const std::vector<In::ControlsLine>& lines, const std::string& action) {
    for (const In::ControlsLine& line : lines) {
        if (line.action == action) {
            return line.inputs;
        }
    }
    return "<missing " + action + ">";
}

void testControlsSummary() {
    const std::vector<In::ControlsLine> d = In::controlsSummary(In::Bindings::defaults());
    const struct {
        const char* action;
        const char* inputs;
    } expected[] = {
        {"Move", "W A S D / left stick"},
        {"Jump / confirm", "Space / Right mouse / Pad bottom"},
        {"Start (title: A and B)", "Return / Keypad Enter / Pad top"},
        {"Spin", "F / Pad left / Pad RB"},
        {"Crouch / ground pound", "Shift / Pad LT"},
        {"Star Pointer", "Mouse / right stick (R3: center)"},
        {"Shoot Star Bits / back", "Left mouse / Backspace / Pad right / Pad RT"},
        {"Grab (Pull Stars)", "Hold Space / Right mouse / Pad bottom on the target"},
        {"Rotate camera", "Q / E"},
        {"Recenter camera", "C / Pad LB"},
        {"First-person view", "Up arrow / Pad D-pad up (leave: Down arrow / Pad D-pad down)"},
        {"Walk slowly", "Hold Left Alt"},
        {"Pause", "Escape / Pad Start / - / Pad Back"},
        {"Star Ball / Ray", "W A S D tilt while riding"},
        {"Tilt the remote by hand", "Hold Tab + W A S D"},
        {"This menu", "F1 / Pad Guide"},
    };
    for (const auto& e : expected) {
        check(summaryOf(d, e.action) == e.inputs,
              std::string("summary: ") + e.action + " = \"" + summaryOf(d, e.action) + "\"");
    }
    check(d.size() == sizeof(expected) / sizeof(expected[0]) && d.size() <= 20, "summary fits the Controls page");
    for (const In::ControlsLine& line : d) {
        check(line.action.size() < 40 && line.inputs.size() < 64, "summary line fits the page's text fields: " + line.action);
    }

    // Remaps from controls.txt show, and unbound actions say so.
    In::Bindings remapped = In::Bindings::defaults();
    std::string error;
    check(remapped.parse("StickUp=Key:I\nStickLeft=Key:J\nStickDown=Key:K\nStickRight=Key:L\n"
                         "Shake=Mouse:Middle\nNunchukZ=Key:LeftShift\nStart=\nWalk=\nPlus=Key:P\nMinus=\n",
                         &error),
          "remap parses: " + error);
    const std::vector<In::ControlsLine> r = In::controlsSummary(remapped);
    check(summaryOf(r, "Move") == "I J K L / left stick", "remapped movement");
    check(summaryOf(r, "Spin") == "Middle mouse", "remapped spin to a mouse button");
    check(summaryOf(r, "Crouch / ground pound") == "Left Shift", "only the left Shift left");
    check(summaryOf(r, "Start (title: A and B)") == "(not bound)", "unbound Start");
    check(summaryOf(r, "Walk slowly") == "(not bound)", "unbound hold reads as not bound");
    check(summaryOf(r, "Pause") == "P", "remapped pause");
    check(summaryOf(r, "Star Ball / Ray") == "I J K L tilt while riding", "ride line follows movement");
    check(In::displayName(In::Binding::key(In::Key::Num1)) == "1" && In::displayName(In::Binding::key(In::Key::G)) == "G",
          "digits and letters display plainly");

    // The title screen's hint.
    check(In::titleHint(In::Bindings::defaults()) == "Keyboard: Return starts   |   F1: all controls",
          "title hint: " + In::titleHint(In::Bindings::defaults()));
    In::Bindings noStart = In::Bindings::defaults();
    check(noStart.parse("Start=\nHome=\n", &error), "unbind Start and Home");
    check(In::titleHint(noStart) == "Keyboard: hold Space, then press Backspace to start",
          "title hint without Start names keyboard keys: " + In::titleHint(noStart));
    In::Bindings mouseOnly = In::Bindings::defaults();
    check(mouseOnly.parse("Start=Mouse:Middle\n", &error), "Start on a mouse button");
    check(In::titleHint(mouseOnly) == "Keyboard: Middle mouse starts   |   F1: all controls", "title hint follows remaps");
    for (const char* text : {"Keyboard: Return starts   |   F1: all controls"}) {
        for (const char* c = text; *c != '\0'; ++c) {
            check(static_cast<unsigned char>(*c) < 0x80, "title hint is ASCII (any ImGui font draws it)");
        }
    }
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
    check(button.testTriggerPlus() && !button.testButtonB(), "Escape: Plus without B so pause is permitted");
    rig.frames_(18);
    check(button.testButtonPlus() && !button.testButtonA() && !button.testButtonB(), "pause hold keeps A and B up");
    lift(In::Key::Escape);
    rig.frames_(2);
    press(In::Key::Backspace);
    rig.frame();
    check(button.testTriggerB() && !button.testButtonPlus(), "Backspace: B without Plus");
    lift(In::Key::Backspace);
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

// --- Start (Return) --------------------------------------------------------

// The title screen (TitleSequenceProduct::exeLogoDisplay), in a native build:
// each frame it reports the prompt, then feeds TriggerCheckers with
// testCorePadButtonA/B; it starts when both levels are high on one frame.
struct TitlePrompt {
    TriggerChecker a;
    TriggerChecker b;
    bool update(WPadButton& button) {
        In::titlePromptShown();
        a.update(button.testButtonA());
        b.update(button.testButtonB());
        return a.getLevel() && b.getLevel();
    }
};

void testStart() {
    Rig rig;
    WPadButton& button = *rig.pad->mButton;

    // Outside the title Return is A alone: menus that check B (back) before A
    // (ScenarioSelectLayout::control, GalaxyMapController) must confirm.
    press(In::Key::Return);
    rig.frame();
    check(button.testTriggerA() && !button.testButtonB(), "Return outside the title: A without B");
    rig.frames_(30);
    check(button.testButtonA() && !button.testButtonB(), "held Return outside the title never adds B");
    lift(In::Key::Return);
    rig.frames_(2);
    check(rig.hold() == 0, "released");

    // On the prompt a tap of Return starts the game.
    TitlePrompt title;
    rig.frame();
    check(!title.update(button), "prompt up, nothing pressed");
    press(In::Key::Return);
    lift(In::Key::Return);
    rig.frame();
    check(title.update(button), "Return tap on the prompt: A and B held on the same frame");
    check(button.testTriggerA() && button.testTriggerB(), "A and B trigger together");
    check((rig.trig() & (WPAD_BUTTON_A | WPAD_BUTTON_B)) == (WPAD_BUTTON_A | WPAD_BUTTON_B), "in the same KPAD read");
    rig.frames_(2);
    title.update(button);
    check(!button.testButtonA() && !button.testButtonB(), "and both release");
    rig.frames_(10);  // the title moves on; the prompt lapses

    // Held keypad Enter, pressed before the prompt appears: A at once, B
    // joins on the frame after the prompt first shows.
    TitlePrompt late;
    press(In::Key::KeypadEnter);
    rig.frame();
    check(button.testButtonA() && !button.testButtonB(), "Enter before the prompt: A only");
    check(!late.update(button), "prompt appears: not yet both");
    rig.frame();
    check(late.update(button), "next frame: B joins the held Enter and the title starts");

    // The prompt lapses 100 ms after the title stops reporting it (the title
    // moved on): B drops from a still-held Enter within 7 frames.
    int framesToDrop = -1;
    for (int f = 1; f <= 12 && framesToDrop < 0; ++f) {
        rig.frame();
        if (!button.testButtonB()) {
            framesToDrop = f;
        }
    }
    check(framesToDrop >= 5 && framesToDrop <= 8, "B lapses with the prompt (" + std::to_string(framesToDrop) + " frames)");
    check(button.testButtonA(), "A stays with the key");
    lift(In::Key::KeypadEnter);
    rig.frames_(2);
    check(rig.hold() == 0, "released");

    // A prompt update every frame keeps B continuous (no flicker): the title's
    // B checker never sees an off trigger while Return is held.
    TitlePrompt steady;
    press(In::Key::Return);
    int offTriggers = 0;
    for (int f = 0; f < 40; ++f) {
        rig.frame();
        steady.update(button);
        offTriggers += steady.b.getOffTrigger() ? 1 : 0;
    }
    check(offTriggers == 0 && button.testButtonB(), "held Return on the prompt: B steady");
    lift(In::Key::Return);
    rig.frames_(10);

    // Neither Space alone nor Backspace alone starts the title.
    TitlePrompt alone;
    bool any = false;
    press(In::Key::Space);
    for (int f = 0; f < 10; ++f) {
        rig.frame();
        any = alone.update(button) || any;
    }
    lift(In::Key::Space);
    rig.frames_(2);
    press(In::Key::Backspace);
    for (int f = 0; f < 10; ++f) {
        rig.frame();
        any = alone.update(button) || any;
    }
    lift(In::Key::Backspace);
    rig.frames_(2);
    check(!any, "Space alone or Backspace alone does not start the title");

    // The two-key form still works: hold Space, then Backspace.
    TitlePrompt twoKeys;
    press(In::Key::Space);
    rig.frame();
    check(!twoKeys.update(button), "Space held: not yet");
    press(In::Key::Backspace);
    rig.frame();
    check(twoKeys.update(button), "Space held plus Backspace: starts");
    lift(In::Key::Space);
    lift(In::Key::Backspace);
    rig.frames_(10);

    // Focus loss releases Return's A and B.
    TitlePrompt focus;
    press(In::Key::Return);
    rig.frame();
    focus.update(button);
    rig.frame();
    focus.update(button);
    In::focusChanged(false);
    rig.frames_(2);
    check(rig.hold() == 0, "focus loss releases Start");
    In::focusChanged(true);
    lift(In::Key::Return);
    rig.frames_(10);

    // Return counts as an A press for the jump-then-spin delay, like Space.
    press(In::Key::Return);
    lift(In::Key::Return);
    rig.frame();
    press(In::Key::F);
    lift(In::Key::F);
    bool swingEarly = false;
    for (int f = 0; f < 9; ++f) {
        rig.frame();
        swingEarly = swingEarly || rig.pad->mCorePadSwing->mIsSwing;
    }
    bool swingLater = false;
    for (int f = 0; f < 20; ++f) {
        rig.frame();
        swingLater = swingLater || rig.pad->mCorePadSwing->mIsSwing;
    }
    check(!swingEarly && swingLater, "Return then F: the flick waits out Mario's A/B swing lockout");
    rig.frames_(20);

    // The host-side view of the prompt, for the native title hint.
    rig.frames_(10);
    check(!In::titlePromptActive(), "host: no title prompt");
    In::titlePromptShown();
    rig.frame();
    check(In::titlePromptActive(), "host: title prompt reported");
    rig.frames_(8);
    check(!In::titlePromptActive(), "host: prompt lapses once the title stops reporting it");

    // Remappable by name.
    In::Bindings remapped = In::bindings();
    std::string error;
    check(remapped.parse("Start=Key:G\n", &error), "Start remaps: " + error);
    In::setBindings(remapped);
    TitlePrompt remap;
    press(In::Key::G);
    rig.frame();
    remap.update(button);
    rig.frame();
    check(remap.update(button), "remapped Start key starts the title");
    lift(In::Key::G);
    rig.frames_(10);
    In::setBindings(In::Bindings::defaults());
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

    // Walk (Left Alt) shortens the stick.
    press(In::Key::LeftAlt);
    press(In::Key::W);
    rig.frame();
    check(near(stick.mStick.y, 0.5f, 0.01f) && stick.mStick.x == 0.0f, "Alt+W: half stick (" +
                                                                          std::to_string(stick.mStick.y) + ")");
    press(In::Key::D);
    rig.frame();
    check(near(stick.mStick.x, stick.mStick.y, 1e-6f) && near(std::hypot(stick.mStick.x, stick.mStick.y), 0.5f, 0.01f),
          "Alt+W+D: half-length diagonal");
    lift(In::Key::LeftAlt);
    rig.frames_(2);
    check(near(std::hypot(stick.mStick.x, stick.mStick.y), 1.0f, 0.02f), "releasing Alt runs again");
    lift(In::Key::W);
    lift(In::Key::D);
    rig.frames_(2);
    check(stick.mStick.x == 0.0f && stick.mStick.y == 0.0f, "released");
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

    // Jump then spin at once: the flick waits out Mario's post-A lockout.
    press(In::Key::Space);
    const int jumpFrame = frame;
    run(1);
    lift(In::Key::Space);
    press(In::Key::F);
    run(1);
    lift(In::Key::F);
    run(60);
    check(spin.requests == 6 && spin.lastRequestFrame - jumpFrame <= 13,
          "shake right after A still spins, once the game's lockout has passed");

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
    check(spin.requests == 6 && counter.swingEdges == 6, "full tilts and walking make no swing");
}

// Space then F after 0..12 frames, at each frame phase of the 10-reports-
// per-3-frames cadence: exactly one spin, at most 13 frames after the jump.
// With the delay off, the game's lockout (MarioActor::updateControllerSwing)
// eats the quick cases, which is what the delay exists for.
int jumpThenSpin(int gapFrames, int phase, int delayReports, int* latency) {
    Rig rig;
    In::Settings settings = In::settings();
    settings.shakeDelayAfterButtonReports = delayReports;
    In::setSettings(settings);
    rig.frames_(30 + phase);
    SpinDetector spin;
    int frame = 0;
    auto run = [&](int count) {
        for (int i = 0; i < count; ++i, ++frame) {
            rig.frame();
            spin.update(*rig.pad, frame);
        }
    };
    run(20);
    press(In::Key::Space);
    const int jumpFrame = frame;
    run(1);
    lift(In::Key::Space);
    run(gapFrames);
    press(In::Key::F);
    run(1);
    lift(In::Key::F);
    run(60);
    *latency = spin.lastRequestFrame - jumpFrame;
    return spin.requests;
}

void testJumpThenSpin() {
    int worst = 0;
    for (int gap = 0; gap <= 12; ++gap) {
        for (int phase = 0; phase < 3; ++phase) {
            int latency = 0;
            const int requests = jumpThenSpin(gap, phase, 36, &latency);
            check(requests == 1, "Space, then F after " + std::to_string(gap) + " frames (phase " + std::to_string(phase) +
                                     "): one spin, got " + std::to_string(requests));
            check(latency <= std::max(13, gap + 3), "spin arrives promptly (" + std::to_string(latency) + " frames)");
            worst = std::max(worst, latency);
        }
    }
    int lost = 0;
    for (int gap = 0; gap <= 6; ++gap) {
        int latency = 0;
        lost += jumpThenSpin(gap, 0, 0, &latency) == 0 ? 1 : 0;
    }
    check(lost == 7, "without the delay, F within 6 frames of Space never spins (" + std::to_string(lost) + " of 7 lost)");
    std::printf("jump then spin: every gap spins; latest spin %d frames after the jump\n", worst);
}

// Scripted key events at frame numbers; records the frames of real swing
// rising edges (WPadHVSwing) and of Mario's spin requests (cooldown replica).
struct ShakeRun {
    std::vector<int> swings;
    std::vector<int> spins;
};

struct KeyStep {
    int frame;
    In::KeyCode key;
    bool down;
};

ShakeRun runShakeScript(const std::vector<KeyStep>& steps, int frames, int focusLossFrame = -1, int focusBackFrame = -1) {
    Rig rig;
    rig.frames_(30);
    SpinDetector spin;
    spin.cooldown = 0;  // past Mario's reset lockout
    bool previous = false;
    ShakeRun result;
    for (int frame = 0; frame < frames; ++frame) {
        for (const KeyStep& step : steps) {
            if (step.frame == frame) {
                In::keyEvent(step.key, step.down, false);
            }
        }
        if (frame == focusLossFrame) {
            In::focusChanged(false);
        }
        if (frame == focusBackFrame) {
            In::focusChanged(true);
        }
        rig.frame();
        const int before = spin.requests;
        spin.update(*rig.pad, frame);
        if (spin.requests != before) {
            result.spins.push_back(frame);
        }
        const bool swing = rig.pad->mCorePadSwing->mIsSwing;
        if (swing && !previous) {
            result.swings.push_back(frame);
        }
        previous = swing;
    }
    return result;
}

std::vector<KeyStep> taps(In::KeyCode key, int first, int interval, int count) {
    std::vector<KeyStep> steps;
    for (int i = 0; i < count; ++i) {
        steps.push_back({first + i * interval, key, true});
        steps.push_back({first + i * interval + 1, key, false});
    }
    return steps;
}

std::string frameList(const std::vector<int>& frames) {
    std::string text;
    for (int f : frames) {
        text += (text.empty() ? "" : " ") + std::to_string(f);
    }
    return text;
}

// Flicks at least 250 ms (15 frames) apart, and each one a swing edge.
bool spacedAtLeast(const std::vector<int>& frames, int minimum) {
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (frames[i] - frames[i - 1] < minimum) {
            return false;
        }
    }
    return true;
}

void testRapidShake() {
    // Mashing F: a flick every 250 ms while presses keep coming, and each is
    // a swing edge Mario turns into a spin request (his cooldown only follows
    // A and B).
    for (int interval : {3, 6, 9, 12, 15, 18}) {
        const ShakeRun run = runShakeScript(taps(In::Key::F, 10, interval, 8), 10 + 8 * interval + 60);
        const int lastTap = 10 + 7 * interval;
        std::printf("F every %2d frames (%3d ms): 8 taps -> swings at %s\n", interval, interval * 1000 / 60,
                    frameList(run.swings).c_str());
        const int expected = interval >= 15 ? 8 : 1 + (7 * interval + 14) / 15;  // one per 250 ms, plus the one waiting
        check(static_cast<int>(run.swings.size()) >= std::min(expected, 8) - 1 &&
                  static_cast<int>(run.swings.size()) <= std::min(expected, 8),
              "mashing every " + std::to_string(interval) + " frames: about one swing per 250 ms (" +
                  std::to_string(run.swings.size()) + ", expected " + std::to_string(std::min(expected, 8)) + ")");
        check(spacedAtLeast(run.swings, 14), "swings at least 250 ms apart: " + frameList(run.swings));
        check(run.spins == run.swings, "every swing is a spin request under Mario's cooldown");
        check(run.swings.empty() || run.swings.back() <= lastTap + 20, "no swing trails the last press by more than one wait");
        if (interval <= 12) {
            check(run.swings.size() >= 2, "mashing gives more than one spin");
        }
    }

    // Many presses inside one flick: only one waits.
    const ShakeRun burst = runShakeScript(taps(In::Key::F, 10, 2, 6), 120);
    check(burst.swings.size() == 2, "six presses within 200 ms: the flick and one waiting flick (" +
                                        frameList(burst.swings) + ")");

    // Holding F does not repeat.
    const ShakeRun held = runShakeScript({{10, In::Key::F, true}, {130, In::Key::F, false}}, 180);
    check(held.swings.size() == 1, "holding F for 2 s is one swing (" + frameList(held.swings) + ")");

    // A tap's release does not cancel the flick waiting for it.
    const ShakeRun released = runShakeScript(taps(In::Key::F, 10, 5, 2), 90);
    check(released.swings.size() == 2 && released.swings[1] - released.swings[0] >= 14,
          "second tap released before its turn still flicks, 250 ms later (" + frameList(released.swings) + ")");

    // Losing focus cancels the waiting flick; nothing fires after focus returns.
    // (The second tap, at frame 12, waits until about frame 25; focus goes at 14.)
    const ShakeRun waiting = runShakeScript(taps(In::Key::F, 10, 2, 2), 150);
    check(waiting.swings.size() == 2, "control: with focus kept, the second tap flicks (" + frameList(waiting.swings) + ")");
    const ShakeRun lost = runShakeScript(taps(In::Key::F, 10, 2, 2), 150, 14, 60);
    check(lost.swings.size() == 1 && lost.swings[0] <= 13, "focus loss cancels the waiting flick (" +
                                                               frameList(lost.swings) + ")");

    // After Space, the first flick waits out Mario's lockout, the next is 250 ms later.
    std::vector<KeyStep> jumpMash = {{10, In::Key::Space, true}, {11, In::Key::Space, false}};
    for (const KeyStep& step : taps(In::Key::F, 11, 4, 6)) {
        jumpMash.push_back(step);
    }
    const ShakeRun jump = runShakeScript(jumpMash, 120);
    check(jump.spins.size() >= 2 && jump.spins[0] >= 20 && jump.spins[0] <= 23 && spacedAtLeast(jump.swings, 14),
          "Space then mashed F: first spin after the lockout, then spaced (swings " + frameList(jump.swings) +
              ", spins " + frameList(jump.spins) + ")");
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

// --- Pause tap ------------------------------------------------------------

// PauseButtonCheckerInGame and GameScenePauseControl::tryStartPauseMenu: the
// counter runs only while unpaused, and pauses on exactly the 12th held frame
// with A and B up. PauseMenu closes on a new Plus or Minus press.
struct PauseScene {
    int hold = 0;
    bool paused = false;
    int opens = 0;
    int closes = 0;
    void update(WPadButton& button, bool plus) {
        const bool held = plus ? button.testButtonPlus() : button.testButtonMinus();
        if (!paused) {
            hold = held ? hold + 1 : 0;
            if (hold == 12 && !button.testButtonA() && !button.testButtonB()) {
                paused = true;
                ++opens;
            }
        } else if (button.testTriggerPlus() || button.testTriggerMinus()) {
            paused = false;
            ++closes;
        }
    }
};

void testPauseTap() {
    Rig rig;
    WPadButton& button = *rig.pad->mButton;
    for (const bool plus : {true, false}) {
        const In::KeyCode key = plus ? In::Key::Escape : In::Key::Minus;
        const std::string name = plus ? "Escape" : "Minus";
        PauseScene scene;
        press(key);
        lift(key);
        for (int f = 0; f < 20; ++f) {
            rig.frame();
            scene.update(button, plus);
        }
        check(scene.paused && scene.opens == 1, name + " tap pauses");
        rig.frames_(10);
        press(key);
        lift(key);
        for (int f = 0; f < 60; ++f) {
            rig.frame();
            scene.update(button, plus);
        }
        check(!scene.paused && scene.closes == 1 && scene.opens == 1, name + " tap again resumes, and does not reopen");

        PauseScene heldScene;
        press(key);
        for (int f = 0; f < 60; ++f) {
            rig.frame();
            heldScene.update(button, plus);
        }
        lift(key);
        rig.frames_(20);
        check(heldScene.opens == 1, name + " held: pauses once");
    }
    // Other buttons keep their short pulse.
    press(In::Key::Space);
    lift(In::Key::Space);
    rig.frame();
    rig.frames_(2);
    check(!button.testButtonA(), "A taps stay short");
}

// --- Ride steering --------------------------------------------------------

void testSteering() {
    Rig rig;
    In::mouseMoved(640.0f, 360.0f);
    const WPadStick& stick = *rig.pad->mStick;
    TVec3f acc;
    float xy;
    float yz;
    auto ride = [&rig](In::Steering steering, int frames) {
        for (int f = 0; f < frames; ++f) {
            In::motionControlShown(steering);
            rig.frame();
        }
    };

    // Star Ball: raised at once, WASD roll it without Tab or T.
    ride(In::Steering::Ball, 30);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(nearAngleDegrees(acc, TVec3f(0.0f, -1.0f, 0.0f), 30.0f), "Star Ball: remote raised without T (TamakoroTutorial)");
    check(In::posture() == In::Posture::Pointing, "the T posture is left alone");
    press(In::Key::D);
    ride(In::Steering::Ball, 40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    sphereAngles(acc, &xy, &yz);
    check(xy > 0.7f, "Star Ball, D: rolls right without Tab (" + std::to_string(xy) + ")");
    check(stick.mStick.x == 0.0f && stick.mStick.y == 0.0f, "Star Ball: the stick stays neutral");
    lift(In::Key::D);
    press(In::Key::W);
    ride(In::Steering::Ball, 40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    sphereAngles(acc, &xy, &yz);
    check(yz > 0.7f, "Star Ball, W: rolls forward");
    lift(In::Key::W);

    // Off the ball the hint lapses: level again, and WASD move the stick.
    rig.frames_(40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(std::fabs(acc.x) < 0.25f && std::fabs(acc.y) < 0.45f, "off the ball: remote level again");
    check(rig.latest().dpd_valid_fg == 2, "off the ball: pointer back");
    press(In::Key::W);
    rig.frames_(3);
    check(stick.mStick.y > 0.9f, "off the ball: W moves Mario");
    lift(In::Key::W);
    rig.frames_(3);

    // Ray: level, A/D twist; W does not pitch it out of "straight".
    ride(In::Steering::Ray, 30);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(std::fabs(acc.x) < 0.25f && std::fabs(acc.y) < 0.45f, "Ray: level (SurfRayTutorial straight)");
    press(In::Key::W);
    ride(In::Steering::Ray, 40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(std::fabs(acc.x) < 0.25f && std::fabs(acc.y) < 0.45f, "Ray, W held: still straight");
    check(stick.mStick.y == 0.0f, "Ray: the stick stays neutral");
    lift(In::Key::W);
    press(In::Key::A);
    ride(In::Steering::Ray, 40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(acc.x <= -0.65f && acc.y >= -0.5f, "Ray, A: turn left without Tab (SurfRayTutorial)");
    lift(In::Key::D);
    lift(In::Key::A);
    press(In::Key::D);
    ride(In::Steering::Ray, 40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(acc.x >= 0.65f, "Ray, D: turn right");
    lift(In::Key::D);

    // A stray T before the Ray does not stand the remote up.
    press(In::Key::T);
    rig.frame();
    lift(In::Key::T);
    check(In::posture() == In::Posture::Upright, "T toggled");
    ride(In::Steering::Ray, 40);
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    check(std::fabs(acc.x) < 0.25f && std::fabs(acc.y) < 0.45f, "Ray keeps the remote level after T");
    In::setPosture(In::Posture::Pointing);
    rig.frames_(40);
}

// --- Game controllers ------------------------------------------------------

void padTap(In::PadButton button) {
    In::padButtonEvent(button, true);
    In::padButtonEvent(button, false);
}

void testGamepad() {
    using In::PadButton;
    using In::PadAxis;
    const In::Bindings d = In::Bindings::defaults();
    const struct {
        In::Action action;
        PadButton button;
        const char* what;
    } defaults[] = {
        {In::Action::A, PadButton::South, "bottom: A (jump)"},
        {In::Action::B, PadButton::East, "right: B (back)"},
        {In::Action::B, PadButton::RightTrigger, "RT: B (Star Bits)"},
        {In::Action::Shake, PadButton::West, "left: spin"},
        {In::Action::Shake, PadButton::RightShoulder, "RB: spin"},
        {In::Action::Start, PadButton::North, "top: Start (title A+B)"},
        {In::Action::NunchukZ, PadButton::LeftTrigger, "LT: Z"},
        {In::Action::NunchukC, PadButton::LeftShoulder, "LB: C"},
        {In::Action::DpadUp, PadButton::DpadUp, "D-pad up"},
        {In::Action::DpadLeft, PadButton::DpadLeft, "D-pad left"},
        {In::Action::Plus, PadButton::Start, "Start: pause"},
        {In::Action::Minus, PadButton::Back, "Back: Minus"},
        {In::Action::Home, PadButton::Guide, "Guide: Home"},
    };
    for (const auto& e : defaults) {
        check(hasBinding(d, e.action, In::Binding::pad(e.button)), std::string("pad default ") + e.what);
    }
    check(!hasBinding(d, In::Action::A, In::Binding::pad(PadButton::Start)) &&
              !hasBinding(d, In::Action::B, In::Binding::pad(PadButton::Start)),
          "Start sends no A or B, which would block pausing");
    In::Bindings parsed;
    std::string error;
    check(parsed.parse("Shake=Pad:LeftStick,Key:F\n", &error) &&
              hasBinding(parsed, In::Action::Shake, In::Binding::pad(PadButton::LeftStick)),
          "controller buttons remap by name: " + error);
    check(!parsed.parse("A=Pad:Nope\n", &error), "unknown controller button is an error");

    Rig rig;
    WPadButton& button = *rig.pad->mButton;
    const WPadStick& stick = *rig.pad->mStick;

    // Buttons.
    In::padButtonEvent(PadButton::South, true);
    rig.frame();
    check(button.testTriggerA(), "pad bottom: A");
    In::padButtonEvent(PadButton::South, false);
    rig.frames_(2);
    check(!button.testButtonA(), "released");
    padTap(PadButton::LeftShoulder);
    rig.frame();
    check(button.testTriggerC(), "LB tap: C");
    rig.frames_(2);

    // Triggers: past half travel, with hysteresis.
    In::padAxisEvent(PadAxis::RightTrigger, 0.6f);
    rig.frame();
    check(button.testTriggerB(), "RT pulled: B (Star Bits)");
    In::padAxisEvent(PadAxis::RightTrigger, 0.5f);
    rig.frames_(2);
    check(button.testButtonB(), "RT at half: still held");
    In::padAxisEvent(PadAxis::RightTrigger, 0.4f);
    rig.frames_(2);
    check(!button.testButtonB(), "RT eased off: released");
    In::padAxisEvent(PadAxis::RightTrigger, 0.0f);

    // Left stick: the Nunchuk stick, analog, with a radial dead zone.
    In::padAxisEvent(PadAxis::LeftX, 0.1f);
    In::padAxisEvent(PadAxis::LeftY, -0.1f);
    rig.frames_(2);
    check(stick.mStick.x == 0.0f && stick.mStick.y == 0.0f, "inside the dead zone: neutral");
    In::padAxisEvent(PadAxis::LeftX, 0.0f);
    In::padAxisEvent(PadAxis::LeftY, -1.0f);  // pushed forward (SDL y down)
    rig.frames_(2);
    check(stick.mStick.y > 0.97f && std::fabs(stick.mStick.x) < 0.03f, "full forward: stick up");
    In::padAxisEvent(PadAxis::LeftY, -0.6f);
    rig.frames_(2);
    check(near(stick.mStick.y, 0.5f, 0.03f), "60% push past a 20% dead zone: half stick (" + std::to_string(stick.mStick.y) + ")");
    press(In::Key::D);
    rig.frames_(2);
    check(stick.mStick.x > 0.97f && std::fabs(stick.mStick.y) < 0.03f, "a stick key overrides the controller stick");
    lift(In::Key::D);
    In::padAxisEvent(PadAxis::LeftY, 0.0f);
    rig.frames_(2);
    check(stick.mStick.y == 0.0f, "stick released");

    // Right stick: the Star Pointer, taking over from the mouse.
    In::mouseMoved(640.0f, 360.0f);
    rig.frames_(10);
    check(rig.latest().dpd_valid_fg == 2 && near(rig.latest().pos.x, 0.0f, 0.02f), "mouse pointer at the centre");
    In::padAxisEvent(PadAxis::RightX, 1.0f);
    rig.frames_(15);  // 0.25 s at 0.9 widths per second: 0.45 of the 2-unit image
    In::padAxisEvent(PadAxis::RightX, 0.0f);
    rig.frames_(10);
    check(near(rig.latest().pos.x, 0.45f, 0.06f) && near(rig.latest().pos.y, 0.0f, 0.03f),
          "right stick moves the pointer right (" + std::to_string(rig.latest().pos.x) + ")");
    In::padAxisEvent(PadAxis::RightY, -1.0f);
    rig.frames_(8);
    In::padAxisEvent(PadAxis::RightY, 0.0f);
    rig.frames_(10);
    check(rig.latest().pos.y < -0.15f, "right stick up moves the pointer up");
    In::padAxisEvent(PadAxis::RightX, 1.0f);
    rig.frames_(120);
    In::padAxisEvent(PadAxis::RightX, 0.0f);
    rig.frames_(10);
    check(rig.latest().dpd_valid_fg == 2 && rig.latest().pos.x > 0.9f && rig.latest().pos.x <= 1.02f,
          "the pointer stops at the image edge");
    padTap(PadButton::RightStick);
    rig.frames_(12);
    check(near(rig.latest().pos.x, 0.0f, 0.03f) && near(rig.latest().pos.y, 0.0f, 0.03f), "R3 recenters the pointer");
    In::mouseMoved(960.0f, 180.0f);
    rig.frames_(12);
    check(near(rig.latest().pos.x, 0.5f, 0.02f) && near(rig.latest().pos.y, -0.5f, 0.02f), "moving the mouse takes the pointer back");

    // Focus loss and disconnection release everything.
    In::padButtonEvent(PadButton::West, true);
    In::padButtonEvent(PadButton::South, true);
    In::padAxisEvent(PadAxis::LeftX, 1.0f);
    rig.frame();
    In::focusChanged(false);
    rig.frames_(3);
    check(rig.hold() == 0 && stick.mStick.x == 0.0f, "focus loss releases the controller");
    In::padButtonEvent(PadButton::South, true);
    rig.frames_(2);
    check(!button.testButtonA(), "controller ignored while unfocused");
    In::focusChanged(true);
    In::padButtonEvent(PadButton::South, false);
    In::padButtonEvent(PadButton::West, false);
    In::padButtonEvent(PadButton::South, true);
    In::padAxisEvent(PadAxis::LeftX, 1.0f);
    rig.frames_(3);
    check(button.testButtonA() && stick.mStick.x > 0.9f, "refocused: works again");
    In::padDisconnected();
    rig.frames_(3);
    check(!button.testButtonA() && stick.mStick.x == 0.0f, "disconnecting releases the controller");
    rig.frames_(40);

    // Rides: the left stick tilts, analog.
    In::padAxisEvent(PadAxis::LeftX, 1.0f);
    for (int f = 0; f < 40; ++f) {
        In::motionControlShown(In::Steering::Ball);
        rig.frame();
    }
    TVec3f acc;
    float xy;
    float yz;
    rig.pad->getAcceleration(&acc, WPAD_DEV_CORE);
    sphereAngles(acc, &xy, &yz);
    check(xy > 0.7f && stick.mStick.x == 0.0f, "Star Ball: the left stick rolls it (" + std::to_string(xy) + ")");
    In::padAxisEvent(PadAxis::LeftX, 0.0f);
    rig.frames_(40);

    // The title: the top button starts it.
    press(In::Key::Space);
    lift(In::Key::Space);
    rig.frames_(10);
    TriggerChecker a;
    TriggerChecker b;
    In::titlePromptShown();
    rig.frame();
    In::titlePromptShown();
    In::padButtonEvent(PadButton::North, true);
    rig.frame();
    In::titlePromptShown();
    a.update(button.testButtonA());
    b.update(button.testButtonB());
    check(a.getLevel() && b.getLevel(), "pad top on the title prompt: A and B");
    In::padButtonEvent(PadButton::North, false);
    rig.frames_(12);
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

// The KPAD thread changes the pointer calibration (KPADInit after WPADInit
// has started the reports; KPADSetSensorHeight) without the interrupt lock,
// while the alarm's report tick places the IR dots. The tick must use the
// copy taken in KPADRead's WPADProbe, not read inside_kpads: under TSan this
// test reported a data race in every run when it did.
//
// It runs before the remote connects (200 ms), while no KPAD code runs in the
// tick. Once connected, KPAD's own sampling callback shares inside_kpads with
// KPADRead without a lock, as the SDK did with an interrupt; that is not
// what this checks.
void testCalibrationThreads() {
    In::resetForTesting();
    PSC::reset();
    KPADInit();
    In::setViewport(In::Viewport::letterbox(1280.0f, 720.0f, 16.0f / 9.0f));
    In::mouseMoved(640.0f, 180.0f);
    KPADStatus statuses[120];
    int changes = 0;
    // A host sleep, not OSSleepTicks: that takes the interrupt lock, which
    // would order the write before the next tick and hide the race.
    while (changes < 8 && WPADProbe(0, nullptr) == WPAD_ERR_NO_CONTROLLER) {
        KPADSetSensorHeight(0, (changes % 2) != 0 ? -0.15f : 0.15f);
        ++changes;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        KPADRead(0, statuses, 120);
    }
    check(changes > 0, "sensor height changed while the reports run");
    In::resetForTesting();
}

// --- Screen saver ----------------------------------------------------------

// Retraces from a host thread (an OS thread cannot raise one), one per game
// frame, after that frame's reports: VI's dimming counts idle retraces and a
// VIResetDimmingCount takes effect at the next one.
void retraces(int count) {
    std::thread source([count] {
        for (int i = 0; i < count; ++i) {
            PVI::signalRetrace();
        }
    });
    source.join();
}

// Runs `seconds` of 60 Hz frames in 2-second steps, calling `act` at the
// start of each step; true if the display was never dimmed.
bool neverDims(Rig& rig, int seconds, const std::function<void(int)>& act) {
    bool dimmed = false;
    for (int step = 0; step < seconds / 2; ++step) {
        act(step);
        rig.frames_(120);
        retraces(120);
        dimmed = dimmed || PVI::displayState().dimmed;
    }
    return !dimmed;
}

void testScreenSaver() {
    PSC::reset();  // screen saver on
    PVI::setClock(PVI::Clock::External);
    VIInit();
    check(VIEnableDimming(TRUE) == TRUE && VIGetDimmingCount() > 0, "dimming on, as the game leaves it in play");
    const int sixMinutes = 6 * 60;

    // Each kind of input alone keeps the screen lit through six minutes.
    {
        Rig rig;
        VIResetDimmingCount();
        check(neverDims(rig, sixMinutes, [](int) { press(In::Key::Space); lift(In::Key::Space); }),
              "6 minutes of Space taps: never dims");
    }
    {
        Rig rig;
        VIResetDimmingCount();
        press(In::Key::W);  // held: the report stops changing, the OS repeats
        check(neverDims(rig, sixMinutes, [](int) { In::keyEvent(In::Key::W, true, true); }),
              "6 minutes holding W (OS key repeats): never dims");
        lift(In::Key::W);
    }
    {
        Rig rig;
        VIResetDimmingCount();
        check(neverDims(rig, sixMinutes, [](int step) { In::mouseMoved(400.0f + (step % 2) * 40.0f, 300.0f); }),
              "6 minutes of mouse motion over the game: never dims");
    }
    {
        Rig rig;
        VIResetDimmingCount();
        // 1280x720 letterboxed to 16:9 has no bars; use a 4:3 window's bars.
        In::setViewport(In::Viewport::letterbox(1280.0f, 960.0f, 16.0f / 9.0f));
        check(neverDims(rig, sixMinutes, [](int step) { In::mouseMoved(640.0f + (step % 2) * 40.0f, 10.0f); }),
              "6 minutes of mouse motion over the letterbox bar: never dims");
        check(rig.latest().dpd_valid_fg == 0, "over the bar the pointer is off the image: only host events count");
    }
    {
        Rig rig;
        VIResetDimmingCount();
        check(neverDims(rig, sixMinutes, [](int step) { In::keyEvent(In::Key::G, (step % 2) == 0, false); }),
              "6 minutes of an unbound key: never dims");
        lift(In::Key::G);
    }

    // A changing report alone counts, as on the Wii (WPADiCheckContInputs):
    // no host event, the remote turns upright.
    {
        Rig rig;
        rig.frames_(2);
        VIResetDimmingCount();
        retraces(1);
        for (int second = 0; second < 240; second += 2) {
            rig.frames_(120);
            retraces(120);
        }
        check(VIGetDimmingCount() < 18000 - 239 * 60, "four idle minutes counted");
        In::setPosture(In::Posture::Upright);
        rig.frames_(30);
        retraces(30);
        check(VIGetDimmingCount() > 18000 - 60, "the remote turning resets the count (" +
                                                    std::to_string(VIGetDimmingCount()) + " retraces left)");
        In::setPosture(In::Posture::Pointing);
        rig.frames_(30);
    }

    // Six idle minutes: dims after five, as on the Wii; any input undims at
    // the next retrace.
    {
        Rig rig;
        rig.frames_(2);
        VIResetDimmingCount();
        retraces(1);
        int dimmedAt = -1;
        for (int second = 0; second < sixMinutes && dimmedAt < 0; second += 2) {
            rig.frames_(120);
            retraces(120);
            if (PVI::displayState().dimmed) {
                dimmedAt = second + 2;
            }
        }
        check(dimmedAt >= 300 && dimmedAt <= 302, "idle: dims after 5 minutes (" + std::to_string(dimmedAt) + " s)");
        press(In::Key::Space);
        rig.frame();
        retraces(1);
        check(!PVI::displayState().dimmed, "a key press undims at the next retrace");
        lift(In::Key::Space);
        rig.frames_(2);
    }
    VIEnableDimming(FALSE);
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
    testControlsSummary();
    testConnection();
    testButtons();
    testStart();
    testStick();
    testFocusLoss();
    testPointer();
    testShake();
    testJumpThenSpin();
    testRapidShake();
    testTilt();
    testPauseTap();
    testSteering();
    testGamepad();
    testDevice();
#ifdef PETARI_INPUT_TEST_SDL3
    testSdl3();
#endif
    testAlarmClock();
    testCalibrationThreads();
    testScreenSaver();
    std::printf("native input tests passed (%d checks)\n", checks);
    return 0;
}

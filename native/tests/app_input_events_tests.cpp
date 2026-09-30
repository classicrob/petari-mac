// Tests for the app's physical-input count (native/app/input_events.cpp,
// Events::physicalInputs), which the smoke uses to tell an unattended run from
// an assisted one: which SDL events count as gameplay input, that pointer
// motion and focus are counted apart, that every event still reaches the input
// layer unchanged, and that the smoke's own presses (Events::pressButton) are
// never counted.
//
// Built with input_events.cpp and native/input/bindings.cpp only: the input
// layer's sinks and SDL_GetScancodeName are recording fakes below, so no
// window, SDL library or game is needed.

#include <SDL3/SDL_events.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../app/host.hpp"
#include "petari/input.hpp"

namespace Input = PetariNative::Input;
namespace Events = PetariNative::App::Events;
namespace Smoke = PetariNative::App::Smoke;

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

// What reached the input layer, in order.
struct Delivered {
    enum Kind { Key, MouseButton, MouseMoved, MouseLeft, Focus, PadButton, PadAxis, PadRemoved } kind;
    unsigned code;
    bool down;
    bool repeat;
};
std::vector<Delivered> gDelivered;
Input::Bindings gBindings;

}  // namespace

// Recording fakes of the input layer's sinks (native/input/wpad_host.cpp).
namespace PetariNative::Input {
void setBindings(const Bindings& value) {
    gBindings = value;
}
Bindings bindings() {
    return gBindings;
}
void keyEvent(KeyCode code, bool down, bool repeat) {
    gDelivered.push_back({Delivered::Key, code, down, repeat});
}
void mouseButtonEvent(MouseButton button, bool down) {
    gDelivered.push_back({Delivered::MouseButton, static_cast<unsigned>(button), down, false});
}
void mouseMoved(float, float) {
    gDelivered.push_back({Delivered::MouseMoved, 0, false, false});
}
void mouseLeft() {
    gDelivered.push_back({Delivered::MouseLeft, 0, false, false});
}
void padButtonEvent(PadButton button, bool down) {
    gDelivered.push_back({Delivered::PadButton, static_cast<unsigned>(button), down, false});
}
void padAxisEvent(PadAxis axis, float value) {
    gDelivered.push_back({Delivered::PadAxis, static_cast<unsigned>(axis), value > 0.0f, false});
}
void padDisconnected() {
    gDelivered.push_back({Delivered::PadRemoved, 0, false, false});
}
void setViewport(const Viewport&) {}
void focusChanged(bool focused) {
    gDelivered.push_back({Delivered::Focus, 0, focused, false});
}
}  // namespace PetariNative::Input

extern "C" const char* SDL_GetScancodeName(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_W:
        return "W";
    case SDL_SCANCODE_SPACE:
        return "Space";
    case SDL_SCANCODE_F9:
        return "F9";
    default:
        return "";
    }
}

namespace {

SDL_Event key(SDL_Scancode scancode, bool down, bool repeat = false) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scancode;
    event.key.down = down;
    event.key.repeat = repeat;
    return event;
}

SDL_Event mouseButton(Uint8 button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.down = down;
    return event;
}

SDL_Event padButton(Uint8 button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.button = button;
    event.gbutton.down = down;
    return event;
}

SDL_Event padAxis(Uint8 axis, Sint16 value) {
    SDL_Event event{};
    event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.axis = axis;
    event.gaxis.value = value;
    return event;
}

SDL_Event ofType(Uint32 type) {
    SDL_Event event{};
    event.type = type;
    return event;
}

std::string last() {
    const Smoke::PhysicalInputs now = Events::physicalInputs();
    return std::string(now.last, strnlen(now.last, sizeof(now.last)));
}

}  // namespace

int main() {
    // Known bindings: W is the stick, Space is A, the left mouse button is B.
    Input::Bindings bindings;
    bindings.bind(Input::Action::StickUp, Input::Binding::key(Input::Key::W));
    bindings.bind(Input::Action::A, Input::Binding::key(Input::Key::Space));
    bindings.bind(Input::Action::B, Input::Binding::mouse(Input::MouseButton::Left));
    Input::setBindings(bindings);

    Smoke::PhysicalInputs now = Events::physicalInputs();
    check(now.gameplay == 0 && now.pointer == 0 && now.focus == 0 && last().empty(), "nothing counted at start");

    // A bound key: the press and the release count, the repeat does not.
    Events::input(key(SDL_SCANCODE_W, true));
    check(Events::physicalInputs().gameplay == 1 && last() == "key W (StickUp) down", "bound key press: " + last());
    Events::input(key(SDL_SCANCODE_W, true, true));
    check(Events::physicalInputs().gameplay == 1, "key repeat not counted");
    Events::input(key(SDL_SCANCODE_W, false));
    check(Events::physicalInputs().gameplay == 2 && last() == "key W (StickUp) up", "bound key release: " + last());

    // An unbound key is not gameplay input.
    Events::input(key(SDL_SCANCODE_F9, true));
    Events::input(key(SDL_SCANCODE_F9, false));
    check(Events::physicalInputs().gameplay == 2 && last() == "key W (StickUp) up", "unbound key not counted");

    // A bound mouse button counts both ways; an unbound one does not.
    Events::input(mouseButton(SDL_BUTTON_LEFT, true));
    check(Events::physicalInputs().gameplay == 3 && last() == "mouse Left (B) down", "bound mouse button: " + last());
    Events::input(mouseButton(SDL_BUTTON_LEFT, false));
    check(Events::physicalInputs().gameplay == 4 && last() == "mouse Left (B) up", "bound mouse release: " + last());
    Events::input(mouseButton(SDL_BUTTON_RIGHT, true));
    Events::input(mouseButton(SDL_BUTTON_RIGHT, false));
    check(Events::physicalInputs().gameplay == 4, "unbound mouse button not counted");

    // Pointer motion and focus changes have their own counts.
    Events::input(ofType(SDL_EVENT_MOUSE_MOTION));
    Events::input(ofType(SDL_EVENT_MOUSE_MOTION));
    Events::input(ofType(SDL_EVENT_WINDOW_FOCUS_LOST));
    Events::input(ofType(SDL_EVENT_WINDOW_FOCUS_GAINED));
    now = Events::physicalInputs();
    check(now.gameplay == 4 && now.pointer == 2 && now.focus == 2, "motion and focus counted apart from gameplay");

    // Every event above reached the input layer unchanged, counted or not.
    const std::vector<Delivered> expected = {
        {Delivered::Key, Input::Key::W, true, false},
        {Delivered::Key, Input::Key::W, true, true},
        {Delivered::Key, Input::Key::W, false, false},
        {Delivered::Key, Input::Key::F9, true, false},
        {Delivered::Key, Input::Key::F9, false, false},
        {Delivered::MouseMoved, 0, false, false},
        {Delivered::MouseButton, static_cast<unsigned>(Input::MouseButton::Left), true, false},
        {Delivered::MouseMoved, 0, false, false},
        {Delivered::MouseButton, static_cast<unsigned>(Input::MouseButton::Left), false, false},
        {Delivered::MouseMoved, 0, false, false},
        {Delivered::MouseButton, static_cast<unsigned>(Input::MouseButton::Right), true, false},
        {Delivered::MouseMoved, 0, false, false},
        {Delivered::MouseButton, static_cast<unsigned>(Input::MouseButton::Right), false, false},
        {Delivered::MouseMoved, 0, false, false},
        {Delivered::MouseMoved, 0, false, false},
        {Delivered::Focus, 0, false, false},
        {Delivered::Focus, 0, true, false},
    };
    bool same = gDelivered.size() == expected.size();
    for (size_t i = 0; same && i < expected.size(); i++) {
        same = gDelivered[i].kind == expected[i].kind && gDelivered[i].code == expected[i].code &&
               gDelivered[i].down == expected[i].down && gDelivered[i].repeat == expected[i].repeat;
    }
    check(same, "every event delivered unchanged (" + std::to_string(gDelivered.size()) + " calls)");

    // The smoke's own presses go to the input layer directly and never count.
    gDelivered.clear();
    const Smoke::PhysicalInputs before = Events::physicalInputs();
    Events::assertFocus();
    Events::pressButton(0, true);   // A
    Events::pressButton(0, false);
    Events::pressButton(2, true);   // stick up
    Events::pressButton(2, false);
    Events::movePointer(10.0f, 20.0f);
    now = Events::physicalInputs();
    check(gDelivered.size() == 6 && gDelivered[1].kind == Delivered::Key && gDelivered[1].code == Input::Key::Space &&
              gDelivered[3].code == Input::Key::W,
          "smoke presses reach the input layer through the bindings");
    check(now.gameplay == before.gameplay && now.pointer == before.pointer && now.focus == before.focus &&
              std::strcmp(now.last, before.last) == 0,
          "smoke presses, pointer and focus are not physical input");

    // Game controllers: a bound button counts both ways, an unbound one does
    // not; a stick counts only past half travel (drift is not a player), and
    // the right stick is pointer motion.
    bindings.bind(Input::Action::A, Input::Binding::pad(Input::PadButton::South));
    Input::setBindings(bindings);
    gDelivered.clear();
    const Smoke::PhysicalInputs padBefore = Events::physicalInputs();
    Events::input(padButton(SDL_GAMEPAD_BUTTON_SOUTH, true));
    check(Events::physicalInputs().gameplay == padBefore.gameplay + 1 && last() == "pad Pad bottom (A) down",
          "bound pad button: " + last());
    Events::input(padButton(SDL_GAMEPAD_BUTTON_SOUTH, false));
    Events::input(padButton(SDL_GAMEPAD_BUTTON_GUIDE, true));
    Events::input(padButton(SDL_GAMEPAD_BUTTON_GUIDE, false));
    check(Events::physicalInputs().gameplay == padBefore.gameplay + 2, "unbound pad button not counted");
    Events::input(padAxis(SDL_GAMEPAD_AXIS_LEFTX, 3000));
    check(Events::physicalInputs().gameplay == padBefore.gameplay + 2, "stick drift not counted");
    Events::input(padAxis(SDL_GAMEPAD_AXIS_LEFTX, 30000));
    check(Events::physicalInputs().gameplay == padBefore.gameplay + 3, "a pushed stick counts");
    Events::input(padAxis(SDL_GAMEPAD_AXIS_RIGHTY, -30000));
    check(Events::physicalInputs().pointer == padBefore.pointer + 1 &&
              Events::physicalInputs().gameplay == padBefore.gameplay + 3,
          "the right stick is pointer motion");
    Events::input(ofType(SDL_EVENT_GAMEPAD_REMOVED));
    check(gDelivered.size() == 8 && gDelivered[0].kind == Delivered::PadButton &&
              gDelivered[0].code == static_cast<unsigned>(Input::PadButton::South) && gDelivered[0].down &&
              gDelivered[5].kind == Delivered::PadAxis && gDelivered[5].down &&
              gDelivered[6].code == static_cast<unsigned>(Input::PadAxis::RightY) && !gDelivered[6].down &&
              gDelivered[7].kind == Delivered::PadRemoved,
          "pad events delivered unchanged (" + std::to_string(gDelivered.size()) + " calls)");

    std::printf("native app input events tests passed (%d checks)\n", checks);
    return 0;
}

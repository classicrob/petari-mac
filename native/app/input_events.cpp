// Window events and remapped controls for the native input layer.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>

#include <cstdio>
#include <fstream>
#include <sstream>

#include "host.hpp"
#include "petari/input.hpp"
#include "petari/input_sdl3.hpp"

namespace PetariNative::App::Events {

bool loadControls(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return true;  // defaults
    }
    std::ifstream in(file);
    if (!in) {
        *error = "cannot read " + file.string();
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    Input::Bindings bindings = Input::Bindings::defaults();
    std::string parseError;
    if (!bindings.parse(text.str(), &parseError)) {
        *error = file.string() + ": " + parseError;
        return false;
    }
    Input::setBindings(bindings);
    return true;
}

namespace {

Smoke::PhysicalInputs gPhysical;

// The first game action the input is bound to, or null.
const char* boundAction(const Input::Binding& input) {
    const Input::Bindings bindings = Input::bindings();
    for (int a = 0; a < static_cast<int>(Input::Action::Count); ++a) {
        const Input::Action action = static_cast<Input::Action>(a);
        for (const Input::Binding& bound : bindings.inputs(action)) {
            if (bound == input) {
                return Input::actionName(action);
            }
        }
    }
    return nullptr;
}

void countGameplay(const char* device, const char* name, const char* action, bool down) {
    ++gPhysical.gameplay;
    std::snprintf(gPhysical.last, sizeof(gPhysical.last), "%s %s (%s) %s", device, name, action,
                  down ? "down" : "up");
}

void countPhysical(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event.key.repeat) {
            return;
        }
        const char* action = boundAction(Input::Binding::key(static_cast<Input::KeyCode>(event.key.scancode)));
        if (action != nullptr) {
            const char* name = SDL_GetScancodeName(event.key.scancode);
            countGameplay("key", name != nullptr && name[0] != '\0' ? name : "?", action, event.key.down);
        }
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        // The buttons Input::SDL3::handleEvent maps.
        static const struct {
            Uint8 sdl;
            Input::MouseButton button;
            const char* name;
        } kButtons[] = {{SDL_BUTTON_LEFT, Input::MouseButton::Left, "Left"},
                        {SDL_BUTTON_MIDDLE, Input::MouseButton::Middle, "Middle"},
                        {SDL_BUTTON_RIGHT, Input::MouseButton::Right, "Right"},
                        {SDL_BUTTON_X1, Input::MouseButton::X1, "X1"},
                        {SDL_BUTTON_X2, Input::MouseButton::X2, "X2"}};
        for (const auto& b : kButtons) {
            if (b.sdl == event.button.button) {
                const char* action = boundAction(Input::Binding::mouse(b.button));
                if (action != nullptr) {
                    countGameplay("mouse", b.name, action, event.button.down);
                }
                return;
            }
        }
        return;
    }
    case SDL_EVENT_MOUSE_MOTION:
        ++gPhysical.pointer;
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ++gPhysical.focus;
        return;
    default:
        return;
    }
}

}  // namespace

bool input(const SDL_Event& event) {
    countPhysical(event);
    return Input::SDL3::handleEvent(event);
}

Smoke::PhysicalInputs physicalInputs() {
    return gPhysical;
}

void setImage(const Rect& image, float windowWidth, float windowHeight) {
    Input::Viewport viewport;
    viewport.windowWidth = windowWidth;
    viewport.windowHeight = windowHeight;
    viewport.imageX = image.x;
    viewport.imageY = image.y;
    viewport.imageWidth = image.width;
    viewport.imageHeight = image.height;
    Input::setViewport(viewport);
}

void pressButton(int button, bool down) {
    static const Input::Action kActions[] = {Input::Action::A,         Input::Action::B,     Input::Action::StickUp,
                                             Input::Action::StickDown, Input::Action::Plus,  Input::Action::Minus,
                                             Input::Action::StickLeft, Input::Action::StickRight};
    if (button < 0 || button >= static_cast<int>(sizeof(kActions) / sizeof(kActions[0]))) {
        return;
    }
    const Input::Action action = kActions[button];
    const Input::Bindings bindings = Input::bindings();
    const auto& inputs = bindings.inputs(action);
    if (inputs.empty()) {
        std::fprintf(stderr, "PETARI SMOKE: no input is bound to %s\n", Input::actionName(action));
        return;
    }
    const Input::Binding& input = inputs.front();
    if (input.device == Input::Binding::Device::Key) {
        Input::keyEvent(input.code, down, false);
    } else {
        Input::mouseButtonEvent(static_cast<Input::MouseButton>(input.code), down);
    }
}

void assertFocus() {
    Input::focusChanged(true);
}

void movePointer(float x, float y) {
    Input::mouseMoved(x, y);
}

}  // namespace PetariNative::App::Events

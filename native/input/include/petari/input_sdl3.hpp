#pragma once
// Feeds SDL3 events to the native input layer. Header-only: it reads SDL's
// event structures and calls no SDL functions, so it needs SDL3's headers
// but adds no link dependency (target petari_input_sdl3).
//
// The application still tells the input layer where the game image is drawn
// (PetariNative::Input::setViewport) whenever the window or its letterboxing
// changes; SDL does not know the image rectangle.

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>

#include "petari/input.hpp"

namespace PetariNative::Input::SDL3 {

// Returns true for events the input layer consumed.
inline bool handleEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        keyEvent(static_cast<KeyCode>(event.key.scancode), event.key.down, event.key.repeat);
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        mouseMoved(event.motion.x, event.motion.y);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        MouseButton button;
        switch (event.button.button) {
        case SDL_BUTTON_LEFT: button = MouseButton::Left; break;
        case SDL_BUTTON_MIDDLE: button = MouseButton::Middle; break;
        case SDL_BUTTON_RIGHT: button = MouseButton::Right; break;
        case SDL_BUTTON_X1: button = MouseButton::X1; break;
        case SDL_BUTTON_X2: button = MouseButton::X2; break;
        default: return false;
        }
        mouseMoved(event.button.x, event.button.y);
        mouseButtonEvent(button, event.button.down);
        return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        mouseLeft();
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focusChanged(true);
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focusChanged(false);
        return true;
    default:
        return false;
    }
}

}  // namespace PetariNative::Input::SDL3

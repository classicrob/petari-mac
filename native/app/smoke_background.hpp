#pragma once

#include <SDL3/SDL_events.h>

namespace PetariNative::App::SmokeBackground {

inline bool enabled = false;

inline bool requested(const char* smoke, bool fixture, const char* overrideValue) {
    if (overrideValue && overrideValue[0]) return overrideValue[0] != '0';
    return fixture || (smoke && smoke[0] && smoke[0] != '0');
}

inline bool physicalEvent(Uint32 type) {
    return (type >= SDL_EVENT_KEY_DOWN && type <= SDL_EVENT_KEYMAP_CHANGED) ||
           (type >= SDL_EVENT_MOUSE_MOTION && type <= SDL_EVENT_MOUSE_WHEEL) ||
           (type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && type <= SDL_EVENT_JOYSTICK_UPDATE_COMPLETE) ||
           (type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && type <= SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED) ||
           (type >= SDL_EVENT_FINGER_DOWN && type <= SDL_EVENT_FINGER_CANCELED) ||
           type == SDL_EVENT_WINDOW_MOUSE_ENTER || type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
}

inline bool SDLCALL eventFilter(void*, SDL_Event* event) {
    return !enabled || !physicalEvent(event->type);
}

} // namespace PetariNative::App::SmokeBackground

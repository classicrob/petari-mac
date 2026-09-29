#pragma once
// Internal interfaces between the application's translation units. Only
// standard headers here: app_main.cpp and frame_seam.cpp include Aurora and
// SDL, platform_host.cpp includes the SDK, and neither side sees the other's
// headers (Aurora's dolphin/ GX headers and the SDK's revolution/ headers
// declare the same names).

#include <filesystem>
#include <string>

#include "petari/app.hpp"

union SDL_Event;
struct SDL_Window;

namespace PetariNative::App {

struct Paths {
    std::filesystem::path disc;  // extracted disc: contains files/ (and sys/)
    std::filesystem::path user;  // saves, settings, controls, crash reports
};

// --- platform_host.cpp (SDK side) ---
namespace Host {
// Before OSInit and the window: crash reports, disc, NAND, settings store.
bool preparePlatform(const Paths& paths, std::string* error);
// On the main thread, after the window exists: OSInit (binds this thread as
// the default OS thread), VI on its own retrace clock with the renderer
// hooks, the exit handler, and the audio sink.
void startOS();
// Runs the game. Never returns: the game leaves through the OS exit
// functions, which reach the exit handler.
[[noreturn]] void runGame();
// The window's close request, as the console's power button.
void requestQuit();
// A second close request while the game is still shutting down.
[[noreturn]] void forceQuit();
}  // namespace Host

// --- input_events.cpp ---
namespace Events {
// Loads remapped controls from a file written by Input::Bindings::serialize,
// if it exists. Before the game starts.
bool loadControls(const std::filesystem::path& file, std::string* error);
// SDL events for the input layer. Returns true if consumed.
bool input(const SDL_Event& event);
// Where the game image is, for the pointer. Window points.
void setImage(const Rect& image, float windowWidth, float windowHeight);
// Smoke run only (smoke.hpp): press or release the input bound to a Wii
// Remote/Nunchuk action, tell the input layer the window has focus (it ignores
// presses while unfocused), and move the pointer.
// button: 0 A, 1 B, 2 stick up, 3 stick down, 4 Plus, 5 Minus (Smoke::Button order).
void pressButton(int button, bool down);
void assertFocus();
// Moves the pointer to a window point.
void movePointer(float x, float y);
}  // namespace Events

// --- frame_seam.cpp ---
namespace Seam {
// The window the seam reads its size from. Before openFirstFrame.
void attach(SDL_Window* window);
// Opens the Aurora frame the game's first GX commands go into. Main thread,
// after Host::startOS and before Host::runGame.
void openFirstFrame();
}  // namespace Seam

}  // namespace PetariNative::App

#pragma once
// Native keyboard and mouse input behind the SDK's WPAD/KPAD API.
//
// Game code keeps calling <revolution/wpad.h> and <revolution/kpad.h>. The
// input library emulates WPAD for a virtual Wii Remote with a Nunchuk on
// channel 0, and compiles the SDK's own KPAD.c on top of it, so KPAD's pointer,
// accelerometer, and button-repeat processing are the original code.
//
// The virtual remote produces WPAD reports at 200 Hz, the Wii rate:
// - buttons and the Nunchuk stick from remappable bindings;
// - two sensor-bar dots in the IR camera image, placed so that KPAD reports
//   the Star Pointer where the mouse is over the game image;
// - accelerometer data from a remote orientation (tilt) and motion bursts
//   (a shake, which the game reads as a spin).
//
// Host events come in through the functions below, from any host thread. They
// identify keys by USB HID keyboard usage, which equals SDL3's SDL_Scancode.
// See input_sdl3.hpp in native/input for an SDL3 event adapter.

#include <cstdint>
#include <string>
#include <vector>

namespace PetariNative::Input {

// USB HID keyboard page (0x07) usages. Identical to SDL_Scancode values.
using KeyCode = std::uint16_t;
namespace Key {
constexpr KeyCode A = 4, B = 5, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11, I = 12, J = 13, K = 14, L = 15, M = 16, N = 17,
                  O = 18, P = 19, Q = 20, R = 21, S = 22, T = 23, U = 24, V = 25, W = 26, X = 27, Y = 28, Z = 29;
constexpr KeyCode Num1 = 30, Num2 = 31, Num3 = 32, Num4 = 33, Num5 = 34, Num6 = 35, Num7 = 36, Num8 = 37, Num9 = 38, Num0 = 39;
constexpr KeyCode Return = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44, Minus = 45, Equals = 46, LeftBracket = 47,
                  RightBracket = 48, Backslash = 49, Semicolon = 51, Apostrophe = 52, Grave = 53, Comma = 54, Period = 55, Slash = 56,
                  CapsLock = 57;
constexpr KeyCode F1 = 58, F2 = 59, F3 = 60, F4 = 61, F5 = 62, F6 = 63, F7 = 64, F8 = 65, F9 = 66, F10 = 67, F11 = 68, F12 = 69;
constexpr KeyCode Right = 79, Left = 80, Down = 81, Up = 82;
constexpr KeyCode LeftCtrl = 224, LeftShift = 225, LeftAlt = 226, LeftGui = 227, RightCtrl = 228, RightShift = 229, RightAlt = 230,
                  RightGui = 231;
constexpr KeyCode Max = 512;  // exclusive bound accepted by the API (SDL_SCANCODE_COUNT)
}  // namespace Key

enum class MouseButton : std::uint8_t { Left, Middle, Right, X1, X2, Count };

// What a binding does. Buttons are the Wii Remote's and the Nunchuk's.
enum class Action : std::uint8_t {
    StickUp,
    StickDown,
    StickLeft,
    StickRight,
    A,
    B,
    Plus,
    Minus,
    Home,
    One,
    Two,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    NunchukC,
    NunchukZ,
    Shake,           // flick the remote: the game reads a swing (Mario spins)
    TiltHold,        // while held, the stick keys tilt the remote instead of the stick
    PostureToggle,   // switch the remote between pointing at the screen and upright
    Count
};

struct Binding {
    enum class Device : std::uint8_t { Key, Mouse };
    Device device = Device::Key;
    std::uint16_t code = 0;  // KeyCode, or MouseButton

    static Binding key(KeyCode code) { return {Device::Key, code}; }
    static Binding mouse(MouseButton button) { return {Device::Mouse, static_cast<std::uint16_t>(button)}; }
    bool operator==(const Binding& other) const { return device == other.device && code == other.code; }
};

// Remappable bindings. Each action may have several inputs, and an input may
// drive several actions (Escape defaults to Plus and B).
class Bindings {
public:
    // The approved defaults from native/CONTROLS.md, plus provisional
    // bindings for the other buttons and tilt (see native/input/README.md).
    static Bindings defaults();

    void bind(Action action, Binding input);    // adds; no effect if present
    void unbind(Action action, Binding input);
    void clear(Action action);
    const std::vector<Binding>& inputs(Action action) const;

    // Text form, one "Action=Input,Input" line per action, for saving remaps.
    // Inputs are "Key:<name>" (see keyName) or "Mouse:<Left|Middle|Right|X1|X2>".
    std::string serialize() const;
    // Replaces bindings for the actions present in the text. Actions not
    // mentioned keep their current bindings. On error nothing changes.
    bool parse(const std::string& text, std::string* error);

private:
    std::vector<Binding> mInputs[static_cast<int>(Action::Count)];
};

const char* actionName(Action action);
// The constant names in Key ("W", "Num1", "LeftShift", ...), or "Usage<n>".
std::string keyName(KeyCode code);
bool parseKeyName(const std::string& name, KeyCode* code);

// Where the game image is drawn in the window, in the units of mouse events
// (SDL3: window coordinates). The Star Pointer spans the image; the rest of
// the window is letterboxing.
struct Viewport {
    float windowWidth = 0.0f;
    float windowHeight = 0.0f;
    float imageX = 0.0f;
    float imageY = 0.0f;
    float imageWidth = 0.0f;
    float imageHeight = 0.0f;

    // The largest image of the given aspect ratio (width / height) centered in
    // the window.
    static Viewport letterbox(float windowWidth, float windowHeight, float imageAspect);
};

// Posture of the remote at rest. Pointing: level and aimed at the screen (the
// pointer works). Upright: tip up, tilted 10 degrees toward the screen, as the
// Star Ball and Ray surfing instructions ask (the IR camera sees no sensor
// bar, as on the Wii).
enum class Posture : std::uint8_t { Pointing, Upright };

struct Settings {
    // Remote distance from the sensor bar in metres, reported as KPAD dist.
    float pointerDistance = 2.0f;
    // Largest tilt from the posture, in degrees, for the stick keys with
    // TiltHold. Ray surfing turns need a twist of at least 40.5 degrees
    // (SurfRayTutorial: 0.65 g across the remote).
    float maxTiltDegrees = 45.0f;
    // How fast the remote turns toward a new tilt, in degrees per second.
    float tiltRateDegreesPerSecond = 360.0f;
    // A button press or release lasts at least this many 5 ms reports, so a
    // quick tap spans a KPADRead at 60 frames per second (KPAD compares only
    // the newest report with the previous read).
    int minimumPulseReports = 4;
};

// --- Host events (any thread) ---

void setBindings(const Bindings& bindings);
Bindings bindings();
void setSettings(const Settings& settings);
Settings settings();

// repeat: the OS auto-repeat flag. Repeats are ignored; the game's KPAD
// button repeat applies instead.
void keyEvent(KeyCode code, bool down, bool repeat);
void mouseButtonEvent(MouseButton button, bool down);
// Mouse position in window coordinates.
void mouseMoved(float x, float y);
// The mouse left the window: the remote points away from the screen.
void mouseLeft();
void setViewport(const Viewport& viewport);
// Losing focus releases every held input and hides the pointer, so nothing
// stays pressed while the window is in the background.
void focusChanged(bool focused);
void setPosture(Posture posture);
Posture posture();

// --- Device state ---

// Connects or disconnects the virtual remote on a channel. Channel 0 is
// connected by WPADInit (after the Bluetooth reconnection delay) and
// reconnects when a bound input is pressed after WPADDisconnect.
void setConnected(int channel, bool connected);
void setNunchukAttached(int channel, bool attached);
// Rumble motor state last set by WPADControlMotor, for host haptics.
bool rumbleActive(int channel);

// Remote speaker. The game streams 4-bit ADPCM (WENC) packets. Without a sink
// they are consumed at the device rate and not played.
using SpeakerSink = void (*)(int channel, const std::uint8_t* data, std::uint16_t length, void* user);
void setSpeakerSink(SpeakerSink sink, void* user);

// --- Report clock ---

enum class Clock : std::uint8_t {
    Alarm,   // default: a 5 ms periodic OSAlarm, like WPAD's manage handler
    Manual,  // reports are produced only by pumpReports (tests, frame-locked hosts)
};
// Takes effect at WPADInit, or immediately if WPAD is running.
void setClock(Clock clock);
// Produces count reports, as count * 5 ms of device time. Runs callbacks with
// interrupts disabled, as the alarm does. Any thread.
void pumpReports(int count);

// Clears all state, bindings, settings, and the SDK layer's initialization.
// Tests only; WPAD must not be in use.
void resetForTesting();

}  // namespace PetariNative::Input

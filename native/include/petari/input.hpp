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
constexpr KeyCode KeypadEnter = 88;
constexpr KeyCode LeftCtrl = 224, LeftShift = 225, LeftAlt = 226, LeftGui = 227, RightCtrl = 228, RightShift = 229, RightAlt = 230,
                  RightGui = 231;
constexpr KeyCode Max = 512;  // exclusive bound accepted by the API (SDL_SCANCODE_COUNT)
}  // namespace Key

enum class MouseButton : std::uint8_t { Left, Middle, Right, X1, X2, Count };

// Game controller buttons, by position (SDL3's standard gamepad layout:
// South is Xbox A, PlayStation Cross, Nintendo B). The triggers are analog
// axes that act as buttons past half travel.
enum class PadButton : std::uint8_t {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,   // L3
    RightStick,  // R3: also recenters the controller's Star Pointer
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftTrigger,
    RightTrigger,
    Count
};
enum class PadAxis : std::uint8_t { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, Count };

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
    Walk,            // while held, the stick moves at Settings::walkStickScale
    Start,           // A; A and B together while the title asks for both (titlePromptShown)
    Count
};

struct Binding {
    enum class Device : std::uint8_t { Key, Mouse, Pad };
    Device device = Device::Key;
    std::uint16_t code = 0;  // KeyCode, MouseButton, or PadButton

    static Binding key(KeyCode code) { return {Device::Key, code}; }
    static Binding mouse(MouseButton button) { return {Device::Mouse, static_cast<std::uint16_t>(button)}; }
    static Binding pad(PadButton button) { return {Device::Pad, static_cast<std::uint16_t>(button)}; }
    bool operator==(const Binding& other) const { return device == other.device && code == other.code; }
};

// Remappable bindings. Each action may have several inputs, and an input may
// drive several actions.
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
    // Inputs are "Key:<name>" (see keyName), "Mouse:<Left|Middle|Right|X1|X2>"
    // or "Pad:<South|East|West|North|Back|Guide|Start|LeftStick|RightStick|
    // LeftShoulder|RightShoulder|DpadUp|DpadDown|DpadLeft|DpadRight|
    // LeftTrigger|RightTrigger>".
    std::string serialize() const;
    // Replaces bindings for the actions present in the text. Actions not
    // mentioned keep their current bindings. On error nothing changes.
    bool parse(const std::string& text, std::string* error);

private:
    std::vector<Binding> mInputs[static_cast<int>(Action::Count)];
};

const char* actionName(Action action);

// The player-facing controls list for a set of bindings (the Home menu's
// Controls page): one line per thing the player does, with the inputs bound
// to it as they appear on the keyboard ("Space / Right mouse"), or
// "(not bound)". Follows remaps. Allocates: on a game thread, call it inside
// a HostAllocationScope.
struct ControlsLine {
    std::string action;
    std::string inputs;  // keyboard and mouse
    std::string pad;     // game controller ("" if it has none)
};
std::vector<ControlsLine> controlsSummary(const Bindings& bindings);
// One line for the title screen, which asks for A and B together:
// "Keyboard: Return starts   |   F1: all controls", from the bindings (without
// a Start key: "hold Space, then press Backspace"). Allocates, like
// controlsSummary.
std::string titleHint(const Bindings& bindings);
// How an input appears to the player: "Space", "Left Shift", "Left mouse".
std::string displayName(Binding input);
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
    // Stick length while Walk is held (the keys otherwise give a full stick).
    float walkStickScale = 0.5f;
    // Mario ignores a swing that starts within 10 frames of pressing A or B
    // (MarioActor::updateControllerSwing). A keyboard shake that soon after a
    // jump is delayed until that window has passed, so jump-then-spin works.
    // Reports from the A or B press; 0 disables the delay.
    // Shake presses also start at least 250 ms apart (a flick must return
    // before the next, or the game sees one long swing). A press while the
    // previous flick is returning waits; at most one waits, and losing focus
    // cancels it.
    int shakeDelayAfterButtonReports = 36;
    // Plus and Minus presses last at least this many reports. The game pauses
    // only once Plus or Minus has been held for 12 frames
    // (PauseButtonCheckerInGame), so a tap of Escape would do nothing; 50
    // reports (250 ms) span 14 frames. The pause menu closes on a new press,
    // and its hold counter is not updated while paused, so the long press
    // cannot reopen it.
    int pauseTapReports = 50;
    // Game controller sticks: a radial dead zone (0..1 of full travel); past
    // it the left stick is the Nunchuk stick, analog.
    float padStickDeadZone = 0.2f;
    // The right stick moves the Star Pointer at up to this many image widths
    // per second (the image is 2 KPAD units wide).
    float padPointerWidthsPerSecond = 0.9f;
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
// Game controllers (all of them act as one). Axis values are -1..1 for the
// sticks, x right and y DOWN as SDL reports them, and 0..1 for the triggers.
void padButtonEvent(PadButton button, bool down);
void padAxisEvent(PadAxis axis, float value);
// The last controller was removed: everything it held is released.
void padDisconnected();
void setPosture(Posture posture);
Posture posture();

// Game side (native builds): the title screen is asking for A and B held
// together this frame (TitleSequenceProduct::exeLogoDisplay calls it every
// frame the prompt is up). For the next 100 ms the Start action (Return)
// presses B with A, so one key starts the game. Elsewhere Start is A alone,
// and never sends the B that backs out of menus. Any thread.
void titlePromptShown();
// Host side: the title prompt was reported within the last 100 ms (the
// native title hint shows while it is). Any thread.
bool titlePromptActive();
// Game side (native builds): the game steers by the remote's tilt this frame.
// For the next 100 ms the stick keys tilt the remote instead of moving the
// stick, as Tab would, so WASD steer with no extra keys. Any thread.
enum class Steering : std::uint8_t {
    Ball,  // Star Ball (SphereAccelSensorController): upright, WASD tilt
    Ray,   // Ray surfing (SurfRay::updateRide): level, A/D twist, W/S ignored
};
void motionControlShown(Steering steering);

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

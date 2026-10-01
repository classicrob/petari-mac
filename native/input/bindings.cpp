// Remappable bindings, their defaults, and their text form.

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <utility>

#include "petari/input.hpp"

namespace PetariNative::Input {
namespace {

constexpr int kActionCount = static_cast<int>(Action::Count);

const char* const kActionNames[kActionCount] = {
    "StickUp", "StickDown", "StickLeft", "StickRight", "A",        "B",        "Plus",     "Minus",     "Home",   "One",
    "Two",     "DpadUp",    "DpadDown",  "DpadLeft",   "DpadRight", "NunchukC", "NunchukZ", "Shake",     "TiltHold",
    "PostureToggle", "Walk",   "Start", "ModCollectStarBits", "ModShootEnemy", "CameraOrbitHold", "CameraZoomIn",
    "CameraZoomOut", "CameraOrbitLeft", "CameraOrbitRight", "CameraPitchUp", "CameraPitchDown",
};

const char* const kMouseNames[static_cast<int>(MouseButton::Count)] = {"Left", "Middle", "Right", "X1", "X2"};

const char* const kPadNames[static_cast<int>(PadButton::Count)] = {
    "South",        "East",          "West",   "North",    "Back",     "Guide",     "Start",       "LeftStick", "RightStick",
    "LeftShoulder", "RightShoulder", "DpadUp", "DpadDown", "DpadLeft", "DpadRight", "LeftTrigger", "RightTrigger"};

// Player-facing, layout neutral (South is Xbox A, PlayStation Cross, Nintendo B).
const char* const kPadDisplayNames[static_cast<int>(PadButton::Count)] = {
    "Pad bottom",   "Pad right",    "Pad left",      "Pad top",        "Pad Back",       "Pad Guide",
    "Pad Start",    "Pad L3",       "Pad R3",        "Pad LB",         "Pad RB",         "Pad D-pad up",
    "Pad D-pad down", "Pad D-pad left", "Pad D-pad right", "Pad LT", "Pad RT"};

struct NamedKey {
    const char* name;
    KeyCode code;
};

const NamedKey kKeyNames[] = {
    {"A", Key::A}, {"B", Key::B}, {"C", Key::C}, {"D", Key::D}, {"E", Key::E}, {"F", Key::F}, {"G", Key::G},
    {"H", Key::H}, {"I", Key::I}, {"J", Key::J}, {"K", Key::K}, {"L", Key::L}, {"M", Key::M}, {"N", Key::N},
    {"O", Key::O}, {"P", Key::P}, {"Q", Key::Q}, {"R", Key::R}, {"S", Key::S}, {"T", Key::T}, {"U", Key::U},
    {"V", Key::V}, {"W", Key::W}, {"X", Key::X}, {"Y", Key::Y}, {"Z", Key::Z},
    {"Num1", Key::Num1}, {"Num2", Key::Num2}, {"Num3", Key::Num3}, {"Num4", Key::Num4}, {"Num5", Key::Num5},
    {"Num6", Key::Num6}, {"Num7", Key::Num7}, {"Num8", Key::Num8}, {"Num9", Key::Num9}, {"Num0", Key::Num0},
    {"Return", Key::Return}, {"Escape", Key::Escape}, {"Backspace", Key::Backspace}, {"Tab", Key::Tab},
    {"Space", Key::Space}, {"Minus", Key::Minus}, {"Equals", Key::Equals}, {"LeftBracket", Key::LeftBracket},
    {"RightBracket", Key::RightBracket}, {"Backslash", Key::Backslash}, {"Semicolon", Key::Semicolon},
    {"Apostrophe", Key::Apostrophe}, {"Grave", Key::Grave}, {"Comma", Key::Comma}, {"Period", Key::Period},
    {"Slash", Key::Slash}, {"CapsLock", Key::CapsLock},
    {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4}, {"F5", Key::F5}, {"F6", Key::F6},
    {"F7", Key::F7}, {"F8", Key::F8}, {"F9", Key::F9}, {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12},
    {"KeypadEnter", Key::KeypadEnter},
    {"Right", Key::Right}, {"Left", Key::Left}, {"Down", Key::Down}, {"Up", Key::Up},
    {"LeftCtrl", Key::LeftCtrl}, {"LeftShift", Key::LeftShift}, {"LeftAlt", Key::LeftAlt}, {"LeftGui", Key::LeftGui},
    {"RightCtrl", Key::RightCtrl}, {"RightShift", Key::RightShift}, {"RightAlt", Key::RightAlt},
    {"RightGui", Key::RightGui},
};

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

bool parseAction(const std::string& name, Action* action) {
    for (int i = 0; i < kActionCount; ++i) {
        if (name == kActionNames[i]) {
            *action = static_cast<Action>(i);
            return true;
        }
    }
    return false;
}

bool parseBinding(const std::string& text, Binding* binding) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    const std::string device = text.substr(0, colon);
    const std::string name = text.substr(colon + 1);
    if (device == "Key") {
        KeyCode code;
        if (!parseKeyName(name, &code)) {
            return false;
        }
        *binding = Binding::key(code);
        return true;
    }
    if (device == "Mouse") {
        for (int i = 0; i < static_cast<int>(MouseButton::Count); ++i) {
            if (name == kMouseNames[i]) {
                *binding = Binding::mouse(static_cast<MouseButton>(i));
                return true;
            }
        }
    }
    if (device == "Pad") {
        for (int i = 0; i < static_cast<int>(PadButton::Count); ++i) {
            if (name == kPadNames[i]) {
                *binding = Binding::pad(static_cast<PadButton>(i));
                return true;
            }
        }
    }
    return false;
}

// Player-facing names where keyName's differ.
const NamedKey kDisplayNames[] = {
    {"Return", Key::Return},         {"Keypad Enter", Key::KeypadEnter}, {"Left Shift", Key::LeftShift},
    {"Right Shift", Key::RightShift}, {"Left Ctrl", Key::LeftCtrl},       {"Right Ctrl", Key::RightCtrl},
    {"Left Alt", Key::LeftAlt},       {"Right Alt", Key::RightAlt},       {"Left Cmd", Key::LeftGui},
    {"Right Cmd", Key::RightGui},     {"Up arrow", Key::Up},              {"Down arrow", Key::Down},
    {"Left arrow", Key::Left},        {"Right arrow", Key::Right},        {"-", Key::Minus},
    {"=", Key::Equals},               {"Caps Lock", Key::CapsLock},       {"`", Key::Grave},
};

const char* const kMouseDisplayNames[static_cast<int>(MouseButton::Count)] = {
    "Left mouse", "Middle mouse", "Right mouse", "Mouse button 4", "Mouse button 5"};

// The inputs bound to any of the actions, in order, without repeats.
// Controller buttons in the Controls page's own column: no "Pad " prefix.
const char* const kPadShortNames[static_cast<int>(PadButton::Count)] = {
    "bottom", "right", "left", "top", "Back", "Guide", "Start", "L3", "R3", "LB", "RB",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "LT", "RT"};

enum class Devices { KeyboardMouse, Pad };

bool isPad(const Binding& input) {
    return input.device == Binding::Device::Pad;
}

std::vector<Binding> inputsOf(const Bindings& bindings, std::initializer_list<Action> actions,
                              Devices devices = Devices::KeyboardMouse) {
    std::vector<Binding> result;
    for (const Action action : actions) {
        for (const Binding& input : bindings.inputs(action)) {
            if (isPad(input) != (devices == Devices::Pad)) {
                continue;
            }
            if (std::find(result.begin(), result.end(), input) == result.end()) {
                result.push_back(input);
            }
        }
    }
    return result;
}

std::string joined(const std::vector<Binding>& inputs, const char* separator) {
    if (inputs.empty()) {
        return "(not bound)";
    }
    std::string text;
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (i > 0) {
            text += separator;
        }
        // Left and right modifiers bound together read as one key.
        const Binding& input = inputs[i];
        if (input.device == Binding::Device::Key && i + 1 < inputs.size()) {
            const Binding& next = inputs[i + 1];
            const std::string name = displayName(input);
            const std::string nextName = displayName(next);
            if (name.rfind("Left ", 0) == 0 && nextName == "Right " + name.substr(5)) {
                text += name.substr(5);
                ++i;
                continue;
            }
        }
        text += isPad(input) && input.code < static_cast<int>(PadButton::Count) ? kPadShortNames[input.code]
                                                                                   : displayName(input);
    }
    return text;
}

// Controller buttons of the actions, or "" when none are bound.
std::string padText(const Bindings& b, std::initializer_list<Action> actions, const std::string& prefix = "",
                    const std::string& suffix = "") {
    const std::vector<Binding> inputs = inputsOf(b, actions, Devices::Pad);
    return inputs.empty() ? std::string() : prefix + joined(inputs, " / ") + suffix;
}

// The first keyboard or mouse input of each action, e.g. "W A S D". Actions
// without one show "-".
std::string firstOfEach(const Bindings& bindings, std::initializer_list<Action> actions, const char* separator) {
    std::string text;
    bool any = false;
    for (const Action action : actions) {
        if (!text.empty()) {
            text += separator;
        }
        const std::vector<Binding> inputs = inputsOf(bindings, {action});
        text += inputs.empty() ? "-" : displayName(inputs.front());
        any = any || !inputs.empty();
    }
    return any ? text : "(not bound)";
}

}  // namespace

std::string displayName(Binding input) {
    if (input.device == Binding::Device::Pad) {
        return input.code < static_cast<int>(PadButton::Count) ? kPadDisplayNames[input.code] : "Pad";
    }
    if (input.device == Binding::Device::Mouse) {
        return input.code < static_cast<int>(MouseButton::Count) ? kMouseDisplayNames[input.code] : "Mouse";
    }
    for (const NamedKey& key : kDisplayNames) {
        if (key.code == input.code) {
            return key.name;
        }
    }
    std::string name = keyName(input.code);
    if (name.size() == 4 && name.rfind("Num", 0) == 0) {
        return name.substr(3);  // Num1 -> 1
    }
    return name;
}

std::vector<ControlsLine> controlsSummary(const Bindings& b) {
    const std::string move = firstOfEach(b, {Action::StickUp, Action::StickLeft, Action::StickDown, Action::StickRight}, " ");
    // "Hold <inputs><suffix>", or "(not bound)".
    auto hold = [&b](std::initializer_list<Action> actions, const std::string& suffix) {
        const std::vector<Binding> inputs = inputsOf(b, actions);
        return inputs.empty() ? std::string("(not bound)") : "Hold " + joined(inputs, " / ") + suffix;
    };
    const std::string padLeave = padText(b, {Action::DpadDown});
    const std::string padRotate = padText(b, {Action::DpadLeft}) + (padText(b, {Action::DpadRight}).empty() ? "" : " / ") +
                                  padText(b, {Action::DpadRight});
    return {
        {"", "Keyboard and mouse", "Controller"},
        {"Move", move, "left stick"},
        {"Jump / confirm", joined(inputsOf(b, {Action::A}), " / "), padText(b, {Action::A})},
        {"Start (title: A and B)", joined(inputsOf(b, {Action::Start}), " / "), padText(b, {Action::Start})},
        {"Spin", joined(inputsOf(b, {Action::Shake}), " / "), padText(b, {Action::Shake})},
        {"Crouch / ground pound", joined(inputsOf(b, {Action::NunchukZ}), " / "), padText(b, {Action::NunchukZ})},
        {"Star Pointer", "Mouse", "right stick (R3: center)"},
        {"Shoot Star Bits / back", joined(inputsOf(b, {Action::B}), " / "), padText(b, {Action::B})},
        {"Grab (Pull Stars)", hold({Action::A}, " on the target"), padText(b, {Action::A}, "hold ")},
        {"Rotate camera", firstOfEach(b, {Action::DpadLeft, Action::DpadRight}, " / "), padRotate},
        {"Recenter camera", joined(inputsOf(b, {Action::NunchukC}), " / "), padText(b, {Action::NunchukC})},
        {"First-person view",
         joined(inputsOf(b, {Action::DpadUp}), " / ") + " (leave: " + joined(inputsOf(b, {Action::DpadDown}), " / ") + ")",
         padText(b, {Action::DpadUp}, "", padLeave.empty() ? "" : " (leave: " + padLeave + ")")},
        {"Walk slowly", hold({Action::Walk}, ""), "push the stick part way"},
        {"Pause", joined(inputsOf(b, {Action::Plus, Action::Minus}), " / "), padText(b, {Action::Plus, Action::Minus})},
        {"Star Ball / Ray", move + " tilt while riding", "left stick tilts while riding"},
        {"Tilt the remote by hand", hold({Action::TiltHold}, " + " + move), ""},
        {"This menu", joined(inputsOf(b, {Action::Home}), " / "), padText(b, {Action::Home})},
    };
}

std::string titleHint(const Bindings& b) {
    // The keyboard input of an action if it has one, else its first input.
    auto preferKey = [&b](Action action) {
        const std::vector<Binding>& inputs = b.inputs(action);
        for (const Binding& input : inputs) {
            if (input.device == Binding::Device::Key) {
                return displayName(input);
            }
        }
        return displayName(inputs.front());
    };
    std::string start;
    if (!b.inputs(Action::Start).empty()) {
        start = preferKey(Action::Start) + " starts";
    } else if (!b.inputs(Action::A).empty() && !b.inputs(Action::B).empty()) {
        start = "hold " + preferKey(Action::A) + ", then press " + preferKey(Action::B) + " to start";
    } else {
        start = "bind Start in controls.txt to start";
    }
    std::string text = "Keyboard: " + start;
    if (!b.inputs(Action::Home).empty()) {
        text += "   |   " + preferKey(Action::Home) + ": all controls";
    }
    return text;
}

Bindings Bindings::defaults() {
    Bindings b;
    // Approved (native/CONTROLS.md).
    b.bind(Action::StickUp, Binding::key(Key::W));
    b.bind(Action::StickDown, Binding::key(Key::S));
    b.bind(Action::StickLeft, Binding::key(Key::A));
    b.bind(Action::StickRight, Binding::key(Key::D));
    b.bind(Action::A, Binding::key(Key::Space));
    b.bind(Action::A, Binding::mouse(MouseButton::Right));  // Pull Stars (GCapture) and pointer grabs use A
    b.bind(Action::B, Binding::mouse(MouseButton::Left));   // Star Bits
    b.bind(Action::Shake, Binding::key(Key::F));
    b.bind(Action::NunchukZ, Binding::key(Key::LeftShift));
    b.bind(Action::NunchukZ, Binding::key(Key::RightShift));
    b.bind(Action::DpadLeft, Binding::key(Key::Q));   // camera rotation (CameraLocalUtil)
    b.bind(Action::DpadRight, Binding::key(Key::E));
    b.bind(Action::NunchukC, Binding::key(Key::C));   // camera recenter
    b.bind(Action::Plus, Binding::key(Key::Escape));  // pause
    b.bind(Action::B, Binding::key(Key::Backspace));  // back in menus
    // Provisional, pending gameplay review.
    b.bind(Action::DpadUp, Binding::key(Key::Up));  // first-person view
    b.bind(Action::DpadDown, Binding::key(Key::Down));
    b.bind(Action::DpadLeft, Binding::key(Key::Left));
    b.bind(Action::DpadRight, Binding::key(Key::Right));
    b.bind(Action::Minus, Binding::key(Key::Minus));
    b.bind(Action::One, Binding::key(Key::Num1));
    b.bind(Action::Two, Binding::key(Key::Num2));
    b.bind(Action::Home, Binding::key(Key::F1));
    b.bind(Action::Home, Binding::key(Key::Grave));  // ` (under Escape), for keyboards without a usable F1
    b.bind(Action::TiltHold, Binding::key(Key::Tab));
    b.bind(Action::PostureToggle, Binding::key(Key::T));
    b.bind(Action::Walk, Binding::key(Key::LeftAlt));
    // Return confirms (A). On the title's "press A and B" prompt it is A and
    // B together; it never sends B elsewhere, where B backs out of menus.
    b.bind(Action::Start, Binding::key(Key::Return));
    b.bind(Action::Start, Binding::key(Key::KeypadEnter));
    // Mods (off by default, native/MODS.md): keys the game does not use.
    b.bind(Action::ModCollectStarBits, Binding::key(Key::G));
    b.bind(Action::ModShootEnemy, Binding::key(Key::V));
    // Odyssey camera mod (off by default): hold-to-orbit for mice without a
    // middle button (Command), and zoom keys.
    b.bind(Action::CameraOrbitHold, Binding::mouse(MouseButton::Middle));
    b.bind(Action::CameraOrbitHold, Binding::key(Key::LeftGui));
    b.bind(Action::CameraZoomIn, Binding::key(Key::Z));
    b.bind(Action::CameraZoomOut, Binding::key(Key::X));
    b.bind(Action::CameraOrbitLeft, Binding::key(Key::J));
    b.bind(Action::CameraOrbitRight, Binding::key(Key::L));
    b.bind(Action::CameraPitchUp, Binding::key(Key::I));
    b.bind(Action::CameraPitchDown, Binding::key(Key::K));
    // Game controllers: the usual console layout for this game. The left
    // stick is the Nunchuk stick and the right stick the Star Pointer (not
    // bindings; see RemoteModel). Start and Back would block pausing if they
    // also sent A or B, so the title's A and B is the top button (Start).
    b.bind(Action::A, Binding::pad(PadButton::South));
    b.bind(Action::B, Binding::pad(PadButton::East));
    b.bind(Action::B, Binding::pad(PadButton::RightTrigger));  // Star Bits
    b.bind(Action::Shake, Binding::pad(PadButton::West));
    b.bind(Action::Shake, Binding::pad(PadButton::RightShoulder));
    b.bind(Action::Start, Binding::pad(PadButton::North));
    b.bind(Action::NunchukZ, Binding::pad(PadButton::LeftTrigger));
    b.bind(Action::NunchukC, Binding::pad(PadButton::LeftShoulder));
    b.bind(Action::DpadUp, Binding::pad(PadButton::DpadUp));
    b.bind(Action::DpadDown, Binding::pad(PadButton::DpadDown));
    b.bind(Action::DpadLeft, Binding::pad(PadButton::DpadLeft));
    b.bind(Action::DpadRight, Binding::pad(PadButton::DpadRight));
    b.bind(Action::Plus, Binding::pad(PadButton::Start));
    b.bind(Action::Minus, Binding::pad(PadButton::Back));
    b.bind(Action::Home, Binding::pad(PadButton::Guide));
    // The only controller button left free; ModShootEnemy has no controller default.
    b.bind(Action::ModCollectStarBits, Binding::pad(PadButton::LeftStick));
    return b;
}

void Bindings::bind(Action action, Binding input) {
    auto& inputs = mInputs[static_cast<int>(action)];
    if (std::find(inputs.begin(), inputs.end(), input) == inputs.end()) {
        inputs.push_back(input);
    }
}

void Bindings::unbind(Action action, Binding input) {
    auto& inputs = mInputs[static_cast<int>(action)];
    inputs.erase(std::remove(inputs.begin(), inputs.end(), input), inputs.end());
}

void Bindings::clear(Action action) {
    mInputs[static_cast<int>(action)].clear();
}

const std::vector<Binding>& Bindings::inputs(Action action) const {
    return mInputs[static_cast<int>(action)];
}

std::string Bindings::serialize() const {
    std::ostringstream out;
    for (int i = 0; i < kActionCount; ++i) {
        out << kActionNames[i] << '=';
        bool first = true;
        for (const Binding& input : mInputs[i]) {
            if (!first) {
                out << ',';
            }
            first = false;
            if (input.device == Binding::Device::Key) {
                out << "Key:" << keyName(input.code);
            } else if (input.device == Binding::Device::Mouse) {
                out << "Mouse:" << kMouseNames[input.code];
            } else {
                out << "Pad:" << kPadNames[input.code];
            }
        }
        out << '\n';
    }
    return out.str();
}

bool Bindings::parse(const std::string& text, std::string* error) {
    Bindings result = *this;
    std::istringstream in(text);
    std::string line;
    int lineNumber = 0;
    auto fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = "line " + std::to_string(lineNumber) + ": " + message;
        }
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            return fail("expected Action=Input,...");
        }
        Action action;
        if (!parseAction(trim(line.substr(0, equals)), &action)) {
            return fail("unknown action \"" + trim(line.substr(0, equals)) + "\"");
        }
        result.clear(action);
        std::istringstream list(line.substr(equals + 1));
        std::string item;
        while (std::getline(list, item, ',')) {
            item = trim(item);
            if (item.empty()) {
                continue;
            }
            Binding binding;
            if (!parseBinding(item, &binding)) {
                return fail("unknown input \"" + item + "\"");
            }
            result.bind(action, binding);
        }
    }
    *this = std::move(result);
    return true;
}

const char* actionName(Action action) {
    const int index = static_cast<int>(action);
    return index >= 0 && index < kActionCount ? kActionNames[index] : "?";
}

std::string keyName(KeyCode code) {
    for (const NamedKey& key : kKeyNames) {
        if (key.code == code) {
            return key.name;
        }
    }
    return "Usage" + std::to_string(code);
}

bool parseKeyName(const std::string& name, KeyCode* code) {
    for (const NamedKey& key : kKeyNames) {
        if (name == key.name) {
            *code = key.code;
            return true;
        }
    }
    if (name.rfind("Usage", 0) == 0 && name.size() > 5) {
        char* end = nullptr;
        const long value = std::strtol(name.c_str() + 5, &end, 10);
        if (*end == '\0' && value > 0 && value < Key::Max) {
            *code = static_cast<KeyCode>(value);
            return true;
        }
    }
    return false;
}

Viewport Viewport::letterbox(float windowWidth, float windowHeight, float imageAspect) {
    Viewport v;
    v.windowWidth = windowWidth;
    v.windowHeight = windowHeight;
    if (windowWidth <= 0.0f || windowHeight <= 0.0f || imageAspect <= 0.0f) {
        return v;
    }
    if (windowWidth / windowHeight > imageAspect) {
        v.imageHeight = windowHeight;
        v.imageWidth = windowHeight * imageAspect;
        v.imageX = (windowWidth - v.imageWidth) * 0.5f;
    } else {
        v.imageWidth = windowWidth;
        v.imageHeight = windowWidth / imageAspect;
        v.imageY = (windowHeight - v.imageHeight) * 0.5f;
    }
    return v;
}

}  // namespace PetariNative::Input

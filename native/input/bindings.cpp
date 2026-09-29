// Remappable bindings, their defaults, and their text form.

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>

#include "petari/input.hpp"

namespace PetariNative::Input {
namespace {

constexpr int kActionCount = static_cast<int>(Action::Count);

const char* const kActionNames[kActionCount] = {
    "StickUp", "StickDown", "StickLeft", "StickRight", "A",        "B",        "Plus",     "Minus",     "Home",   "One",
    "Two",     "DpadUp",    "DpadDown",  "DpadLeft",   "DpadRight", "NunchukC", "NunchukZ", "Shake",     "TiltHold",
    "PostureToggle", "Walk",
};

const char* const kMouseNames[static_cast<int>(MouseButton::Count)] = {"Left", "Middle", "Right", "X1", "X2"};

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
    return false;
}

}  // namespace

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
    b.bind(Action::TiltHold, Binding::key(Key::Tab));
    b.bind(Action::PostureToggle, Binding::key(Key::T));
    b.bind(Action::Walk, Binding::key(Key::LeftAlt));
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
            } else {
                out << "Mouse:" << kMouseNames[input.code];
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

#include "petari/mods.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "petari/host_allocation.hpp"
#include "petari/input.hpp"

namespace PetariNative::Mods {
namespace {

constexpr int kCount = static_cast<int>(Mod::Count);
constexpr const char* kNames[kCount] = {"CollectStarBits", "ShootEnemy"};
constexpr const char* kDescriptions[kCount] = {"Collect visible Star Bits", "Fire a Star Bit at the nearest enemy"};
constexpr Input::Action kActions[kCount] = {Input::Action::ModCollectStarBits, Input::Action::ModShootEnemy};

std::atomic<bool> gEnabled[kCount];
std::mutex gFileMutex;
std::filesystem::path gFile;

bool parseInto(const std::string& text, bool (&out)[kCount], std::string* error) {
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        const auto eq = line.find('=');
        const std::string key = line.substr(start, eq == std::string::npos ? std::string::npos : eq - start);
        const std::string value = eq == std::string::npos ? "" : line.substr(eq + 1);
        int index = -1;
        for (int i = 0; i < kCount; ++i) {
            if (key == kNames[i]) index = i;
        }
        if (index < 0 || (value != "on" && value != "off")) {
            *error = "line " + std::to_string(number) + ": expected <CollectStarBits|ShootEnemy>=on|off";
            return false;
        }
        out[index] = value == "on";
    }
    return true;
}

}  // namespace

const char* name(Mod mod) {
    const int i = static_cast<int>(mod);
    return i >= 0 && i < kCount ? kNames[i] : "?";
}

const char* description(Mod mod) {
    const int i = static_cast<int>(mod);
    return i >= 0 && i < kCount ? kDescriptions[i] : "?";
}

bool enabled(Mod mod) {
    const int i = static_cast<int>(mod);
    return i >= 0 && i < kCount && gEnabled[i].load(std::memory_order_relaxed);
}

void setEnabled(Mod mod, bool on) {
    const int i = static_cast<int>(mod);
    if (i >= 0 && i < kCount) gEnabled[i].store(on, std::memory_order_relaxed);
}

std::string serialize() {
    std::string text = "# Native mods (native/MODS.md). on or off.\n";
    for (int i = 0; i < kCount; ++i) {
        text += kNames[i];
        text += gEnabled[i].load(std::memory_order_relaxed) ? "=on\n" : "=off\n";
    }
    return text;
}

bool parse(const std::string& text, std::string* error) {
    bool values[kCount];
    for (int i = 0; i < kCount; ++i) values[i] = gEnabled[i].load(std::memory_order_relaxed);
    if (!parseInto(text, values, error)) return false;
    for (int i = 0; i < kCount; ++i) gEnabled[i].store(values[i], std::memory_order_relaxed);
    return true;
}

bool load(const std::filesystem::path& file, std::string* error) {
    HostAllocationScope scope;
    bool values[kCount] = {};
    std::ifstream in(file);
    if (in) {
        std::stringstream text;
        text << in.rdbuf();
        if (!parseInto(text.str(), values, error)) {
            *error = file.string() + ": " + *error;
            return false;
        }
    }
    if (const char* env = std::getenv("PETARI_MODS"); env != nullptr && env[0] != '\0') {
        std::string list = env;
        for (int i = 0; i < kCount; ++i) values[i] = false;
        std::istringstream names(list);
        std::string item;
        while (std::getline(names, item, ',')) {
            if (item.empty() || item == "none") continue;
            int index = -1;
            for (int i = 0; i < kCount; ++i) {
                if (item == kNames[i]) index = i;
            }
            if (index < 0) {
                *error = "PETARI_MODS: unknown mod " + item;
                return false;
            }
            values[index] = true;
        }
    }
    for (int i = 0; i < kCount; ++i) gEnabled[i].store(values[i], std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(gFileMutex);
    gFile = file;
    return true;
}

bool save(std::string* error) {
    HostAllocationScope scope;
    std::lock_guard<std::mutex> lock(gFileMutex);
    if (gFile.empty()) return true;
    std::ofstream out(gFile, std::ios::trunc);
    out << serialize();
    if (!out) {
        *error = "cannot write " + gFile.string();
        return false;
    }
    return true;
}

void resetForTesting() {
    for (auto& value : gEnabled) value.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(gFileMutex);
    gFile.clear();
}

}  // namespace PetariNative::Mods

extern "C" bool petari_mod_take_press(int mod) {
    using namespace PetariNative;
    if (mod < 0 || mod >= static_cast<int>(Mods::Mod::Count)) return false;
    const int presses = Input::takeActionPresses(Mods::kActions[mod]);
    return presses > 0 && Mods::enabled(static_cast<Mods::Mod>(mod));
}

extern "C" bool petari_mod_enabled(int mod) {
    using namespace PetariNative;
    return mod >= 0 && mod < static_cast<int>(Mods::Mod::Count) && Mods::enabled(static_cast<Mods::Mod>(mod));
}

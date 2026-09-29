// SC system settings over native preferences. Replaces src/RVL_SDK/sc/*.c,
// which parse the console's SYSCONF through NAND. Bluetooth pairing records
// (SCGet/SetBt*DeviceInfoArray) and the generic item accessors (SCFind*Item,
// SCReplace*Item) have no host equivalent and are not implemented.

#include <revolution/os.h>
#include <revolution/sc.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#include "petari/host_allocation.hpp"
#include "petari/platform/sc.hpp"

namespace fs = std::filesystem;

namespace PetariNative::Platform::SC {
namespace {

struct State {
    std::mutex lock;
    Settings settings;
    fs::path store;
    bool loadFailed = false;
};

State& state() {
    static State* instance = new State;
    return *instance;
}

using Guard = std::lock_guard<std::mutex>;

// Field access by name, for the text store.
bool assign(Settings& s, const std::string& key, long value) {
    auto setU8 = [&](std::uint8_t& field) {
        field = static_cast<std::uint8_t>(value);
        return value >= 0 && value <= 255;
    };
    if (key == "language") return setU8(s.language);
    if (key == "aspect_ratio") return setU8(s.aspectRatio);
    if (key == "progressive_mode") return setU8(s.progressiveMode);
    if (key == "sound_mode") return setU8(s.soundMode);
    if (key == "eurgb60_mode") return setU8(s.euRgb60Mode);
    if (key == "screen_saver_mode") return setU8(s.screenSaverMode);
    if (key == "display_offset_h") { s.displayOffsetH = static_cast<std::int8_t>(value); return value >= -128 && value <= 127; }
    if (key == "sensor_bar_position") return setU8(s.sensorBarPosition);
    if (key == "speaker_volume") return setU8(s.speakerVolume);
    if (key == "motor_mode") return setU8(s.motorMode);
    if (key == "dpd_sensibility") { s.dpdSensibility = static_cast<std::uint32_t>(value); return value >= 0; }
    if (key == "idle_mode") return setU8(s.idleMode);
    if (key == "idle_led") return setU8(s.idleLed);
    return false;
}

std::string serialize(const Settings& s) {
    std::ostringstream out;
    out << "# Petari native system settings\n"
        << "language=" << int(s.language) << "\n"
        << "aspect_ratio=" << int(s.aspectRatio) << "\n"
        << "progressive_mode=" << int(s.progressiveMode) << "\n"
        << "sound_mode=" << int(s.soundMode) << "\n"
        << "eurgb60_mode=" << int(s.euRgb60Mode) << "\n"
        << "screen_saver_mode=" << int(s.screenSaverMode) << "\n"
        << "display_offset_h=" << int(s.displayOffsetH) << "\n"
        << "sensor_bar_position=" << int(s.sensorBarPosition) << "\n"
        << "speaker_volume=" << int(s.speakerVolume) << "\n"
        << "motor_mode=" << int(s.motorMode) << "\n"
        << "dpd_sensibility=" << s.dpdSensibility << "\n"
        << "idle_mode=" << int(s.idleMode) << "\n"
        << "idle_led=" << int(s.idleLed) << "\n";
    return out.str();
}

bool parse(const std::string& text, Settings& out, std::string* error) {
    Settings s;
    std::istringstream in(text);
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t eq = line.find('=');
        char* end = nullptr;
        const std::string value = eq == std::string::npos ? "" : line.substr(eq + 1);
        const long number = std::strtol(value.c_str(), &end, 10);
        if (eq == std::string::npos || value.empty() || *end != '\0' || number < -128 || number > 0xFFFF ||
            !assign(s, line.substr(0, eq), number)) {
            if (error) {
                *error = "line " + std::to_string(lineNumber) + ": invalid setting '" + line + "'";
            }
            return false;
        }
    }
    const std::string problem = validate(s);
    if (!problem.empty()) {
        if (error) {
            *error = problem;
        }
        return false;
    }
    out = s;
    return true;
}

bool loadLocked(std::string* error) {
    State& st = state();
    std::error_code ec;
    if (!fs::exists(st.store, ec)) {
        return true;
    }
    std::ifstream in(st.store);
    std::stringstream buffer;
    buffer << in.rdbuf();
    if (!in) {
        if (error) {
            *error = "cannot read " + st.store.string();
        }
        return false;
    }
    Settings loaded;
    if (!parse(buffer.str(), loaded, error)) {
        return false;
    }
    st.settings = loaded;
    return true;
}

bool saveLocked(std::string* error) {
    State& st = state();
    if (st.store.empty()) {
        if (error) {
            *error = "no settings store is configured";
        }
        return false;
    }
    const fs::path temporary = st.store.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << serialize(st.settings);
        out.flush();
        if (!out) {
            if (error) {
                *error = "cannot write " + temporary.string();
            }
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, st.store, ec);
    if (ec) {
        if (error) {
            *error = "cannot replace " + st.store.string() + ": " + ec.message();
        }
        return false;
    }
    return true;
}

Settings snapshot() {
    Guard guard(state().lock);
    return state().settings;
}

}  // namespace

std::string validate(const Settings& s) {
    auto range = [](const char* name, long value, long min, long max) -> std::string {
        if (value < min || value > max) {
            return std::string(name) + " " + std::to_string(value) + " is outside " + std::to_string(min) + "..." + std::to_string(max);
        }
        return {};
    };
    for (const std::string& problem : {
             range("language", s.language, SC_LANG_JAPANESE, SC_LANG_KOREAN),
             range("aspect ratio", s.aspectRatio, SC_ASPECT_RATIO_4x3, SC_ASPECT_RATIO_16x9),
             range("progressive mode", s.progressiveMode, SC_PROGRESSIVE_MODE_OFF, SC_PROGRESSIVE_MODE_ON),
             range("sound mode", s.soundMode, SC_SOUND_MODE_MONO, SC_SOUND_MODE_SURROUND),
             range("EuRGB60 mode", s.euRgb60Mode, SC_EURGB60_MODE_OFF, SC_EURGB60_MODE_ON),
             range("screen saver mode", s.screenSaverMode, 0, 1),
             range("horizontal display offset", s.displayOffsetH, -32, 32),
             range("sensor bar position", s.sensorBarPosition, 0, 1),
             range("speaker volume", s.speakerVolume, 0, 127),
             range("motor mode", s.motorMode, 0, 1),
             range("sensor bar sensitivity", static_cast<long>(s.dpdSensibility), 1, 5),
             range("idle mode", s.idleMode, 0, 1),
             range("idle LED", s.idleLed, 0, 1),
         }) {
        if (!problem.empty()) {
            return problem;
        }
    }
    return {};
}

Settings current() {
    return snapshot();
}

bool set(const Settings& settings, std::string* error) {
    PetariNative::HostAllocationScope hostAllocations;
    const std::string problem = validate(settings);
    if (!problem.empty()) {
        if (error) {
            *error = problem;
        }
        return false;
    }
    Guard guard(state().lock);
    state().settings = settings;
    return true;
}

bool setStore(const fs::path& file, std::string* error) {
    PetariNative::HostAllocationScope hostAllocations;
    Guard guard(state().lock);
    state().store = file;
    state().loadFailed = !loadLocked(error);
    return !state().loadFailed;
}

bool save(std::string* error) {
    PetariNative::HostAllocationScope hostAllocations;
    Guard guard(state().lock);
    return saveLocked(error);
}

void reset() {
    PetariNative::HostAllocationScope hostAllocations;
    Guard guard(state().lock);
    state().settings = Settings{};
    state().store.clear();
    state().loadFailed = false;
}

}  // namespace PetariNative::Platform::SC

using namespace PetariNative::Platform::SC;

extern "C" {

void SCInit(void) {
    PetariNative::HostAllocationScope hostAllocations;
    std::string error;
    Guard guard(state().lock);
    if (!state().store.empty()) {
        state().loadFailed = !loadLocked(&error);
        if (state().loadFailed) {
            OSReport("SC: cannot load %s: %s\n", state().store.c_str(), error.c_str());
        }
    }
}

u32 SCCheckStatus(void) {
    Guard guard(state().lock);
    return state().loadFailed ? SC_STATUS_ERROR : SC_STATUS_OK;
}

u8 SCGetLanguage(void) { return snapshot().language; }
u8 SCGetAspectRatio(void) { return snapshot().aspectRatio; }
u8 SCGetProgressiveMode(void) { return snapshot().progressiveMode; }
u8 SCGetSoundMode(void) { return snapshot().soundMode; }
u8 SCGetEuRgb60Mode(void) { return snapshot().euRgb60Mode; }
u8 SCGetScreenSaverMode(void) { return snapshot().screenSaverMode; }
s8 SCGetDisplayOffsetH(void) { return snapshot().displayOffsetH; }
u8 SCGetWpadSensorBarPosition(void) { return snapshot().sensorBarPosition; }
u8 SCGetWpadSpeakerVolume(void) { return snapshot().speakerVolume; }
u8 SCGetWpadMotorMode(void) { return snapshot().motorMode; }
u32 SCGetBtDpdSensibility(void) { return snapshot().dpdSensibility; }

BOOL SCGetIdleMode(SCIdleModeInfo* info) {
    const Settings s = snapshot();
    info->mode = s.idleMode;
    info->led = s.idleLed;
    return TRUE;
}

BOOL SCSetWpadSpeakerVolume(u8 volume) {
    Settings s = snapshot();
    s.speakerVolume = volume;
    return set(s, nullptr) ? TRUE : FALSE;
}

BOOL SCSetWpadMotorMode(u8 mode) {
    Settings s = snapshot();
    s.motorMode = mode;
    return set(s, nullptr) ? TRUE : FALSE;
}

// Persists the settings. The callback runs before this returns; on the console
// it runs later, from the NAND completion.
void SCFlushAsync(SCFlushCallback callback) {
    std::string error;
    const bool ok = save(&error);
    if (!ok) {
        OSReport("SC: flush failed: %s\n", error.c_str());
    }
    if (callback) {
        callback(ok ? SC_STATUS_OK : SC_STATUS_ERROR);
    }
}

}  // extern "C"

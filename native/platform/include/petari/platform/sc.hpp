#pragma once
// Native system settings behind the SDK's SC API (<revolution/sc.h>).
//
// On the Wii these come from the console's SYSCONF file. Natively they are
// application preferences, validated here, optionally persisted to a text
// file chosen by the application. Defaults suit a modern display: English,
// 16:9, progressive scan, stereo.

#include <cstdint>
#include <filesystem>
#include <string>

namespace PetariNative::Platform::SC {

struct Settings {
    std::uint8_t language = 1;           // SC_LANG_* (0 Japanese ... 9 Korean)
    std::uint8_t aspectRatio = 1;        // SC_ASPECT_RATIO_4x3 / _16x9
    std::uint8_t progressiveMode = 1;    // SC_PROGRESSIVE_MODE_OFF / _ON
    std::uint8_t soundMode = 1;          // SC_SOUND_MODE_MONO / _STEREO / _SURROUND
    std::uint8_t euRgb60Mode = 0;        // SC_EURGB60_MODE_OFF / _ON
    std::uint8_t screenSaverMode = 1;    // 0 off, 1 on
    std::int8_t displayOffsetH = 0;      // -32 ... 32
    std::uint8_t sensorBarPosition = 0;  // 0 below the screen, 1 above
    std::uint8_t speakerVolume = 0x58;   // Wii Remote speaker, 0 ... 127
    std::uint8_t motorMode = 1;          // Wii Remote rumble, 0 off, 1 on
    std::uint32_t dpdSensibility = 3;    // sensor bar sensitivity, 1 ... 5
    std::uint8_t idleMode = 0;           // WiiConnect24 standby, 0 off, 1 on
    std::uint8_t idleLed = 0;            // standby LED, 0 off, 1 on
};

// Returns an empty string when every field is in range, otherwise a
// description of the first invalid field.
std::string validate(const Settings& settings);

Settings current();

// Replaces the settings after validating them; SC getters see them at once.
bool set(const Settings& settings, std::string* error);

// Chooses the file the settings are loaded from (SCInit) and flushed to
// (SCFlushAsync). An existing file is loaded now; a missing one is created on
// the next flush. Without a store, SCFlushAsync reports an error: there is
// nowhere to persist to.
bool setStore(const std::filesystem::path& file, std::string* error);

// Writes the current settings to the store.
bool save(std::string* error);

// Restores defaults and forgets the store. For tests.
void reset();

}  // namespace PetariNative::Platform::SC

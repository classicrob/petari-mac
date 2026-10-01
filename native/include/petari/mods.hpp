#pragma once
// Native gameplay mods (native/MODS.md). Every mod is off unless the player
// turns it on, so an unmodified game and every test run are untouched.
//
// Settings live in mods.txt in the user directory, one "Name=on|off" line per
// mod; PETARI_MODS=<name>,<name> (or "none") overrides the file for a run.
// The F1 menu's Mods page toggles them and saves the file.

#include <filesystem>
#include <string>

namespace PetariNative::Mods {

enum class Mod : int {
    CollectStarBits,  // one button collects every Star Bit on screen
    ShootEnemy,       // one button fires a Star Bit at the nearest enemy on screen
    OdysseyMovement,  // Mario moves with Super Mario Odyssey's physics (docs/dev/ODYSSEY_MOVEMENT.md); no button
    Count
};

const char* name(Mod mod);          // "CollectStarBits", the mods.txt key
const char* description(Mod mod);   // "Collect visible Star Bits", for the menu

bool enabled(Mod mod);  // any thread
void setEnabled(Mod mod, bool on);

// Reads mods.txt (a missing file leaves every mod off), then PETARI_MODS.
// Unknown names or values are an error; nothing changes on error.
bool load(const std::filesystem::path& file, std::string* error);
// Writes every mod's state to the file load() read (no-op before load).
bool save(std::string* error);
// Text form used by load/save.
std::string serialize();
bool parse(const std::string& text, std::string* error);

// Tests only: every mod off, no file.
void resetForTesting();

}  // namespace PetariNative::Mods

// Game side (SDK code, native builds). A press is consumed each call; while a
// mod is off its presses are dropped, so turning it on never fires a stale press.
extern "C" bool petari_mod_take_press(int mod);
extern "C" bool petari_mod_enabled(int mod);

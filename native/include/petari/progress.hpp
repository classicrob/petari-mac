#pragma once
// My Progress (native/PROGRESS.md): a personal record of the missions you cleared, kept
// beside the game save, never inside it. <user dir>/progress.json holds, per galaxy and
// mission: first and last clear (UTC), clear count, best time (stage entry to Power Star),
// fewest deaths, and the best coins, Star Bits and purple coins of a clearing run, plus
// the latest clearing run. It works on any save (an unlocked one too) and changes nothing
// in it. Always on: it only records. Delete the file (or Reset on the My Progress page) to
// wipe it.
//
// The game side calls petari_progress_star_get (progress_hook.hpp) when a Power Star is
// collected; the frame seam feeds Progress::frame() once per game frame.

#include <filesystem>
#include <string>
#include <vector>

namespace PetariNative::Progress {

struct Run {
    double timeS = 0;  // gameplay time from stage entry, pauses excluded
    int deaths = 0;
    int coins = 0;
    int starBits = 0;
    int purpleCoins = 0;
};

struct Mission {
    std::string stage;  // internal name, e.g. "EggStarGalaxy"
    int mission = 0;    // the Power Star / scenario number
    bool grand = false; // a Grand Star
    std::string firstClear, lastClear;  // "2026-09-30T20:11:03Z"
    int clears = 0;
    double bestTimeS = 0;
    int fewestDeaths = 0;
    int bestCoins = 0, bestStarBits = 0, bestPurpleCoins = 0;
    Run last;
};

// What the frame seam reads from the game each frame.
struct FrameState {
    bool inGame = false;  // a Game scene is ready and the player exists
    bool demo = false;    // a cutscene or demo is playing
    bool paused = false;  // the pause menu is open
    bool dead = false;    // the player is dead
    std::string stage;
    int scenario = 0;     // the selected mission
    unsigned long sceneId = 0;  // changes when the scene is created again (retry, new stage)
    int coins = 0, starBits = 0, purpleCoins = 0;
};

// --- record ---
bool find(const std::string& stage, int mission, Mission* out);
int clearedCount();
std::vector<Mission> all();
// Wipes the record in memory and the file.
void reset();

// --- file ---
// Reads <file> (missing: empty record) and remembers it for save(). A damaged file is an
// error and is left untouched; nothing changes.
bool load(const std::filesystem::path& file, std::string* error);
bool save(std::string* error);
std::string serialize();
bool parse(const std::string& text, std::string* error);

// --- tracking ---
// Called once per game frame, in game-thread order.
void frame(const FrameState& state);
// A Power Star (or Grand Star) was collected now; mission is its number. Records the clear
// from the current run's counters (last frame()) and saves the file.
void starGet(int mission, bool grand);
// Time source for tests; default is the system clock. Returns "YYYY-MM-DDTHH:MM:SSZ".
void setClockForTesting(std::string (*now)());
void resetForTesting();
// Seconds the current run has been timed, and its deaths (tests, diagnostics).
double currentRunTimeS();
int currentRunDeaths();

// --- badges on the mission-select screen (native/PROGRESS.md) ---
// The game publishes where each shown mission star is (normalized game-image coordinates, origin top-left)
// every frame the mission select is up; the overlay (home_menu imgui_overlay.cpp) draws a marker and the best
// time by each mission you cleared yourself.
struct BadgeStar {
    int mission = 0;
    float u = 0, v = 0;
    bool cleared = false;
    double bestTimeS = 0;
    int clears = 0;
};
void publishBadge(const char* stage, int mission, float u, float v);  // game thread
// The stars published in the last couple of frames, with this record's data; false when none (the mission select
// is not up) or badges are switched off.
bool currentBadges(std::vector<BadgeStar>* out, std::string* stage = nullptr);
bool badgesEnabled();
// Persisted in progress_settings.txt beside progress.json (not part of the record: Reset keeps it).
void setBadgesEnabled(bool on);

constexpr double kFramesPerSecond = 60.0;

}  // namespace PetariNative::Progress

// Tests for the personal progress record (petari/progress.hpp): tracking from per-frame game
// state, best values, pause/demo exclusion, deaths, file round trip, damaged files, reset.
// No game, no retail data.

#include "petari/progress.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
namespace P = PetariNative::Progress;

namespace {
int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}
int clockCalls = 0;
std::string fakeClock() {
    static const char* const stamps[] = {"2026-10-01T10:00:00Z", "2026-10-02T11:30:00Z", "2026-10-03T12:00:00Z"};
    return stamps[clockCalls++ % 3];
}
std::string read(const fs::path& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

P::FrameState play(const char* stage, int scenario, unsigned long scene) {
    P::FrameState s;
    s.inGame = true;
    s.stage = stage;
    s.scenario = scenario;
    s.sceneId = scene;
    return s;
}
// n gameplay frames.
void run(P::FrameState s, int n) {
    for (int i = 0; i < n; ++i) P::frame(s);
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("petari_progress_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "progress.json";
    std::string error;

    // Missing file: empty, and loading remembers the path.
    P::resetForTesting();
    P::setClockForTesting(fakeClock);
    check(P::load(file, &error) && P::clearedCount() == 0, "missing progress.json: empty record");
    check(!fs::exists(file), "loading does not create the file");

    // A first clear: 2 s of play, a pause (not counted), a demo (not counted), one death, items.
    auto s = play("EggStarGalaxy", 1, 100);
    s.coins = 12;
    s.starBits = 40;
    run(s, 60);
    auto paused = s;
    paused.paused = true;
    run(paused, 300);
    auto demo = s;
    demo.demo = true;
    run(demo, 120);
    auto dead = s;
    dead.dead = true;
    run(dead, 5);       // one death however long it lasts
    run(s, 55);         // 60 + 5 + 55 = 120 counted frames = 2.0 s
    check(P::currentRunDeaths() == 1 && P::currentRunTimeS() == 2.0, "pauses and demos are not timed; a death is counted once");
    s.coins = 31;
    s.starBits = 77;
    P::frame(s);        // the frame of the star: 121 counted frames
    P::starGet(1, false);
    P::Mission m;
    check(P::find("EggStarGalaxy", 1, &m) && m.clears == 1 && m.firstClear == "2026-10-01T10:00:00Z" && m.lastClear == m.firstClear,
          "first clear is recorded with its date");
    check(m.bestTimeS > 2.0 && m.bestTimeS < 2.02 && m.fewestDeaths == 1 && m.bestCoins == 31 && m.bestStarBits == 77,
          "time, deaths, coins and Star Bits of the clearing run");
    check(m.last.coins == 31 && m.last.deaths == 1 && !m.grand, "the latest run is kept");
    check(fs::exists(file), "a clear saves the file");

    // A second, better clear in a new scene (retry): bests improve, first clear stays.
    auto retry = play("EggStarGalaxy", 1, 101);
    retry.coins = 5;
    retry.starBits = 90;
    run(retry, 90);
    P::starGet(1, false);
    check(P::find("EggStarGalaxy", 1, &m) && m.clears == 2 && m.firstClear == "2026-10-01T10:00:00Z" &&
              m.lastClear == "2026-10-02T11:30:00Z",
          "second clear: count and last date");
    check(m.bestTimeS == 1.5 && m.fewestDeaths == 0 && m.bestCoins == 31 && m.bestStarBits == 90 && m.last.timeS == 1.5,
          "bests: fastest, fewest deaths, most coins, most Star Bits");
    // A slower, deadlier run does not worsen the bests.
    auto worse = play("EggStarGalaxy", 1, 102);
    run(worse, 600);
    auto died = worse;
    died.dead = true;
    P::frame(died);
    P::frame(worse);
    P::frame(died);
    P::starGet(1, false);
    check(P::find("EggStarGalaxy", 1, &m) && m.clears == 3 && m.bestTimeS == 1.5 && m.fewestDeaths == 0 && m.last.deaths == 2,
          "a worse run keeps the bests");

    // Another mission in the same galaxy and a hidden star number; leaving the Game scene ends the run.
    P::frame(P::FrameState{});
    auto other = play("EggStarGalaxy", 2, 103);
    run(other, 30);
    P::starGet(2, false);
    check(P::find("EggStarGalaxy", 2, &m) && m.bestTimeS == 0.5, "another mission is its own record");
    check(P::clearedCount() == 2, "two missions cleared");
    // A star outside a tracked run (no game frames since the scene ended) records nothing.
    P::frame(P::FrameState{});
    P::starGet(3, false);
    check(!P::find("EggStarGalaxy", 3, nullptr) && P::clearedCount() == 2, "no run, no record");

    // File round trip: a fresh process reads the same record.
    const std::string saved = read(file);
    check(saved.find("\"first_clear\": \"2026-10-01T10:00:00Z\"") != std::string::npos && saved.find("\"version\": 1") != std::string::npos,
          "the file is readable JSON with the fields");
    P::resetForTesting();
    check(P::load(file, &error) && P::clearedCount() == 2, "reload");
    check(P::find("EggStarGalaxy", 1, &m) && m.clears == 3 && m.bestTimeS == 1.5 && m.bestStarBits == 90 && m.firstClear == "2026-10-01T10:00:00Z",
          "values survive the round trip");
    check(P::serialize() == saved, "serialize is stable");

    // Separate record per user directory.
    const fs::path other_file = dir / "other" / "progress.json";
    check(P::load(other_file, &error) && P::clearedCount() == 0, "another user directory has its own empty record");
    auto third = play("HoneyBeeKingdomGalaxy", 1, 7);
    run(third, 60);
    P::starGet(1, false);
    check(fs::exists(other_file) && P::clearedCount() == 1 && read(file) == saved, "the first user's file is untouched");

    // A grand star.
    auto grand = play("PeachCastleFinalGalaxy", 1, 9);
    run(grand, 10);
    P::starGet(1, true);
    check(P::find("PeachCastleFinalGalaxy", 1, &m) && m.grand, "a Grand Star is flagged");

    // Damaged files: reported, nothing changes, the file is not overwritten.
    P::resetForTesting();
    P::setClockForTesting(fakeClock);
    const fs::path bad = dir / "bad.json";
    {
        std::ofstream out(bad);
        out << "{\"version\": 1, \"missions\": {\"X:1\": ";
    }
    check(!P::load(bad, &error) && !error.empty() && P::clearedCount() == 0, "a truncated file is an error and changes nothing");
    check(read(bad) == "{\"version\": 1, \"missions\": {\"X:1\": ", "the damaged file is left as it was");
    auto still = play("EggStarGalaxy", 5, 11);
    run(still, 10);
    P::starGet(5, false);
    check(read(bad) == "{\"version\": 1, \"missions\": {\"X:1\": ", "no clear is written over a damaged file");
    check(!fs::exists(dir / "progress.json.tmp"), "and nothing is written elsewhere");
    for (const char* text : {"[]", "{\"version\": 2, \"missions\": {}}", "{\"version\": 1}", "{\"version\": 1, \"missions\": {\"A:1\": 3}}",
                             "{\"version\": 1, \"missions\": {\"A:1\": {\"stage\": \"A\"}}}", ""}) {
        check(!P::parse(text, &error), "invalid progress text is rejected");
    }
    check(P::parse("{\"version\": 1, \"missions\": {}}", &error) && P::clearedCount() == 0, "an empty record parses");

    // Reset wipes memory and the file.
    P::resetForTesting();
    check(P::load(file, &error) && P::clearedCount() == 2, "reload before reset");
    P::reset();
    check(P::clearedCount() == 0 && !fs::exists(file), "reset wipes the record and deletes the file");
    check(P::load(file, &error) && P::clearedCount() == 0, "a wiped record loads empty");

    // The save never touches anything but progress.json (no .tmp left behind).
    auto last = play("EggStarGalaxy", 1, 1);
    run(last, 6);
    P::starGet(1, false);
    int entries = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++entries;
    }
    check(!fs::exists(fs::path(file.string() + ".tmp")) && fs::exists(file), "atomic save leaves no temporary file");
    (void)entries;

    // Badges: positions published by the game during the last two frames, joined with this record.
    P::resetForTesting();
    P::setClockForTesting(fakeClock);
    check(P::load(file, &error), "load for the badge checks");
    {
        auto s1 = play("EggStarGalaxy", 1, 1);
        run(s1, 30);
        P::starGet(1, false);
        std::vector<P::BadgeStar> stars;
        P::frame(P::FrameState{});  // the mission select: not in a Game scene
        P::publishBadge("EggStarGalaxy", 1, 0.25f, 0.5f);
        P::publishBadge("EggStarGalaxy", 2, 0.75f, 0.5f);
        P::publishBadge("EggStarGalaxy", 2, 0.80f, 0.55f);  // the same star again: updated, not duplicated
        check(P::currentBadges(&stars) && stars.size() == 2, "two stars published this frame");
        P::Mission recorded;
        check(P::find("EggStarGalaxy", 1, &recorded), "the record exists");
        check(stars[0].mission == 1 && stars[0].cleared && stars[0].bestTimeS == recorded.bestTimeS && stars[0].clears == recorded.clears &&
                  stars[0].u == 0.25f,
              "a cleared star carries its record");
        check(stars[1].mission == 2 && !stars[1].cleared && stars[1].u == 0.80f, "an uncleared star is listed without one");
        P::frame(P::FrameState{});
        check(P::currentBadges(&stars), "still fresh one frame later");
        P::frame(P::FrameState{});
        P::frame(P::FrameState{});
        check(!P::currentBadges(&stars), "gone once the mission select stops publishing");
        P::publishBadge("HoneyBeeKingdomGalaxy", 1, 0.1f, 0.1f);
        check(P::currentBadges(&stars) && stars.size() == 1 && !stars[0].cleared, "another galaxy's frame replaces the old list");
        // The setting: on by default, persisted beside the record, kept by Reset.
        check(P::badgesEnabled(), "badges default on");
        P::setBadgesEnabled(false);
        check(!P::badgesEnabled() && !P::currentBadges(&stars), "off hides the badges");
        check(read(dir / "progress_settings.txt") == "badges=off\n", "the setting is saved in progress_settings.txt");
        P::resetForTesting();
        check(P::load(file, &error) && !P::badgesEnabled(), "the setting is read back");
        P::reset();
        check(!P::badgesEnabled() && fs::exists(dir / "progress_settings.txt"), "Reset wipes the record, not the setting");
        P::setBadgesEnabled(true);
    }

    fs::remove_all(dir);
    P::resetForTesting();
    std::printf("progress tests passed (%d checks)\n", checks);
    return 0;
}

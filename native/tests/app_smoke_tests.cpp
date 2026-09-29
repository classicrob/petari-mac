// Tests for the smoke script (native/app/smoke.hpp) and the milestone history
// (petari/milestone.hpp): the presses the script makes for each observed game
// state, their timing, and its pass/fail/blocked decisions.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../app/smoke.hpp"
#include "petari/milestone.hpp"

namespace Smoke = PetariNative::App::Smoke;
using Smoke::Button;
using Smoke::Observation;
using Smoke::Result;

namespace {

int checks = 0;
void check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label.c_str());
        std::exit(1);
    }
}

struct Event {
    unsigned long frame;
    Button button;
    bool down;
    bool focus;
};

struct Run {
    Smoke::Driver driver;
    std::vector<Event> events;
    std::vector<std::string> log;
    int quits = 0;

    explicit Run(unsigned long limit = 100000) : driver(limit) {}

    void frames(const Observation& observation, unsigned long count) {
        for (unsigned long i = 0; i < count; i++) {
            const Smoke::Step step = driver.step(observation);
            for (const Smoke::Press& press : step.presses) {
                events.push_back({driver.frame(), press.button, press.down, step.assertFocus});
            }
            for (const std::string& line : driver.log()) {
                log.push_back(line);
            }
            quits += step.requestQuit ? 1 : 0;
        }
    }
    void frame(const Observation& observation) { frames(observation, 1); }

    int count(Button button, bool down) const {
        int n = 0;
        for (const Event& e : events) {
            n += e.button == button && e.down == down;
        }
        return n;
    }
    bool logged(const std::string& text) const {
        for (const std::string& line : log) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

Observation logo(bool strap) {
    Observation o;
    o.scene = "Logo";
    o.strap = strap;
    o.videoConfigured = true;
    o.videoBlack = false;
    return o;
}

Observation fileSelect(bool ready) {
    Observation o;
    o.scene = "Game";
    o.stage = "FileSelect";
    o.sceneReady = ready;
    o.videoConfigured = true;
    o.videoBlack = false;
    return o;
}

Observation with(Observation o, const char* milestone) {
    o.milestones.push_back(milestone);
    return o;
}

// Runs a script through the title prompt; returns the frame of the A+B press.
unsigned long toTitlePress(Run& run) {
    run.frames(Observation{}, 10);
    run.frames(logo(false), 30);
    run.frames(logo(true), 200);
    run.frames(fileSelect(false), 50);
    run.frame(with(fileSelect(true), "FileSelector.Title"));
    run.frame(with(fileSelect(true), "TitleSequence.BgmPrepare"));
    run.frames(fileSelect(true), 30);
    run.frame(with(fileSelect(true), "TitleSequence.LogoDisplay"));
    run.frames(fileSelect(true), 9);
    return run.driver.frame();
}

void testHappyPath() {
    Run run;
    run.frames(Observation{}, 10);
    check(run.driver.phase() == std::string("boot") && run.events.empty(), "boot: no presses before the Logo scene");
    check(run.logged("video not configured, black"), "video state logged");
    run.frames(logo(false), 30);
    check(run.logged("scene Logo") && run.logged("video configured, showing"), "Logo scene and video logged");

    // Strap reminder: first tap at strap frame 480, then every 120.
    run.frames(logo(true), 479);
    check(run.events.empty(), "no tap before strap frame 480");
    run.frame(logo(true));
    check(run.count(Button::A, true) == 1 && run.events.back().focus, "A tap at strap frame 480, with focus asserted");
    run.frames(logo(true), 5);
    check(run.count(Button::A, false) == 0, "A held for 6 frames");
    run.frame(logo(true));
    check(run.count(Button::A, false) == 1, "A released after 6 frames");
    run.frames(logo(true), 114);
    check(run.count(Button::A, true) == 2, "second tap 120 frames later");
    run.frames(logo(false), 10);
    run.frames(logo(true), 100);
    check(run.count(Button::A, true) == 2, "strap count restarts when the reminder goes away");

    // Title: wait for the ready FileSelect scene and the Title milestone.
    run.frames(fileSelect(false), 200);
    check(run.logged("scene Game stage FileSelect"), "FileSelect scene logged");
    run.frames(fileSelect(true), 200);
    run.frame(with(fileSelect(true), "FileSelector.Title"));
    run.frame(with(fileSelect(true), "TitleSequence.BgmPrepare"));
    run.frames(fileSelect(true), 600);
    check(run.count(Button::B, true) == 0, "no title press while the title prepares its BGM");
    run.frame(with(fileSelect(true), "TitleSequence.LogoDisplay"));
    run.frames(fileSelect(true), 9);
    check(run.count(Button::B, true) == 0, "A+B waits 10 frames after LogoDisplay");
    run.frame(fileSelect(true));
    check(run.count(Button::A, true) == 3 && run.count(Button::B, true) == 1, "A and B pressed together on the title");
    const unsigned long pressFrame = run.driver.frame();
    run.frames(fileSelect(true), 19);
    check(run.count(Button::B, false) == 0, "A+B held");
    run.frame(fileSelect(true));
    check(run.count(Button::A, false) == 3 && run.count(Button::B, false) == 1 && run.driver.frame() == pressFrame + 20,
          "A+B released after 20 frames");

    run.frame(with(fileSelect(true), "FileSelector.TitleEnd"));
    check(run.driver.phase() == std::string("waiting for file select"), "TitleEnd accepted");
    run.frames(fileSelect(true), 100);
    check(run.driver.result() == Result::Running && run.quits == 0, "still running before file select");
    run.frame(with(fileSelect(true), "FileSelector.FileSelectStart"));
    check(run.driver.result() == Result::Pass && run.quits == 1, "PASS at file select, power button once");
    check(Smoke::exitStatus(Result::Pass) == 0, "PASS exits 0");
    check(run.driver.reason().find("1 title press") != std::string::npos, "reason records the attempts");
    run.frames(fileSelect(true), 50);
    check(run.quits == 1 && run.count(Button::A, true) == 3, "nothing more after the result");
}

void testTitleRetries() {
    Run run;
    toTitlePress(run);
    run.frame(fileSelect(true));
    check(run.count(Button::B, true) == 1, "first attempt");
    run.frames(fileSelect(true), 300);
    check(run.count(Button::B, true) == 2, "retry 300 frames later");
    run.frames(fileSelect(true), 300);
    check(run.count(Button::B, true) == 3, "third attempt");
    run.frames(fileSelect(true), 300);
    check(run.driver.result() == Result::Fail && run.quits == 1, "FAIL after three unanswered attempts");
    check(run.driver.reason().find("did not accept A+B") != std::string::npos, "reason names the title");
    check(Smoke::exitStatus(Result::Fail) == 1, "FAIL exits 1");
}

void testTitleBgmStall() {
    Run run;
    run.frames(logo(false), 5);
    run.frames(fileSelect(true), 10);
    run.frame(with(fileSelect(true), "TitleSequence.BgmPrepare"));
    run.frames(fileSelect(true), 1799);
    check(run.driver.result() == Result::Running && run.count(Button::B, true) == 0, "waiting for the title BGM");
    run.frame(fileSelect(true));
    check(run.driver.result() == Result::Fail && run.driver.reason().find("BgmPrepare") != std::string::npos &&
              run.driver.reason().find("STM_TITLE") != std::string::npos,
          "FAIL names the BgmPrepare stall after 1800 frames");

    Run late;
    late.frames(fileSelect(true), 5);
    late.frame(with(fileSelect(true), "TitleSequence.BgmPrepare"));
    late.frames(fileSelect(true), 1000);
    late.frame(with(fileSelect(true), "TitleSequence.LogoDisplay"));
    late.frames(fileSelect(true), 2000);
    check(late.driver.result() != Result::Fail || late.driver.reason().find("BgmPrepare") == std::string::npos,
          "a slow but completed BGM preparation is not a stall");
    check(late.count(Button::B, true) >= 1, "and the title is pressed");
}

void testMiiErrorWindow() {
    Run run;
    toTitlePress(run);
    run.frame(fileSelect(true));
    run.frames(fileSelect(true), 20);
    const int before = run.count(Button::A, true);
    run.frame(with(fileSelect(true), "FileSelector.RFLError"));
    run.frames(fileSelect(true), 89);
    check(run.count(Button::A, true) == before, "no tap before the window settles");
    run.frame(fileSelect(true));
    check(run.count(Button::A, true) == before + 1 && run.logged("Mii error"), "A dismisses the Mii error key window");
    run.frame(with(fileSelect(true), "FileSelector.TitleEnd"));
    run.frame(with(fileSelect(true), "FileSelector.FileSelectStart"));
    check(run.driver.result() == Result::Pass, "then file select passes");
}

void testSavePromptBlocks() {
    Run run;
    Observation o = logo(false);
    o.saveSequence = true;
    run.frames(o, 899);
    check(run.driver.result() == Result::Running && run.logged("save-data sequence active"), "save sequence observed");
    run.frame(o);
    check(run.driver.result() == Result::Blocked && run.quits == 1, "BLOCKED after 900 frames of save sequence");
    check(run.driver.reason().find("pointer") != std::string::npos, "reason explains the Yes/No prompt");
    check(run.count(Button::A, true) == 0, "no blind presses at a save prompt");
    check(Smoke::exitStatus(Result::Blocked) == 2, "BLOCKED exits 2");

    Run shortSave;
    shortSave.frames(o, 500);
    shortSave.frames(logo(false), 10);
    shortSave.frames(o, 500);
    check(shortSave.driver.result() == Result::Running, "separate short save sequences do not block");
}

void testUnexpected() {
    Run scene;
    scene.frames(logo(false), 5);
    Observation other;
    other.scene = "Intermission";
    scene.frame(other);
    check(scene.driver.result() == Result::Fail && scene.driver.reason().find("Intermission") != std::string::npos,
          "unexpected scene fails");

    Run stage;
    Observation galaxy = fileSelect(true);
    galaxy.stage = "AstroGalaxy";
    stage.frame(galaxy);
    check(stage.driver.result() == Result::Fail && stage.driver.reason().find("AstroGalaxy") != std::string::npos,
          "unexpected stage fails");

    Run limit(100);
    limit.frames(logo(true), 100);
    check(limit.driver.result() == Result::Fail && limit.driver.reason().find("frame limit 100") != std::string::npos &&
              limit.driver.reason().find("logo") != std::string::npos,
          "frame limit fails with the phase");
}

void testReleaseAfterResult() {
    Run run;
    toTitlePress(run);
    run.frame(fileSelect(true));
    run.frames(fileSelect(true), 5);
    run.frame(with(fileSelect(true), "FileSelector.FileSelectStart"));
    check(run.driver.result() == Result::Pass, "pass while A+B are held");
    run.frames(fileSelect(true), 20);
    check(run.count(Button::A, false) == run.count(Button::A, true) && run.count(Button::B, false) == 1,
          "held buttons are released after the result");
}

void testMilestones() {
    const unsigned long start = petari_milestone_count();
    check(petari_milestone_at(start) == nullptr, "no milestone beyond the count");
    petari_milestone("Test.First");
    petari_milestone("Test.Second");
    check(petari_milestone_count() == start + 2, "count");
    check(std::string(petari_milestone_at(start)) == "Test.First" &&
              std::string(petari_milestone_at(start + 1)) == "Test.Second",
          "in order");
    for (int i = 0; i < 70; i++) {
        petari_milestone("Test.Many");
    }
    check(petari_milestone_at(start) == nullptr, "old entries leave the 64-entry history");
    check(std::string(petari_milestone_at(petari_milestone_count() - 1)) == "Test.Many", "latest kept");
}

}  // namespace

int main() {
    testHappyPath();
    testTitleRetries();
    testTitleBgmStall();
    testMiiErrorWindow();
    testSavePromptBlocks();
    testUnexpected();
    testReleaseAfterResult();
    testMilestones();
    std::printf("native app smoke tests passed (%d checks)\n", checks);
    return 0;
}

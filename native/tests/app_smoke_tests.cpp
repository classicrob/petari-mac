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
    int pointerMoves = 0;
    float pointerU = -1.0f, pointerV = -1.0f;
    bool wantedPlayer = false;

    explicit Run(unsigned long limit = 100000, Smoke::Script script = Smoke::Script::Title) : driver(limit, script) {}

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
            if (step.pointer) {
                ++pointerMoves;
                pointerU = step.pointerU;
                pointerV = step.pointerV;
            }
            wantedPlayer = wantedPlayer || driver.wantsPlayer();
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
    run.frame(with(fileSelect(true), "FileSelector.FileSelect"));
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
    run.frame(with(fileSelect(true), "FileSelector.FileSelect"));
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
    run.frame(with(fileSelect(true), "FileSelector.FileSelect"));
    check(run.driver.result() == Result::Pass, "pass while A+B are held");
    run.frames(fileSelect(true), 20);
    check(run.count(Button::A, false) == run.count(Button::A, true) && run.count(Button::B, false) == 1,
          "held buttons are released after the result");
}

Observation target(Observation o, const char* id, int index, float u, float v, unsigned flags) {
    o.targets.push_back({id, index, u, v, flags});
    return o;
}

Observation prompt(Observation o, const char* id, int type) {
    o.prompts.push_back({id, type});
    return o;
}

constexpr unsigned kSel = Smoke::kTargetSelectable;
constexpr unsigned kPoint = Smoke::kTargetPointing;
constexpr unsigned kEmpty = Smoke::kTargetEmpty;

// Title, then file select, in playable mode.
void toFileSelect(Run& run) {
    toTitlePress(run);
    run.frame(fileSelect(true));
    run.frames(fileSelect(true), 20);
    run.frame(with(fileSelect(true), "FileSelector.TitleEnd"));
    run.frame(with(fileSelect(true), "FileSelector.FileSelect"));
}

// Aims at a target until the game reports it pointed for 3 frames; returns the A presses made.
int pointAndPress(Run& run, Observation base, const char* id, int index, unsigned extraFlags = 0) {
    const int before = run.count(Button::A, true);
    run.frames(target(base, id, index, 0.3f, 0.6f, kSel | extraFlags), 5);  // pointer travelling
    run.frames(target(base, id, index, 0.3f, 0.6f, kSel | kPoint | extraFlags), 3);
    return run.count(Button::A, true) - before;
}

void testPlayableFlow() {
    Run run(1000000, Smoke::Script::Playable);
    toFileSelect(run);
    check(run.driver.result() == Result::Running && run.logged("creating a file"), "playable continues past file select");

    // Lowest empty slot: slot 0 is used, slot 1 and 2 are empty.
    Observation slots = fileSelect(true);
    slots = target(slots, "FileSelect.Slot", 0, 0.1f, 0.5f, kSel);
    slots = target(slots, "FileSelect.Slot", 2, 0.7f, 0.5f, kSel | kEmpty);
    slots = target(slots, "FileSelect.Slot", 1, 0.4f, 0.5f, kSel | kEmpty);
    const int aBefore = run.count(Button::A, true);
    run.frames(slots, 10);
    check(run.pointerU == 0.4f && run.pointerV == 0.5f, "aims at the lowest empty slot");
    check(run.count(Button::A, true) == aBefore, "no A until the game reports the pointer over it");
    Observation pointed = fileSelect(true);
    pointed = target(pointed, "FileSelect.Slot", 1, 0.4f, 0.5f, kSel | kEmpty | kPoint);
    run.frames(pointed, 2);
    check(run.count(Button::A, true) == aBefore, "pointing must hold 3 frames");
    run.frame(pointed);
    check(run.count(Button::A, true) == aBefore + 1 && run.events.back().focus, "A on the pointed empty slot");

    // Create prompt: yes.
    run.frames(fileSelect(true), 10);
    run.frame(prompt(fileSelect(true), "System_FileSelect001", 2));
    check(run.logged("prompt System_FileSelect001 type 2"), "prompt logged");
    run.frames(fileSelect(true), 20);  // yes/no appearing
    check(run.driver.result() == Result::Running, "waiting for the Yes button");
    check(pointAndPress(run, fileSelect(true), "Prompt.Yes", 0) == 1 && run.logged("answered System_FileSelect001"),
          "System_FileSelect001 answered yes");

    // Save while creating, Mii select, icon prompt, Start.
    Observation saving = with(fileSelect(true), "FileSelector.Create");
    saving.saveSequence = true;
    run.frame(saving);
    saving.milestones.clear();
    run.frames(saving, 100);
    run.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
    check(pointAndPress(run, fileSelect(true), "MiiSelect.Mario", 0) == 1, "Mario icon chosen");
    run.frame(prompt(fileSelect(true), "System_FileSelect013", 2));
    check(pointAndPress(run, fileSelect(true), "Prompt.Yes", 0) == 1, "System_FileSelect013 answered yes");
    run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    check(pointAndPress(run, fileSelect(true), "FileSelect.Start", 0) == 1, "Start chosen");
    run.frame(with(fileSelect(true), "FileSelector.DemoStartWait"));

    // Other scenes and stages are fine now.
    Observation intermission;
    intermission.scene = "Intermission";
    run.frames(intermission, 100);
    Observation garden;
    garden.scene = "Game";
    garden.stage = "PeachCastleGardenGalaxy";
    garden.sceneReady = true;
    run.frames(garden, 50);
    check(run.driver.result() == Result::Running, "scenes after the demo start are allowed");

    // Prologue: A only after page and letter milestones.
    const int aPrologue = run.count(Button::A, true);
    run.frame(with(garden, "Prologue.PictureBook"));
    run.frames(garden, 200);
    check(run.count(Button::A, true) == aPrologue, "no blind prose presses");
    run.frame(with(garden, "PictureBook.PageReady"));
    run.frames(garden, 29);
    check(run.count(Button::A, true) == aPrologue, "page settles 30 frames");
    run.frame(garden);
    check(run.count(Button::A, true) == aPrologue + 1, "A after a page is ready");
    run.frame(with(garden, "PictureBook.PageReady"));
    run.frames(garden, 40);
    run.frame(with(garden, "Prologue.PeachLetter"));
    run.frame(with(garden, "PrologueLetter.Ready"));
    run.frames(garden, 40);
    check(run.count(Button::A, true) == aPrologue + 3, "one A per page and for the letter");
    run.frame(with(garden, "Prologue.Arrive"));
    run.frames(garden, 300);
    check(run.count(Button::A, true) == aPrologue + 3, "the arrival cutscene gets no presses");

    // Game start: Mario moves with the stick.
    run.frame(with(garden, "Prologue.GameStart"));
    Observation mario = garden;
    mario.playerValid = true;
    mario.playerX = 100.0f;
    mario.playerY = 0.0f;
    mario.playerZ = 200.0f;
    run.frames(mario, 119);
    check(run.wantedPlayer && run.count(Button::StickUp, true) == 0, "Mario's position requested; settling first");
    run.frame(mario);
    check(run.count(Button::StickUp, true) == 1 && run.logged("hold stick up"), "stick held up");
    run.frames(mario, 89);
    check(run.count(Button::StickUp, false) == 0, "for 90 frames");
    mario.playerZ = 520.0f;
    run.frame(mario);
    check(run.count(Button::StickUp, false) == 1, "then released");
    run.frames(mario, 29);
    check(run.driver.result() == Result::Running, "measured 30 frames after release");
    run.frame(mario);
    check(run.driver.result() == Result::Pass && run.driver.reason().find("moved 320") != std::string::npos &&
              run.quits == 1,
          "PASS with the measured distance");
}

void testPrologueSameFrame() {
    // The first page can be ready in the same observation as the demo start,
    // or while the driver still waits for it: its tap must not be lost.
    for (int variant = 0; variant < 2; variant++) {
        Run run(1000000, Smoke::Script::Playable);
        toFileSelect(run);
        run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
        run.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
        run.frames(target(fileSelect(true), "MiiSelect.Mario", 0, 0.5f, 0.5f, kSel | kPoint), 3);
        run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
        run.frames(target(fileSelect(true), "FileSelect.Start", 0, 0.5f, 0.5f, kSel | kPoint), 3);
        check(run.driver.phase() == std::string("starting the file"), "reached the demo start");
        const int before = run.count(Button::A, true);
        Observation demo = fileSelect(true);
        if (variant == 0) {
            demo.milestones = {"FileSelector.DemoStartWait", "PictureBook.PageReady"};
            run.frame(demo);
        } else {
            demo.milestones = {"FileSelector.DemoStartWait"};
            run.frame(demo);
            run.frame(with(fileSelect(true), "PictureBook.PageReady"));
        }
        run.frames(fileSelect(true), 40);
        check(run.count(Button::A, true) == before + 1,
              variant == 0 ? "a page ready with the demo start gets its tap" : "a page ready right after it gets its tap");
    }

    // Before the demo starts, page milestones (e.g. an unrelated storybook)
    // are not answered.
    Run early(1000000, Smoke::Script::Playable);
    toFileSelect(early);
    const int before = early.count(Button::A, true);
    early.frame(with(fileSelect(true), "PictureBook.PageReady"));
    early.frames(fileSelect(true), 40);
    check(early.count(Button::A, true) == before, "no page tap before the demo starts");
}

void testFileSelectMilestone() {
    // Title mode passes at FileSelect (TitleEnd leads straight there), not at
    // FileSelectStart, which only the return and cancel paths enter.
    Run title;
    toTitlePress(title);
    title.frame(fileSelect(true));
    title.frame(with(fileSelect(true), "FileSelector.TitleEnd"));
    title.frame(with(fileSelect(true), "FileSelector.FileSelectStart"));
    check(title.driver.result() == Result::Running, "FileSelectStart alone does not pass");
    title.frame(with(fileSelect(true), "FileSelector.FileSelect"));
    check(title.driver.result() == Result::Pass, "FileSelect passes");

    // Playable: back in file select after a slot was chosen (a cancelled
    // prompt) does not restart the script.
    Run run(1000000, Smoke::Script::Playable);
    toFileSelect(run);
    run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    check(run.driver.phase() == std::string("creating the file"), "slot chosen");
    run.frame(with(fileSelect(true), "FileSelector.FileSelect"));
    check(run.driver.phase() == std::string("creating the file") && !run.logged("file select reached; creating a file\n"),
          "a later FileSelect does not restart the script");
}

void testPlayableGuards() {
    Run unknown(1000000, Smoke::Script::Playable);
    toFileSelect(unknown);
    unknown.frame(prompt(fileSelect(true), "System_FileSelect002", 2));
    check(unknown.driver.result() == Result::Blocked &&
              unknown.driver.reason().find("System_FileSelect002") != std::string::npos,
          "an unlisted prompt blocks with its ID");

    Run keyPrompt(1000000, Smoke::Script::Playable);
    toFileSelect(keyPrompt);
    keyPrompt.frame(prompt(fileSelect(true), "System_FileSelect001", 0));
    check(keyPrompt.driver.result() == Result::Blocked, "an allowed ID with the wrong type blocks");

    Run neverPointed(1000000, Smoke::Script::Playable);
    toFileSelect(neverPointed);
    Observation slot = target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty);
    neverPointed.frames(slot, 239);
    check(neverPointed.driver.result() == Result::Running, "aiming");
    neverPointed.frame(slot);
    check(neverPointed.driver.result() == Result::Fail &&
              neverPointed.driver.reason().find("never got over FileSelect.Slot") != std::string::npos &&
              neverPointed.count(Button::A, true) == 1,  // the title's A only
          "FAIL when the pointer never gets over the target");

    Run missing(1000000, Smoke::Script::Playable);
    toFileSelect(missing);
    missing.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel), 600);
    check(missing.driver.result() == Result::Fail && missing.driver.reason().find("(empty)") != std::string::npos,
          "FAIL when no empty slot is shown");

    Run unselectable(1000000, Smoke::Script::Playable);
    toFileSelect(unselectable);
    unselectable.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kEmpty | kPoint), 100);
    check(unselectable.pointerMoves == 0 && unselectable.count(Button::A, true) == 1,
          "targets not selectable are not aimed at");

    Run still(1000000, Smoke::Script::Playable);
    toFileSelect(still);
    still.driver.step(with(fileSelect(true), "FileSelector.MiiSelect"));
    Observation garden;
    garden.scene = "Game";
    garden.stage = "PeachCastleGardenGalaxy";
    garden.sceneReady = true;
    garden.playerValid = true;
    // Jump straight to the game start (milestones in one frame).
    Observation start = garden;
    start.milestones = {"FileSelector.FileConfirm", "FileSelector.DemoStartWait", "Prologue.GameStart"};
    still.frames(start, 1);
    still.frames(garden, 400);
    check(still.driver.result() != Result::Pass, "no PASS without movement");

    Run title;  // title mode ignores prompts and targets
    title.frames(prompt(logo(false), "System_FileSelect999", 2), 5);
    check(title.driver.result() == Result::Running, "title mode unchanged by prompts");
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
    testPlayableFlow();
    testPrologueSameFrame();
    testFileSelectMilestone();
    testPlayableGuards();
    testMilestones();
    std::printf("native app smoke tests passed (%d checks)\n", checks);
    return 0;
}

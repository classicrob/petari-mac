// Tests for the smoke script (native/app/smoke.hpp) and the milestone history
// (petari/milestone.hpp): the presses the script makes for each observed game
// state, their timing, and its pass/fail/blocked decisions.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
    run.frame(with(garden, "ProloguePictureBook.PageReady"));
    run.frames(garden, 29);
    check(run.count(Button::A, true) == aPrologue, "page settles 30 frames");
    run.frame(garden);
    check(run.count(Button::A, true) == aPrologue + 1, "A after a page is ready");
    run.frame(with(garden, "ProloguePictureBook.PageReady"));
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
            demo.milestones = {"FileSelector.DemoStartWait", "ProloguePictureBook.PageReady"};
            run.frame(demo);
        } else {
            demo.milestones = {"FileSelector.DemoStartWait"};
            run.frame(demo);
            run.frame(with(fileSelect(true), "ProloguePictureBook.PageReady"));
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
    early.frame(with(fileSelect(true), "ProloguePictureBook.PageReady"));
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

void testSavingWindow() {
    // Real sequence (app23): Yes on System_FileSelect001, FileSelector.Create,
    // the saving window System_Save01 (blocking) while the file is written,
    // then Mii select.
    Run run(1000000, Smoke::Script::Playable);
    toFileSelect(run);
    run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    run.frame(prompt(fileSelect(true), "System_FileSelect001", 2));
    pointAndPress(run, fileSelect(true), "Prompt.Yes", 0);
    const int aBefore = run.count(Button::A, true);
    Observation saving = with(fileSelect(true), "FileSelector.Create");
    saving.saveSequence = true;
    run.frame(saving);
    saving.milestones.clear();
    run.frame(prompt(saving, "System_Save01", 1));
    check(run.driver.result() == Result::Running && run.logged("saving window System_Save01: no input"),
          "the saving window during file creation is expected");
    run.frames(saving, 120);
    check(run.count(Button::A, true) == aBefore && run.pointerMoves > 0, "no presses for the saving window");
    run.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
    check(pointAndPress(run, fileSelect(true), "MiiSelect.Mario", 0) == 1, "then Mii select continues");

    // Elsewhere, or as another type, it still blocks.
    Run early(1000000, Smoke::Script::Playable);
    toFileSelect(early);
    early.frame(prompt(fileSelect(true), "System_Save01", 1));
    check(early.driver.result() == Result::Blocked, "System_Save01 before the file is created blocks");

    Run wrongType(1000000, Smoke::Script::Playable);
    toFileSelect(wrongType);
    wrongType.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    wrongType.frame(with(fileSelect(true), "FileSelector.Create"));
    wrongType.frame(prompt(fileSelect(true), "System_Save01", 2));
    check(wrongType.driver.result() == Result::Blocked, "System_Save01 as a yes/no window blocks");

    Run other(1000000, Smoke::Script::Playable);
    toFileSelect(other);
    other.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    other.frame(with(fileSelect(true), "FileSelector.Create"));
    other.frame(prompt(fileSelect(true), "System_Save02", 1));
    check(other.driver.result() == Result::Blocked && other.driver.reason().find("System_Save02") != std::string::npos,
          "another blocking window during creation blocks");

    Run later(1000000, Smoke::Script::Playable);
    toFileSelect(later);
    later.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    later.frame(with(fileSelect(true), "FileSelector.Create"));
    later.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
    later.frame(prompt(fileSelect(true), "System_Save01", 1));
    check(later.driver.result() == Result::Blocked, "System_Save01 after Mii select started blocks");
}

void testIconSavingWindow() {
    // Second save: after System_FileSelect013 Yes, FileSelector::exeMiiCreateWait
    // -> storeSetMiiIdUserFile -> startSaveAllUserFileSequence shows
    // System_Save01 again, before FileSelector.FileConfirm.
    auto toMarioChosen = [](Run& run) {
        toFileSelect(run);
        run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
        run.frame(prompt(fileSelect(true), "System_FileSelect001", 2));
        pointAndPress(run, fileSelect(true), "Prompt.Yes", 0);
        run.frame(with(fileSelect(true), "FileSelector.Create"));
        run.frame(prompt(fileSelect(true), "System_Save01", 1));
        run.frames(fileSelect(true), 30);
        run.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
        pointAndPress(run, fileSelect(true), "MiiSelect.Mario", 0);
    };

    Run run(1000000, Smoke::Script::Playable);
    toMarioChosen(run);
    run.frame(prompt(fileSelect(true), "System_FileSelect013", 2));
    pointAndPress(run, fileSelect(true), "Prompt.Yes", 0);
    const int aBefore = run.count(Button::A, true);
    Observation saving = fileSelect(true);
    saving.saveSequence = true;
    run.frame(prompt(saving, "System_Save01", 1));
    check(run.driver.result() == Result::Running && run.log.back() == "saving window System_Save01: no input",
          "the icon save's System_Save01 is expected after 013 Yes");
    run.frames(saving, 100);
    check(run.count(Button::A, true) == aBefore, "no presses while the icon is saved");
    run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    check(pointAndPress(run, fileSelect(true), "FileSelect.Start", 0) == 1, "then Start");

    // Before the icon was confirmed (013 not answered yet), it still blocks.
    Run early(1000000, Smoke::Script::Playable);
    toMarioChosen(early);
    early.frame(prompt(fileSelect(true), "System_Save01", 1));
    check(early.driver.result() == Result::Blocked, "System_Save01 before the icon is confirmed blocks");

    // After FileConfirm, it blocks again.
    Run late(1000000, Smoke::Script::Playable);
    toMarioChosen(late);
    late.frame(prompt(fileSelect(true), "System_FileSelect013", 2));
    pointAndPress(late, fileSelect(true), "Prompt.Yes", 0);
    late.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    late.frame(prompt(fileSelect(true), "System_Save01", 1));
    check(late.driver.result() == Result::Blocked, "System_Save01 after FileConfirm blocks");
}

void testProloguePictureBook() {
    // The prologue's book is ProloguePictureBook: five key waits, each
    // announced by ProloguePictureBook.PageReady, then the letter.
    Run run(1000000, Smoke::Script::Playable);
    toFileSelect(run);
    run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    run.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
    run.frames(target(fileSelect(true), "MiiSelect.Mario", 0, 0.5f, 0.5f, kSel | kPoint), 3);
    run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    run.frames(target(fileSelect(true), "FileSelect.Start", 0, 0.5f, 0.5f, kSel | kPoint), 3);
    Observation garden;
    garden.scene = "Game";
    garden.stage = "PeachCastleGardenGalaxy";
    garden.sceneReady = true;
    run.frame(with(garden, "FileSelector.DemoStartWait"));
    run.frame(with(garden, "Prologue.PictureBook"));
    const int before = run.count(Button::A, true);
    for (int page = 0; page < 5; page++) {
        run.frames(garden, 300);  // the book animates to its next stop
        run.frame(with(garden, "ProloguePictureBook.PageReady"));
        run.frames(garden, 40);
    }
    check(run.count(Button::A, true) == before + 5, "one A per ProloguePictureBook page, five pages");
    run.frame(with(garden, "Prologue.PeachLetter"));
    run.frame(with(garden, "PrologueLetter.Ready"));
    run.frames(garden, 40);
    check(run.count(Button::A, true) == before + 6, "and one for the letter");
    check(run.driver.result() == Result::Running, "still running toward GameStart");
    run.frame(with(garden, "PictureBook.PageReady"));
    run.frames(garden, 40);
    check(run.count(Button::A, true) == before + 6, "the storybook's PictureBook.PageReady gets no tap");
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

// A small simulated Mario for the gameplay checks, driven by the presses the
// driver makes: the stick moves him along z, A jumps (gravity -y), and the
// pause menu opens after Plus is held 12 frames (not while B is held) and
// closes on the next Plus press, as PauseButtonCheckerInGame and PauseMenu do.
struct Sim {
    float x = 100.0f, y = 0.0f, z = 200.0f, vy = 0.0f;
    bool held[6] = {};
    bool pressedA = false, pressedPlus = false;
    bool paused = false;
    int plusFrames = 0;
    int pausedFrames = 0;
    std::vector<std::string> pending;  // milestones for the next observation
    // Faults to inject.
    bool drift = false, noJump = false, downSameWay = false, bHeldWithPlus = false, moveWhilePaused = false,
         neverClose = false;

    Observation observe(const Observation& base) {
        Observation o = base;
        o.playerValid = true;
        o.playerX = x;
        o.playerY = y;
        o.playerZ = z;
        o.playerOnGround = y <= 0.0f;
        o.gravityX = 0.0f;
        o.gravityY = -1.0f;
        o.gravityZ = 0.0f;
        o.demoActive = false;
        o.pausePermitted = !paused;
        const bool b = held[1] || (bHeldWithPlus && held[4]);
        o.padA = held[0];
        o.padB = b;
        o.padPlus = held[4];
        o.padMinus = held[5];
        o.padOperating = held[0] || b;
        o.milestones.insert(o.milestones.end(), pending.begin(), pending.end());
        pending.clear();
        return o;
    }
    void apply(const Smoke::Step& step) {
        for (const Smoke::Press& p : step.presses) {
            const int i = static_cast<int>(p.button);
            if (p.down && !held[i]) {
                if (p.button == Button::A) {
                    pressedA = true;
                }
                if (p.button == Button::Plus) {
                    pressedPlus = true;
                }
            }
            held[i] = p.down;
        }
    }
    void advance() {
        const bool operating = held[0] || held[1] || (bHeldWithPlus && held[4]);
        if (paused) {
            ++pausedFrames;
            if (moveWhilePaused && held[2]) {
                z += 3.0f;
            }
            if (pressedPlus && pausedFrames > 30 && !neverClose) {
                paused = false;
                pending.push_back("PauseMenu.Close");
            }
        } else {
            if ((held[4] || held[5]) && !operating) {
                if (++plusFrames == 12) {
                    paused = true;
                    pausedFrames = 0;
                    pending.push_back("PauseMenu.Open");
                }
            } else {
                plusFrames = 0;
            }
            if (!paused) {
                if (held[2]) {
                    z += 3.0f;
                }
                if (held[3]) {
                    z += downSameWay ? 3.0f : -3.0f;
                }
                if (drift) {
                    x += 0.5f;
                }
                if (pressedA && y <= 0.0f && !noJump) {
                    vy = 12.0f;
                }
                if (vy != 0.0f || y > 0.0f) {
                    y += vy;
                    vy -= 1.0f;
                    if (y <= 0.0f) {
                        y = 0.0f;
                        vy = 0.0f;
                    }
                }
            }
        }
        pressedA = false;
        pressedPlus = false;
    }
};

Observation garden() {
    Observation o;
    o.scene = "Game";
    o.stage = "PeachCastleGardenGalaxy";
    o.sceneReady = true;
    return o;
}

// The fresh-file path to Prologue.GameStart (gameplay script).
void toGameStart(Run& run) {
    toFileSelect(run);
    run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 3);
    run.frame(with(fileSelect(true), "FileSelector.MiiSelect"));
    run.frames(target(fileSelect(true), "MiiSelect.Mario", 0, 0.5f, 0.5f, kSel | kPoint), 3);
    run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    run.frames(target(fileSelect(true), "FileSelect.Start", 0, 0.5f, 0.5f, kSel | kPoint), 3);
    run.frame(with(garden(), "FileSelector.DemoStartWait"));
    run.frame(with(garden(), "Prologue.GameStart"));
}

// Runs the simulation until the result is decided or the frame budget ends.
void simulate(Run& run, Sim& sim, unsigned long frames) {
    for (unsigned long i = 0; i < frames && run.driver.result() == Result::Running; i++) {
        const Smoke::Step step = run.driver.step(sim.observe(garden()));
        for (const Smoke::Press& press : step.presses) {
            run.events.push_back({run.driver.frame(), press.button, press.down, step.assertFocus});
        }
        for (const std::string& line : run.driver.log()) {
            run.log.push_back(line);
        }
        run.quits += step.requestQuit ? 1 : 0;
        sim.apply(step);
        sim.advance();
    }
}

bool allReleased(const Run& run) {
    for (int b = 0; b < 6; b++) {
        int balance = 0;
        for (const Event& e : run.events) {
            if (static_cast<int>(e.button) == b) {
                balance += e.down ? 1 : -1;
            }
        }
        if (balance != 0) {
            return false;
        }
    }
    return true;
}

void testGameplayFlow() {
    Run run(1000000, Smoke::Script::Gameplay);
    toGameStart(run);
    check(run.driver.wantsPlayer(), "gameplay asks for Mario after GameStart");
    Sim sim;
    simulate(run, sim, 3000);
    check(run.driver.result() == Result::Pass, "gameplay checks pass: " + run.driver.reason());
    check(run.logged("gameplay ready at (100") && run.logged("idle 120 frames") && run.logged("drift 0.0"),
          "idle baseline logged with positions");
    check(run.logged("tap A: jump from") && run.logged("landed at") && run.logged("highest"), "jump and landing logged");
    check(run.logged("stick up: (") && run.logged("stick down: (") && run.logged("cosine -1"),
          "opposite moves logged with positions");
    check(run.logged("hold Plus (default binding) 18 frames") && run.logged("game buttons during the pause hold"),
          "pause by holding the default Plus binding, with the game's buttons logged");
    check(run.logged("while paused:") && run.logged("tap Plus: resume") && run.logged("after resuming:"),
          "paused freeze, resume and movement logged");
    check(run.count(Button::Plus, true) == 2 && run.count(Button::Minus, true) == 0, "Plus to pause and to resume");
    check(allReleased(run), "every input released");
    // Order: jump before moving.
    size_t jumpAt = 0, moveAt = 0;
    for (size_t i = 0; i < run.log.size(); i++) {
        if (run.log[i].find("tap A: jump") != std::string::npos) jumpAt = i;
        if (run.log[i].find("hold stick up 45 frames from") != std::string::npos && moveAt == 0) moveAt = i;
    }
    check(jumpAt != 0 && moveAt > jumpAt, "the jump comes before the long moves");
}

void testGameplayFaults() {
    auto runWith = [](void (*fault)(Sim&)) {
        auto run = std::make_unique<Run>(1000000, Smoke::Script::Gameplay);
        toGameStart(*run);
        Sim sim;
        fault(sim);
        simulate(*run, sim, 3000);
        return run;
    };
    auto drift = runWith([](Sim& s) { s.drift = true; });
    check(drift->driver.result() == Result::Fail && drift->driver.reason().find("not still") != std::string::npos,
          "drifting with no input fails the idle baseline");
    auto noJump = runWith([](Sim& s) { s.noJump = true; });
    check(noJump->driver.result() == Result::Fail &&
              noJump->driver.reason().find("did not leave the ground") != std::string::npos,
          "no jump fails");
    auto sameWay = runWith([](Sim& s) { s.downSameWay = true; });
    check(sameWay->driver.result() == Result::Fail && sameWay->driver.reason().find("opposite") != std::string::npos,
          "stick down moving the same way fails");
    auto blocked = runWith([](Sim& s) { s.bHeldWithPlus = true; });
    check(blocked->driver.result() == Result::Fail &&
              blocked->driver.reason().find("no PauseMenu.Open") != std::string::npos &&
              blocked->driver.reason().find("B held") != std::string::npos &&
              blocked->driver.reason().find("operating yes") != std::string::npos,
          "a pause blocked by B reports the game's buttons");
    check(allReleased(*blocked), "inputs released after the pause failure");
    auto paused = runWith([](Sim& s) { s.moveWhilePaused = true; });
    check(paused->driver.result() == Result::Fail &&
              paused->driver.reason().find("while the game was paused") != std::string::npos,
          "moving while paused fails");
    auto stuck = runWith([](Sim& s) { s.neverClose = true; });
    check(stuck->driver.result() == Result::Fail && stuck->driver.reason().find("no PauseMenu.Close") != std::string::npos,
          "a menu that does not close fails");

    // A PauseMenu.Close from before the press (the menu's kill at stage start)
    // does not count as the resume.
    Run stale(1000000, Smoke::Script::Gameplay);
    toGameStart(stale);
    stale.frame(with(garden(), "PauseMenu.Close"));
    stale.frame(with(garden(), "PauseMenu.Open"));
    Sim sim;
    sim.neverClose = true;
    simulate(stale, sim, 3000);
    check(stale.driver.result() == Result::Fail && stale.driver.reason().find("no PauseMenu.Close") != std::string::npos,
          "earlier Open/Close events do not satisfy the pause checks");
}

void testReload() {
    // A copied save: slot 0 used, slot 1 empty. The reload picks slot 0, goes
    // through FileConfirm (no Mii select, no create, no save) to Start.
    auto toStart = [](Run& run) {
        toFileSelect(run);
        Observation slots = fileSelect(true);
        slots = target(slots, "FileSelect.Slot", 1, 0.6f, 0.5f, kSel | kEmpty);
        slots = target(slots, "FileSelect.Slot", 0, 0.3f, 0.5f, kSel | kPoint);
        run.frames(slots, 3);
        check(run.pointerU == 0.3f, "reload aims at the used slot");
        run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
        run.frames(target(fileSelect(true), "FileSelect.Start", 0, 0.5f, 0.5f, kSel | kPoint), 3);
        run.frame(with(garden(), "FileSelector.DemoStartWait"));
    };
    Run run(1000000, Smoke::Script::Reload);
    toStart(run);
    check(run.logged("loading a saved file"), "reload logged");
    // No prologue milestones: the game is playable directly.
    Sim sim;
    simulate(run, sim, 3000);
    check(run.driver.result() == Result::Pass && run.driver.reason().find("reloaded save") != std::string::npos &&
              run.logged("in the game without a prologue"),
          "reload reaches gameplay without a prologue and passes the checks");

    Run prologue(1000000, Smoke::Script::Reload);
    toStart(prologue);
    prologue.frame(with(garden(), "Prologue.PictureBook"));
    Observation book = garden();
    book.playerValid = true;
    book.pausePermitted = false;
    book.demoActive = true;
    const int before = prologue.count(Button::A, true);
    prologue.frames(book, 100);
    prologue.frame(with(book, "ProloguePictureBook.PageReady"));
    prologue.frames(book, 40);
    check(prologue.count(Button::A, true) == before + 1, "a reload's prologue pages are tapped as in playable");
    prologue.frame(with(book, "Prologue.GameStart"));
    Sim sim2;
    simulate(prologue, sim2, 3000);
    check(prologue.driver.result() == Result::Pass, "reload through the prologue passes");

    Run created(1000000, Smoke::Script::Reload);
    toFileSelect(created);
    created.frame(with(fileSelect(true), "FileSelector.Create"));
    check(created.driver.result() == Result::Fail && created.driver.reason().find("created a new file") != std::string::npos,
          "a reload that creates a file fails");

    Run rewrite(1000000, Smoke::Script::Reload);
    toStart(rewrite);
    rewrite.frame(prompt(garden(), "System_Save01", 1));
    check(rewrite.driver.result() == Result::Fail && rewrite.driver.reason().find("rewrote the save") != std::string::npos,
          "a save during the reload fails");

    Run asked(1000000, Smoke::Script::Reload);
    toFileSelect(asked);
    asked.frame(prompt(fileSelect(true), "System_FileSelect001", 2));
    check(asked.driver.result() == Result::Blocked && asked.pointerMoves == 0, "reload answers no prompt");

    Run onlyEmpty(1000000, Smoke::Script::Reload);
    toFileSelect(onlyEmpty);
    onlyEmpty.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.5f, 0.5f, kSel | kEmpty | kPoint), 600);
    check(onlyEmpty.driver.result() == Result::Fail && onlyEmpty.driver.reason().find("(non-empty)") != std::string::npos,
          "reload with no used slot fails rather than creating one");
}

void testReleaseOnFailure() {
    // A result decided while the stick is held releases it in that step.
    Run run(1000000, Smoke::Script::Gameplay);
    toGameStart(run);
    Sim sim;
    // Run to the first stick hold.
    for (int i = 0; i < 3000 && run.count(Button::StickUp, true) == 0; i++) {
        simulate(run, sim, 1);
    }
    check(run.count(Button::StickUp, true) == 1, "stick held");
    Observation bad = sim.observe(garden());
    bad.scene = "Intermission";
    bad.playerValid = false;
    const Smoke::Step step = run.driver.step(bad);
    bool released = false;
    for (const Smoke::Press& press : step.presses) {
        released = released || (press.button == Button::StickUp && !press.down);
    }
    check(run.driver.result() == Result::Fail && released, "a failure releases the held stick at once");
}

// A simulated PeachCastleGarden for the story route: Mario moves at 15 units
// per frame relative to a slowly turning camera, whose reported axes may have
// either sign; the plaza area starts PrologueA, which leaves him at the
// restart point; the castle box starts PrologueB, then HeavensDoorGalaxy.
struct StorySim {
    float x = 13550.0f, z = 11900.0f, yaw = 0.3f;
    float zSign = 1.0f, xSign = 1.0f;  // reported camera axes vs the stick's forward/right
    bool held[8] = {};
    bool pressedA = false;
    int movie = -1;             // playing movie (0 A, 1 B)
    int movieFrames = 0;
    int afterMovie = -1;        // frames since PrologueA ended
    bool heavensDoor = false;
    std::vector<std::string> pending;
    // Faults.
    bool wall = false, talk = false, dies = false, noTriggerA = false, badRestart = false, noEndB = false;
    // Hill gravity on the segment-A climb (x 6000-11500), as measured in story-1:
    // up (-0.28168, 0.896431, -0.342151); the camera pitches with it.
    bool hill = false;
    int triggerDelay = 0;       // frames Mario must stand in the castle box before PrologueB
    int inBox = 0;
    float hillUpY = 0.896431f;  // overridable for a too-steep case
    bool zeroGravity = false, cameraAlongGravity = false, nanPosition = false;
    float speed = 15.0f;

    Observation observe() {
        Observation o;
        o.scene = "Game";
        o.stage = heavensDoor ? "HeavensDoorGalaxy" : "PeachCastleGardenGalaxy";
        o.sceneReady = true;
        o.playerValid = true;
        o.playerX = x;
        o.playerY = 0.0f;
        o.playerZ = z;
        o.playerOnGround = true;
        o.gravityY = -1.0f;
        const bool onHill = hill && afterMovie < 0 && x > 6000.0f && x < 11500.0f;
        float upX = 0.0f, upY = 1.0f, upZ = 0.0f;
        if (onHill) {
            // The measured tilt, rescaled so up.y is hillUpY.
            const float tx = -0.28168f, tz = -0.342151f;
            const float side = std::sqrt(std::max(0.0f, 1.0f - hillUpY * hillUpY)) / std::sqrt(tx * tx + tz * tz);
            upX = tx * side;
            upY = hillUpY;
            upZ = tz * side;
            o.gravityX = -upX;
            o.gravityY = -upY;
            o.gravityZ = -upZ;
        }
        if (zeroGravity && x < 12000.0f) {
            o.gravityX = o.gravityY = o.gravityZ = 0.0f;
        }
        if (nanPosition && x < 12000.0f) {
            o.playerX = std::nanf("");
        }
        const bool inMovie = movie >= 0 || (afterMovie >= 0 && afterMovie < 60);
        o.demoActive = inMovie;
        o.pausePermitted = !inMovie;
        const float fx = std::sin(yaw), fz = std::cos(yaw);   // forward
        const float rx = std::cos(yaw), rz = -std::sin(yaw);  // right
        o.camZx = fx * zSign;
        o.camZz = fz * zSign;
        o.camXx = rx * xSign;
        o.camXz = rz * xSign;
        if (onHill) {
            // The camera looks slightly down the slope: its Z axis gains the
            // up component, so only the projection onto the plane is level.
            o.camZy = 0.3f * zSign;
        }
        if (cameraAlongGravity && x < 12000.0f) {
            o.camZx = upX;
            o.camZy = upY;
            o.camZz = upZ;
        }
        o.talkActive = talk && x < 12000.0f;
        o.playerDead = dies && x < 8000.0f;
        o.milestones = pending;
        pending.clear();
        return o;
    }
    void apply(const Smoke::Step& step) {
        for (const Smoke::Press& p : step.presses) {
            const int i = static_cast<int>(p.button);
            if (p.down && !held[i] && p.button == Button::A) {
                pressedA = true;
            }
            held[i] = p.down;
        }
    }
    void advance() {
        yaw += 0.002f;
        if (movie >= 0) {
            if (++movieFrames == 300) {
                pending.push_back(movie == 0 ? "Movie.PrologueA.End" : "Movie.PrologueB.End");
                if (movie == 0) {
                    x = badRestart ? 9000.0f : -500.0f;
                    z = badRestart ? 9000.0f : 6250.0f;
                    afterMovie = 0;
                } else {
                    heavensDoor = true;
                    if (noEndB) {
                        pending.pop_back();  // the stage changed before MovieStarter saw the end
                    }
                    pending.push_back("Stage.HeavensDoorGalaxy");
                }
                movie = -1;
            }
            return;
        }
        if (afterMovie >= 0) {
            ++afterMovie;
        }
        const float fx = std::sin(yaw), fz = std::cos(yaw);
        const float rx = std::cos(yaw), rz = -std::sin(yaw);
        float dx = 0.0f, dz = 0.0f;
        if (held[2]) { dx += fx; dz += fz; }
        if (held[3]) { dx -= fx; dz -= fz; }
        if (held[7]) { dx += rx; dz += rz; }
        if (held[6]) { dx -= rx; dz -= rz; }
        const float n = std::sqrt(dx * dx + dz * dz);
        if (n > 0.0f && !(wall && x < 12200.0f)) {
            x += dx / n * speed;
            z += dz / n * speed;
        }
        pressedA = false;
        const bool plaza = afterMovie < 0 && std::hypot(x + 650.0f, z - 4350.0f) < 700.0f;
        const bool castle = afterMovie >= 60 && x >= -7650.0f && x < -6350.0f && z >= -11408.0f && z < -6408.0f;
        if (plaza && !noTriggerA) {
            movie = 0;
            movieFrames = 0;
            pending.push_back("Movie.PrologueA.Start");
        } else if (castle && !heavensDoor && ++inBox > triggerDelay) {
            movie = 1;
            movieFrames = 0;
            pending.push_back("Movie.PrologueB.Start");
        }
    }
};

// The reload path to the file's start (as in testReload), for the story script.
void toStoryStart(Run& run) {
    toFileSelect(run);
    run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.3f, 0.5f, kSel | kPoint), 3);
    run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    run.frames(target(fileSelect(true), "FileSelect.Start", 0, 0.5f, 0.5f, kSel | kPoint), 3);
    run.frame(with(garden(), "FileSelector.DemoStartWait"));
    run.frame(with(garden(), "Prologue.GameStart"));
}

void simulateStory(Run& run, StorySim& sim, unsigned long frames) {
    for (unsigned long i = 0; i < frames && run.driver.result() == Result::Running; i++) {
        const Smoke::Step step = run.driver.step(sim.observe());
        for (const Smoke::Press& press : step.presses) {
            run.events.push_back({run.driver.frame(), press.button, press.down, step.assertFocus});
        }
        for (const std::string& line : run.driver.log()) {
            run.log.push_back(line);
        }
        run.quits += step.requestQuit ? 1 : 0;
        sim.apply(step);
        sim.advance();
    }
}

bool allKeysReleased(const Run& run) {
    for (int b = 0; b < 8; b++) {
        int balance = 0;
        for (const Event& e : run.events) {
            if (static_cast<int>(e.button) == b) {
                balance += e.down ? 1 : -1;
            }
        }
        if (balance != 0) {
            return false;
        }
    }
    return true;
}

void testStickKeys() {
    const Smoke::StickKeys up = Smoke::stickKeysFor(0.0f, 1.0f);
    check(up.up && !up.down && !up.left && !up.right, "straight ahead: up");
    const Smoke::StickKeys diag = Smoke::stickKeysFor(1.0f, 1.0f);
    check(diag.up && diag.right && !diag.left && !diag.down, "45 degrees: up and right");
    const Smoke::StickKeys near = Smoke::stickKeysFor(0.2f, 1.0f);
    check(near.up && !near.right, "11 degrees off: up only");
    const Smoke::StickKeys back = Smoke::stickKeysFor(-1.0f, -0.1f);
    check(back.left && !back.down && !back.up, "left");
    const Smoke::StickKeys none = Smoke::stickKeysFor(0.0f, 0.0f);
    check(!none.up && !none.down && !none.left && !none.right, "no direction: no keys");
}

void testStoryRoute() {
    for (int signs = 0; signs < 4; signs++) {
        Run run(10000000, Smoke::Script::Story);
        toStoryStart(run);
        StorySim sim;
        sim.zSign = (signs & 1) ? -1.0f : 1.0f;
        sim.xSign = (signs & 2) ? -1.0f : 1.0f;
        simulateStory(run, sim, 40000);
        const std::string label = " (camera signs " + std::to_string(signs) + ")";
        check(run.driver.result() == Result::Pass && run.driver.reason().find("HeavensDoorGalaxy") != std::string::npos,
              "the story route reaches HeavensDoorGalaxy" + label + ": " + run.driver.reason());
        // The movies start as Mario enters their trigger areas, which can be
        // before the last waypoint.
        check(run.logged("calibration: stick up moves along") && run.logged("segment 0 waypoint 6 reached") &&
                  run.logged("segment 1 waypoint 16 reached"),
              "calibrated and walked both routes" + label);
        check(run.logged("Movie.PrologueA.Start at") && run.logged("Movie.PrologueA.End after") &&
                  run.logged("units from the restart point") && run.logged("Movie.PrologueB.End after"),
              "both movies and the restart point logged" + label);
        check(allKeysReleased(run), "every key released" + label);
    }
    // Tilted gravity on the hill (the story-1 failure) is walked through.
    for (int signs = 0; signs < 2; signs++) {
        Run hill(10000000, Smoke::Script::Story);
        toStoryStart(hill);
        StorySim tilted;
        tilted.hill = true;
        tilted.zSign = signs ? -1.0f : 1.0f;
        simulateStory(hill, tilted, 40000);
        check(hill.driver.result() == Result::Pass,
              "the route passes on a hill with gravity tilted 26 degrees: " + hill.driver.reason());
    }

    // PrologueB starting some frames after Mario stopped at the last waypoint
    // (inside the box) is still recognised.
    Run delayed(10000000, Smoke::Script::Story);
    toStoryStart(delayed);
    StorySim late;
    late.triggerDelay = 300;
    simulateStory(delayed, late, 40000);
    check(delayed.driver.result() == Result::Pass && delayed.logged("Movie.PrologueB.Start at"),
          "a movie starting while the driver waits at the last waypoint is recognised: " + delayed.driver.reason());

    // The stage change can come without Movie.PrologueB.End.
    Run noEnd(10000000, Smoke::Script::Story);
    toStoryStart(noEnd);
    StorySim sim;
    sim.noEndB = true;
    simulateStory(noEnd, sim, 40000);
    check(noEnd.driver.result() == Result::Pass && noEnd.logged("without Movie.PrologueB.End"),
          "HeavensDoorGalaxy after PrologueB passes without its End milestone");
    // Reload behaviour stays: a new file or a rewrite is refused.
    Run created(1000000, Smoke::Script::Story);
    toFileSelect(created);
    created.frame(with(fileSelect(true), "FileSelector.Create"));
    check(created.driver.result() == Result::Fail, "the story route never creates a file");
}

void testStoryFaults() {
    auto runWith = [](void (*fault)(StorySim&)) {
        auto run = std::make_unique<Run>(10000000, Smoke::Script::Story);
        toStoryStart(*run);
        StorySim sim;
        fault(sim);
        simulateStory(*run, sim, 40000);
        return run;
    };
    auto wall = runWith([](StorySim& s) { s.wall = true; });
    check(wall->driver.result() == Result::Fail && wall->driver.reason().find("stuck near segment 0") != std::string::npos,
          "a wall fails as stuck, with the waypoint: " + wall->driver.reason());
    check(wall->logged("stuck: sidestep") && wall->logged("stuck: jump"), "recoveries tried before failing");
    check(allKeysReleased(*wall), "keys released after being stuck");
    auto talk = runWith([](StorySim& s) { s.talk = true; });
    check(talk->driver.result() == Result::Fail && talk->driver.reason().find("a talk opened") != std::string::npos,
          "an open talk fails");
    auto dies = runWith([](StorySim& s) { s.dies = true; });
    check(dies->driver.result() == Result::Fail && dies->driver.reason().find("Mario died") != std::string::npos,
          "death fails");
    auto noMovie = runWith([](StorySim& s) { s.noTriggerA = true; });
    check(noMovie->driver.result() == Result::Fail &&
              noMovie->driver.reason().find("no Movie.PrologueA.Start") != std::string::npos,
          "no movie at the trigger fails");
    auto restart = runWith([](StorySim& s) { s.badRestart = true; });
    check(restart->driver.result() == Result::Fail &&
              restart->driver.reason().find("from the restart point") != std::string::npos,
          "a wrong position after PrologueA fails");
    auto zero = runWith([](StorySim& s) { s.zeroGravity = true; });
    check(zero->driver.result() == Result::Fail && zero->driver.reason().find("no gravity direction") != std::string::npos,
          "zero gravity fails: " + zero->driver.reason());
    auto alongGravity = runWith([](StorySim& s) {
        s.hill = true;
        s.cameraAlongGravity = true;
    });
    check(alongGravity->driver.result() == Result::Fail &&
              alongGravity->driver.reason().find("camera axis along gravity") != std::string::npos,
          "a camera axis along gravity fails: " + alongGravity->driver.reason());
    auto steep = runWith([](StorySim& s) {
        s.hill = true;
        s.hillUpY = 0.3f;
    });
    check(steep->driver.result() == Result::Fail && steep->driver.reason().find("more than 60 degrees") != std::string::npos,
          "gravity beyond 60 degrees fails: " + steep->driver.reason());
    auto nan = runWith([](StorySim& s) { s.nanPosition = true; });
    check(nan->driver.result() == Result::Fail && nan->driver.reason().find("non-finite") != std::string::npos,
          "a non-finite position fails: " + nan->driver.reason());
    check(allKeysReleased(*steep) && allKeysReleased(*nan), "keys released after geometry failures");

    auto slow = runWith([](StorySim& s) { s.speed = 0.9f; });
    check(slow->driver.result() == Result::Fail &&
              (slow->driver.reason().find("took over") != std::string::npos ||
               slow->driver.reason().find("stuck") != std::string::npos),
          "a segment that cannot finish in time fails: " + slow->driver.reason());

    Run prompt(10000000, Smoke::Script::Story);
    toStoryStart(prompt);
    StorySim sim;
    simulateStory(prompt, sim, 200);
    Observation withPrompt = sim.observe();
    withPrompt.prompts.push_back({"System_FileSelect001", 2});
    prompt.frame(withPrompt);
    check(prompt.driver.result() == Result::Blocked, "any prompt stops the story route");
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
    testSavingWindow();
    testIconSavingWindow();
    testProloguePictureBook();
    testGameplayFlow();
    testGameplayFaults();
    testReload();
    testReleaseOnFailure();
    testStickKeys();
    testStoryRoute();
    testStoryFaults();
    testPlayableGuards();
    testMilestones();
    std::printf("native app smoke tests passed (%d checks)\n", checks);
    return 0;
}

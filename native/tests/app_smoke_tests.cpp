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
#include "../app/smoke_goodegg.hpp"
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

    // A start without the prologue that opens with a demo (Luigi's fresh file at
    // the Gateway): A once a second after 300 frames, until the game is playable.
    Run opening(1000000, Smoke::Script::Reload);
    toStart(opening);
    Observation demo = garden();
    demo.stage = "HeavensDoorGalaxy";
    demo.demoActive = true;
    const int aBefore = opening.count(Button::A, true);
    opening.frames(demo, 250);
    check(opening.count(Button::A, true) == aBefore, "no presses early in an opening demo");
    opening.frames(demo, 200);
    check(opening.count(Button::A, true) >= aBefore + 2 && opening.logged("advancing the opening demo"),
          "A advances an opening demo without a prologue");
    Observation calm = garden();
    calm.stage = "HeavensDoorGalaxy";
    const int aCalm = opening.count(Button::A, true);
    opening.frames(calm, 30);
    check(opening.count(Button::A, true) == aCalm, "no demo presses once the demo ends");

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

// PETARI_SMOKE_PLAYER=luigi: after the file is chosen, FileSelect.Bros (index 0
// Mario, 1 Luigi) is pressed until it reports Luigi, and only then Start.
void testReloadLuigi() {
    auto toConfirm = [](Run& run) {
        toFileSelect(run);
        run.frames(target(fileSelect(true), "FileSelect.Slot", 0, 0.3f, 0.5f, kSel | kPoint), 3);
        run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    };
    // The file-confirm screen: Start and the switch, with the switch at index.
    auto confirm = [](int brosIndex, unsigned brosFlags) {
        Observation o = target(fileSelect(true), "FileSelect.Start", 0, 0.8f, 0.9f, kSel);
        return brosIndex < 0 ? o : target(o, "FileSelect.Bros", brosIndex, 0.2f, 0.9f, kSel | brosFlags);
    };

    setenv("PETARI_SMOKE_PLAYER", "luigi", 1);
    Run luigi(1000000, Smoke::Script::Reload);
    toConfirm(luigi);
    const int before = luigi.count(Button::A, true);
    luigi.frames(confirm(0, 0), 5);
    luigi.frames(confirm(0, kPoint), 3);
    check(luigi.count(Button::A, true) == before + 1 && luigi.pointerU == 0.2f, "Luigi: the switch is pressed once, not Start");
    luigi.frames(confirm(-1, 0), 40);  // the switch hides while it animates
    luigi.frames(confirm(1, 0), 2);
    check(luigi.logged("Luigi selected with FileSelect.Bros after 1 press"), "Luigi: the switch reporting Luigi is logged");
    check(pointAndPress(luigi, target(fileSelect(true), "FileSelect.Bros", 1, 0.2f, 0.9f, kSel), "FileSelect.Start", 0) == 1,
          "Luigi: then Start");
    luigi.frame(with(garden(), "FileSelector.DemoStartWait"));
    check(luigi.driver.result() == Result::Running, "Luigi: the run continues into the game");

    Run locked(1000000, Smoke::Script::Reload);
    toConfirm(locked);
    locked.frames(confirm(-1, 0), 700);
    check(locked.driver.result() == Result::Fail && locked.driver.reason().find("FileSelect.Bros") != std::string::npos,
          "Luigi on a file without Luigi (no switch shown) fails rather than playing Mario");

    Run stuck(1000000, Smoke::Script::Reload);
    toConfirm(stuck);
    for (int i = 0; i < 4; i++) {
        stuck.frames(confirm(0, kPoint), 70);  // pressed, but it keeps reporting Mario
    }
    check(stuck.driver.result() == Result::Fail && stuck.driver.reason().find("did not switch to Luigi after 3 presses") != std::string::npos,
          "a switch that never reports Luigi fails after 3 presses");
    unsetenv("PETARI_SMOKE_PLAYER");

    Run mario(1000000, Smoke::Script::Reload);
    toConfirm(mario);
    const int marioBefore = mario.count(Button::A, true);
    mario.frames(target(target(fileSelect(true), "FileSelect.Start", 0, 0.8f, 0.9f, kSel | kPoint), "FileSelect.Bros", 0, 0.2f, 0.9f, kSel), 3);
    check(mario.count(Button::A, true) == marioBefore + 1 && mario.pointerU == 0.8f, "unset: Start directly, the switch untouched");
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

// After PrologueA, from the game worker's collision grid: the 70-80 unit curb
// north of story-3/6's first stall and the building north of the second.
struct Box {
    float x0, x1, z0, z1;
};
const Box kTownObstacles[] = {{-4836.0f, -3450.0f, 2270.0f, 2380.0f}, {-5705.0f, -5105.0f, 810.0f, 1060.0f}};

bool inTownObstacle(float x, float z) {
    for (const Box& b : kTownObstacles) {
        if (x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1) {
            return true;
        }
    }
    return false;
}

// Closest approach of a walk through the points to the obstacles (sampled
// every 10 units).
float townClearance(const std::vector<Smoke::Waypoint>& points) {
    float best = 1e30f;
    for (size_t i = 0; i + 1 < points.size(); i++) {
        const Smoke::Waypoint a = points[i], b = points[i + 1];
        const int steps = std::max(1, static_cast<int>(std::hypot(b.x - a.x, b.z - a.z) / 10.0f));
        for (int s = 0; s <= steps; s++) {
            const float t = static_cast<float>(s) / steps;
            const float x = a.x + (b.x - a.x) * t, z = a.z + (b.z - a.z) * t;
            for (const Box& box : kTownObstacles) {
                const float ox = std::max({box.x0 - x, 0.0f, x - box.x1});
                const float oz = std::max({box.z0 - z, 0.0f, z - box.z1});
                best = std::min(best, std::hypot(ox, oz));
            }
        }
    }
    return best;
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
    int movieLength[2] = {300, 300};  // Start to End, in frames
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
    // Scene changes: Mario absent while a movie plays; HeavensDoorGalaxy
    // loading (not ready, no player) for stageLoad frames from PrologueB's end;
    // then, until flickerUntil frames after the load, not ready one frame in
    // every flickerEvery.
    bool absentInMovies = false;
    int stageLoad = 0;
    int afterLoad = -1;
    int flickerEvery = 0, flickerUntil = 0;
    int pressesWhileWaiting = 0;  // key downs during a movie or the stage load
    bool townObstacles = true;    // after PrologueA: the curb and the building Mario stalled at
    // Physical input, as the seam reports it: gameplay inputs appear when
    // Mario passes x < assistBelowX (after PrologueA), pointer motion from
    // the start if pointerMoving.
    Smoke::PhysicalInputs physical;
    float assistBelowX = -1e30f;  // never
    bool pointerMoving = false;

    bool stageReady() const {
        if (!heavensDoor || stageLoad > 0) {
            return !heavensDoor;
        }
        return !(flickerEvery > 0 && afterLoad < flickerUntil && afterLoad % flickerEvery == flickerEvery - 1);
    }

    Observation observe() {
        Observation o;
        if (afterMovie >= 0 && x < assistBelowX && physical.gameplay == 0) {
            physical.gameplay = 2;
            std::snprintf(physical.last, sizeof(physical.last), "key Space (A) up");
        }
        if (pointerMoving) {
            ++physical.pointer;
        }
        o.physical = physical;
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
        if ((absentInMovies && movie >= 0) || !stageReady()) {
            o.sceneReady = stageReady();
            o.playerValid = false;
            o.playerX = o.playerZ = 0.0f;
            o.playerDead = true;  // stale: only meaningful with a player
        }
        o.milestones = pending;
        pending.clear();
        return o;
    }
    void apply(const Smoke::Step& step) {
        for (const Smoke::Press& p : step.presses) {
            const int i = static_cast<int>(p.button);
            if (p.down && (movie >= 0 || heavensDoor)) {
                ++pressesWhileWaiting;
            }
            if (p.down && !held[i] && p.button == Button::A) {
                pressedA = true;
            }
            held[i] = p.down;
        }
    }
    void advance() {
        yaw += 0.002f;
        if (heavensDoor) {
            if (stageLoad > 0) {
                --stageLoad;
            } else {
                ++afterLoad;
            }
            return;
        }
        if (movie >= 0) {
            if (++movieFrames == movieLength[movie]) {
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
            const float nx = x + dx / n * speed, nz = z + dz / n * speed;
            if (!(afterMovie >= 0 && townObstacles && inTownObstacle(nx, nz))) {
                x = nx;
                z = nz;
            }
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
                  run.logged("segment 1 waypoint 29 reached"),
              "calibrated and walked both routes" + label);
        check(!run.logged("stuck:"), "the castle walk goes round the curb and the building, no recovery" + label);
        check(run.logged("Movie.PrologueA.Start at") && run.logged("Movie.PrologueA.End after") &&
                  run.logged("units from the restart point") && run.logged("Movie.PrologueB.End after"),
              "both movies and the restart point logged" + label);
        check(allKeysReleased(run), "every key released" + label);
    }
    // The castle walk keeps its 100-unit clearance from the curb and the
    // building, from the restart point on (turning for the next point from
    // up to 100 away cannot reach them); the story-3/6 chain ran into both.
    std::vector<Smoke::Waypoint> walk{{-500.0f, 6250.0f}};
    for (const Smoke::Waypoint& w : Smoke::storyRoute(1)) {
        walk.push_back(w);
    }
    check(townClearance(walk) >= 100.0f, "the castle walk clears the obstacles by 100 units: " +
                                             std::to_string(townClearance(walk)));
    const std::vector<Smoke::Waypoint> oldWalk{{-500.0f, 6250.0f}, {-1300.0f, 6000.0f}, {-2100.0f, 5200.0f},
                                               {-2900.0f, 4400.0f}, {-3500.0f, 3600.0f}, {-4100.0f, 2900.0f},
                                               {-4700.0f, 2100.0f}, {-5300.0f, 1600.0f}, {-5800.0f, 800.0f}};
    check(townClearance(oldWalk) == 0.0f, "the story-3/6 chain crosses them (the check can fail)");
    const Smoke::Waypoint last = Smoke::storyRoute(1).back();
    check(last.x - 80.0f >= -7650.0f && last.x + 80.0f < -6350.0f && last.z + 80.0f < -6408.0f,
          "within 80 of the last castle point is inside the trigger box");

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

    // Movies as long as the real ones: PrologueA took 5779 frames in story-3;
    // PrologueB, about 7205 expected, here with A's full 188 frames of waits.
    Run full(10000000, Smoke::Script::Story);
    toStoryStart(full);
    StorySim real;
    real.movieLength[0] = 5779;
    real.movieLength[1] = 7076 + 188;
    simulateStory(full, real, 40000);
    check(full.driver.result() == Result::Pass && full.logged("Movie.PrologueA.End after 5779 frames") &&
              full.logged("Movie.PrologueB.End after 7264 frames"),
          "movies of the real lengths pass: " + full.driver.reason());
    // A movie still playing 400 frames after its THP should have ended FAILs.
    Run overlong(10000000, Smoke::Script::Story);
    toStoryStart(overlong);
    StorySim hung;
    hung.movieLength[1] = 7076 + 400 + 1;
    simulateStory(overlong, hung, 40000);
    check(overlong.driver.result() == Result::Fail &&
              overlong.driver.reason() == "no Movie.PrologueB.End within 7476 frames",
          "an overlong PrologueB FAILs: " + overlong.driver.reason());
    check(allKeysReleased(overlong), "keys released after an overlong movie");

    // The scenes change under the driver: Mario absent during both movies,
    // HeavensDoorGalaxy loading (not ready, no player) from the frame of
    // PrologueB's end, then ready but dropping out for a frame every 50 until
    // 200 frames in. PASS only after 60 ready frames in a row, and no input
    // while the movies and the load run.
    Run changes(10000000, Smoke::Script::Story);
    toStoryStart(changes);
    StorySim moving;
    moving.absentInMovies = true;
    moving.stageLoad = 900;
    moving.flickerEvery = 50;
    moving.flickerUntil = 200;
    simulateStory(changes, moving, 40000);
    check(changes.driver.result() == Result::Pass, "absent player in movies and stage load passes: " +
                                                       changes.driver.reason());
    check(changes.logged("Movie.PrologueB.End after") && changes.logged("not ready again after 49 ready frames") &&
              moving.afterLoad >= 199 + 60,
          "PASS needs 60 ready frames in a row after the last drop");
    check(moving.pressesWhileWaiting == 0, "no input during the movies and the stage load");
    check(allKeysReleased(changes), "keys released after the stage change");
    // A stage that never stays ready for 60 frames FAILs on the stage clock.
    Run flaky(10000000, Smoke::Script::Story);
    toStoryStart(flaky);
    StorySim unstable;
    unstable.stageLoad = 100;
    unstable.flickerEvery = 50;
    unstable.flickerUntil = 1000000;
    simulateStory(flaky, unstable, 40000);
    check(flaky.driver.result() == Result::Fail &&
              flaky.driver.reason().find("HeavensDoorGalaxy not ready within 3600 frames") == 0,
          "a stage never ready 60 frames in a row FAILs: " + flaky.driver.reason());
    // Mario missing while walking still FAILs.
    Run gone(10000000, Smoke::Script::Story);
    toStoryStart(gone);
    StorySim vanish;
    for (int i = 0; i < 20000 && gone.driver.result() == Result::Running; i++) {
        Observation o = vanish.observe();
        if (gone.logged("waypoint 2 reached")) {
            o.playerValid = false;
        }
        const Smoke::Step step = gone.driver.step(o);
        for (const Smoke::Press& press : step.presses) {
            gone.events.push_back({gone.driver.frame(), press.button, press.down, step.assertFocus});
        }
        for (const std::string& line : gone.driver.log()) {
            gone.log.push_back(line);
        }
        vanish.apply(step);
        vanish.advance();
    }
    check(gone.driver.result() == Result::Fail && gone.driver.reason().find("no player position while") == 0,
          "no player while walking FAILs: " + gone.driver.reason());
    check(allKeysReleased(gone), "keys released after losing the player");

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

void testAssisted() {
    check(Smoke::exitStatus(Result::Assisted) == 3 && std::string(Smoke::resultName(Result::Assisted)) == "ASSISTED",
          "ASSISTED exits 3");

    // Physical gameplay input during the castle walk: the goal is reached,
    // but the result is ASSISTED with the first input's frame and phase.
    Run helped(10000000, Smoke::Script::Story);
    toStoryStart(helped);
    StorySim hand;
    hand.assistBelowX = -4000.0f;
    simulateStory(helped, hand, 40000);
    check(helped.driver.result() == Result::Assisted &&
              helped.driver.reason().find("story route reached HeavensDoorGalaxy") == 0 &&
              helped.driver.reason().find("ASSISTED, not unattended: 2 physical gameplay inputs, first seen at frame ") !=
                  std::string::npos &&
              helped.driver.reason().find("while walking the story route at (-4") != std::string::npos &&
              helped.driver.reason().find("(2 new, latest key Space (A) up)") != std::string::npos,
          "a helped run ends ASSISTED with its first input: " + helped.driver.reason());
    check(helped.logged("physical input: 2 new, latest key Space (A) up, while walking the story route"),
          "physical inputs are logged as they arrive");
    check(allKeysReleased(helped), "keys released after an assisted run");

    // Helped, then failed: FAIL stays FAIL and names the help.
    Run helpedFail(10000000, Smoke::Script::Story);
    toStoryStart(helpedFail);
    StorySim noCastle;
    noCastle.assistBelowX = -1000.0f;
    noCastle.noEndB = true;
    noCastle.movieLength[1] = 100000;  // PrologueB never ends
    simulateStory(helpedFail, noCastle, 40000);
    check(helpedFail.driver.result() == Result::Fail &&
              helpedFail.driver.reason().find("no Movie.PrologueB.End within") == 0 &&
              helpedFail.driver.reason().find("(also assisted: 2 physical gameplay inputs") != std::string::npos,
          "a helped FAIL stays FAIL: " + helpedFail.driver.reason());

    // Pointer motion alone: PASS, reported rather than hidden.
    Run pointed(10000000, Smoke::Script::Story);
    toStoryStart(pointed);
    StorySim mouse;
    mouse.pointerMoving = true;
    simulateStory(pointed, mouse, 40000);
    check(pointed.driver.result() == Result::Pass &&
              pointed.driver.reason().find("no physical gameplay input; the pointer moved") != std::string::npos &&
              pointed.logged("physical pointer motion while"),
          "pointer motion alone is reported, not assistance: " + pointed.driver.reason());

    // Input before the driver's first frame is not the run's; after it, it is.
    Run baseline(100000);
    Observation before;
    before.physical.gameplay = 7;
    before.physical.focus = 1;
    baseline.frames(before, 5);
    check(!baseline.logged("physical input") && !baseline.logged("window focus"), "earlier input is the baseline");
    Observation pressed = before;
    pressed.physical.gameplay = 8;
    std::snprintf(pressed.physical.last, sizeof(pressed.physical.last), "mouse Left (A) down");
    pressed.physical.focus = 2;
    baseline.frames(pressed, 1);
    check(baseline.logged("physical input: 1 new, latest mouse Left (A) down, while") &&
              baseline.logged("window focus changed while"),
          "a new press and a focus change are logged");
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

void testGalaxy() {
    auto start = [](Run& run) {
        toFileSelect(run);
        run.frames(target(fileSelect(true), "FileSelect.Slot", 0, .3f, .5f, kSel | kPoint), 3);
        run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
        run.frames(target(fileSelect(true), "FileSelect.Start", 0, .5f, .5f, kSel | kPoint), 3);
        run.frame(with(garden(), "FileSelector.DemoStartWait"));
        Observation o = garden();
        o.stage = "AstroGalaxy"; o.scenario = 1;
        o.playerValid = o.playerOnGround = o.pausePermitted = true;
        o.playerX = 2825; o.playerY = 787; o.playerZ = -3750;
        run.frame(o);
        const int beforeCalibration = run.count(Button::StickUp, true);
        o.pausePermitted = false;
        run.frames(o, 180);
        check(run.driver.result() == Result::Running && run.count(Button::StickUp, true) == beforeCalibration,
              "initialized observatory with opening camera does not start calibration");
        o.pausePermitted = true;
        run.frames(o, 35);
        o.pausePermitted = false;
        run.frame(o);
        o.pausePermitted = true;
        run.frames(o, 30);
        check(run.count(Button::StickUp, true) == beforeCalibration,
              "observatory calibration requires consecutive ready frames");
        run.frames(o, 30);
        check(run.count(Button::StickUp, true) == beforeCalibration + 1,
              "sixty ready frames starts the forward hold");
        o.demoActive = true;
        run.frames(o, 90);
        check(run.driver.result() == Result::Running &&
              run.count(Button::StickUp, false) == run.count(Button::StickUp, true),
              "readiness lost during calibration releases the held stick and resets calibration");
        o.demoActive = false;
        for (int i = 0; i < 105; ++i) {
            o.playerX += 4; o.playerZ += 4;
            run.frame(o);
        }
        check(run.count(Button::StickUp, true) > 0 && run.count(Button::StickRight, true) > 0,
              "galaxy route calibrates through real stick inputs");
        o.stage = "AstroDome";
        run.frames(o, 65);
        return o;
    };
    auto click = [](Run& run, Observation o, const char* id) {
        const int before = run.count(Button::A, true);
        run.frames(target(o, id, 1, .4f, .3f, kSel), 4);
        check(run.count(Button::A, true) == before, std::string(id) + " waits for real pointing");
        run.frames(target(o, id, 1, .4f, .3f, kSel | kPoint), 3);
        check(run.count(Button::A, true) == before + 1, std::string(id) + " selected with A");
        run.frames(o, 10);
    };
    auto select = [&](Run& run, Observation o, bool newlyAvailable = false) {
        click(run, o, "Dome.BlueStar");
        if (newlyAvailable) {
            click(run, o, "Galaxy.UnlockEggStarGalaxy");
            const int before = run.count(Button::A, true);
            // Stale New publication must not cause a second unlock click.
            run.frames(target(o, "Galaxy.UnlockEggStarGalaxy", 1, .4f, .3f, kSel | kPoint), 5);
            bool quiet = true;
            for (int i = 0; i < 210; ++i) {
                const auto step = run.driver.step(o);
                quiet = quiet && !step.pointer && step.presses.empty();
            }
            run.frames(target(o, "Galaxy.EggStarGalaxy", 1, .4f, .3f, kPoint), 5);
            check(quiet && run.count(Button::A, true) == before && run.driver.result() == Result::Running &&
                      std::string(run.driver.phase()) == "waiting for Good Egg reveal",
                  "New reveal sends no input and waits past 150 frames for selectable Open");
            run.frame(target(o, "Galaxy.EggStarGalaxy", 1, .4f, .3f, kSel));
            check(run.count(Button::A, true) == before, "observed Open ends reveal without clicking yet");
        }
        click(run, o, "Galaxy.EggStarGalaxy");
        click(run, o, "Galaxy.Start");
        // ScenarioSelectScene runs while the destination Game scene is loading;
        // Mario and a decided scenario number are not available yet.
        o.stage = "EggStarGalaxy";
        o.scenario = -1;
        o.sceneReady = o.playerValid = false;
        run.frames(o, 120);
        click(run, o, "Scenario.First");
        check(run.driver.result() == Result::Running &&
                  std::string(run.driver.phase()) == "loading Good Egg mission 1",
              "mission selection accepts observed UI while destination scene is initializing");
    };
    Run run(1000000, Smoke::Script::Galaxy);
    auto dome = start(run);
    auto talking = target(dome, "Talk.Advance", 0, .5f, .5f, kSel);
    talking.talkActive = true;
    int before = run.count(Button::A, true);
    run.frames(talking, 30);
    check(run.count(Button::A, true) == before + 1, "visible lecture page advances once with debounce");
    select(run, dome, true);
    Observation loading = dome;
    loading.sceneReady = loading.playerValid = false;
    run.frames(loading, 120);
    check(run.driver.result() == Result::Running, "galaxy load tolerates missing Mario");
    Observation egg = dome; egg.stage = "EggStarGalaxy";
    Sim sim;
    for (int i = 0; i < 3000 && run.driver.result() == Result::Running; ++i) {
        auto step = run.driver.step(sim.observe(egg));
        sim.apply(step); sim.advance();
    }
    check(run.driver.result() == Result::Pass && run.driver.reason().find("Good Egg mission 1") != std::string::npos,
          "New galaxy reveal, confirmation, mission selection and all gameplay checks pass");
    Run helped(1000000, Smoke::Script::Galaxy);
    auto helpedDome = start(helped);
    select(helped, helpedDome);
    Sim helpedSim;
    Observation helpedEgg = egg;
    helpedEgg.physical.gameplay = 1;
    for (int i = 0; i < 3000 && helped.driver.result() == Result::Running; ++i) {
        auto step = helped.driver.step(helpedSim.observe(helpedEgg));
        helpedSim.apply(step); helpedSim.advance();
    }
    check(helped.driver.result() == Result::Assisted, "already-open galaxy skips unlock, completes gameplay and preserves ASSISTED");
    Run wrong(1000000, Smoke::Script::Galaxy);
    auto wrongDome = start(wrong);
    select(wrong, wrongDome);
    egg.scenario = 2;
    wrong.frame(egg);
    check(wrong.driver.result() == Result::Fail && wrong.driver.reason().find("wrong mission") != std::string::npos,
          "wrong Good Egg mission fails");
    Run stalled(1000000, Smoke::Script::Galaxy);
    auto stalledDome = start(stalled);
    click(stalled, stalledDome, "Dome.BlueStar");
    click(stalled, stalledDome, "Galaxy.UnlockEggStarGalaxy");
    const int unlockPresses = stalled.count(Button::A, true);
    stalled.frames(stalledDome, 600);
    check(stalled.driver.result() == Result::Fail && stalled.driver.reason().find("reveal did not show selectable") != std::string::npos &&
              stalled.count(Button::A, true) == unlockPresses,
          "missing Open after unlock fails within bounded reveal timeout without more clicks");
    Run readiness(1000000, Smoke::Script::Galaxy);
    auto readyDome = start(readiness);
    select(readiness, readyDome);
    auto readyEgg = readyDome; readyEgg.stage = "EggStarGalaxy";
    readiness.frames(readyEgg, 40);
    auto interrupted = readyEgg; interrupted.sceneReady = interrupted.playerValid = false;
    readiness.frame(interrupted);
    readiness.frames(readyEgg, 30);
    check(std::string(readiness.driver.phase()) == "loading Good Egg mission 1",
          "mission loading interruption resets consecutive readiness");
    interrupted = readyEgg; interrupted.talkActive = true;
    readiness.frame(interrupted);
    readiness.frames(readyEgg, 30);
    check(std::string(readiness.driver.phase()) == "loading Good Egg mission 1",
          "mission dialogue interruption resets consecutive readiness");
    readiness.frames(readyEgg, 100);
    readyEgg.playerDead = true;
    readiness.frame(readyEgg);
    check(readiness.driver.result() == Result::Fail && readiness.driver.reason().find("Mario died") != std::string::npos,
          "death after mission readiness fails during gameplay checks");
    Run returned(1000000, Smoke::Script::Galaxy);
    auto back = start(returned); back.stage = "AstroGalaxy";
    returned.frame(back);
    check(returned.driver.result() == Result::Fail, "backing out of Terrace fails");
    Run missing(1000000, Smoke::Script::Galaxy);
    auto empty = start(missing);
    missing.frames(empty, 610);
    check(missing.driver.result() == Result::Fail && missing.driver.reason().find("Dome.BlueStar") != std::string::npos,
          "missing Blue Star fails with target name");
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


// --- Good Egg mission 1 (smoke_goodegg.hpp) ---

struct EggRun {
    Smoke::GoodEggDriver driver;
    std::vector<Event> events;
    std::vector<std::string> log;
    int quits = 0;
    int pointerMoves = 0;

    explicit EggRun(bool synthetic = true, unsigned long limit = 1000000)
        : driver(limit, Smoke::GoodEggConfig{synthetic}) {}

    Smoke::Step frame(const Observation& observation) {
        const Smoke::Step step = driver.step(observation);
        for (const Smoke::Press& press : step.presses) {
            events.push_back({driver.frame(), press.button, press.down, step.assertFocus});
        }
        for (const std::string& line : driver.log()) {
            log.push_back(line);
        }
        quits += step.requestQuit ? 1 : 0;
        pointerMoves += step.pointer ? 1 : 0;
        return step;
    }
    void frames(const Observation& observation, unsigned long count) {
        for (unsigned long i = 0; i < count; i++) {
            frame(observation);
        }
    }
    int count(Button button, bool down) const {
        int n = 0;
        for (const Event& e : events) {
            n += e.button == button && e.down == down;
        }
        return n;
    }
    // The last press or release of the button was a press.
    bool held(Button button) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->button == button) {
                return it->down;
            }
        }
        return false;
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

// Good Egg mission 1 in play: Mario at a point, gravity -y (or +y), camera
// looking along -z with +y up.
Observation egg(float x, float y, float z, bool upsideDown = false) {
    Observation o;
    o.scene = "Game";
    o.stage = "EggStarGalaxy";
    o.scenario = 1;
    o.sceneReady = true;
    o.videoConfigured = true;
    o.videoBlack = false;
    o.playerValid = o.playerOnGround = o.pausePermitted = true;
    o.playerX = x;
    o.playerY = y;
    o.playerZ = z;
    o.gravityY = upsideDown ? 1.0f : -1.0f;
    o.camXx = 1.0f; o.camXy = 0.0f; o.camXz = 0.0f;
    o.camZx = 0.0f; o.camZy = 0.0f; o.camZz = -1.0f;
    o.powerStars = 1;
    o.playerLife = 3;
    return o;
}

Observation actor(Observation o, const char* kind, float x, float y, float z, int state, unsigned flags) {
    Observation::Actor a;
    a.kind = kind;
    a.x = x; a.y = y; a.z = z;
    a.state = state;
    a.flags = flags;
    o.actors.push_back(a);
    return o;
}

// The synthetic-entry boot: title, the saved file, Start, then the stage.
void toGoodEgg(EggRun& run) {
    run.frames(Observation{}, 10);
    run.frames(logo(false), 30);
    run.frames(logo(true), 200);
    run.frames(fileSelect(false), 50);
    run.frame(with(fileSelect(true), "FileSelector.Title"));
    run.frame(with(fileSelect(true), "TitleSequence.BgmPrepare"));
    run.frames(fileSelect(true), 30);
    run.frame(with(fileSelect(true), "TitleSequence.LogoDisplay"));
    run.frames(fileSelect(true), 31);
    run.frame(with(fileSelect(true), "FileSelector.TitleEnd"));
    run.frame(with(fileSelect(true), "FileSelector.FileSelect"));
    run.frames(target(fileSelect(true), "FileSelect.Slot", 0, .3f, .5f, kSel | kPoint), 3);
    run.frame(with(fileSelect(true), "FileSelector.FileConfirm"));
    run.frames(target(fileSelect(true), "FileSelect.Start", 0, .5f, .5f, kSel | kPoint), 3);
    Observation loading = egg(0, 0, 0);
    loading.stage = "FileSelect";
    run.frame(with(loading, "FileSelector.DemoStartWait"));
}

void testGoodEggGeometry() {
    using Smoke::Planet;
    check(Smoke::planetAt({-3265, -13081, -15332}) == Planet::DiskGarden, "mission start is on the Disk Garden");
    check(Smoke::planetAt({-4671, -16600, -16381}) == Planet::DiskGarden, "the stem's bottom is on the Disk Garden");
    check(Smoke::planetAt({-10547.9f, -14915.6f, -2485.1f}) == Planet::Peanut, "a Peanut chip is on the Peanut");
    check(Smoke::planetAt({-18416.7f, -15888.5f, -8672.8f}) == Planet::BeanB, "the Piranha Plant is on Bean B");
    check(Smoke::planetAt({-17716.7f, -10726.9f, -9460.3f}) == Planet::FruitPeel, "the Hammer Head is on the Fruit Peel");
    check(Smoke::planetAt({-18586.7f, -5200.0f, -12112.8f}) == Planet::BeanC, "the crystal is on Bean C");
    check(Smoke::planetAt({-8670.0f, -6174.4f, -37890.0f}) == Planet::Dino, "the launch star lands on Dino Piranha's planet");
    check(Smoke::planetAt({0, 20000, 0}) == Planet::None, "open space is no planet");
    const auto& route = Smoke::diskGardenRoute();
    check(route.size() == 29 && route.back().y < -16500.0f, "the Disk Garden rail ends at the stem's bottom");
    check(Smoke::planetAt(Smoke::fruitPeelRoute().front()) == Planet::FruitPeel &&
              Smoke::planetAt(Smoke::fruitPeelRoute().back()) == Planet::FruitPeel &&
              Smoke::planetAt(Smoke::beanCRoute().front()) == Planet::BeanC &&
              Smoke::planetAt(Smoke::beanCRoute().back()) == Planet::BeanC,
          "the Fruit Peel and Bean C routes lie on their planets");
    for (const auto* r : {&Smoke::diskGardenRoute(), &Smoke::fruitPeelRoute(), &Smoke::beanCRoute()}) {
        float longest = 0.0f;
        for (size_t i = 1; i < r->size(); ++i) {
            const float dx = (*r)[i].x - (*r)[i - 1].x, dy = (*r)[i].y - (*r)[i - 1].y, dz = (*r)[i].z - (*r)[i - 1].z;
            longest = std::max(longest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        check(longest < 600.0f, "route points are close enough to steer between");
    }

    // Stick directions relative to the camera, on the ground and upside down.
    Observation o = egg(0, 0, 0);
    Smoke::StickKeys k = Smoke::stickKeysForWorld(o, {0, 0, -1});
    check(k.up && !k.down && !k.left && !k.right, "away from a level camera is stick up");
    k = Smoke::stickKeysForWorld(o, {1, 0, 0});
    check(k.right && !k.up && !k.down, "camera right is stick right");
    k = Smoke::stickKeysForWorld(o, {0, 5, 0});
    check(!k.up && !k.down && !k.left && !k.right, "straight up has no ground direction");
    Observation down = o;
    down.camZx = 0; down.camZy = -1; down.camZz = 0;  // looking straight down, screen-up is -z
    k = Smoke::stickKeysForWorld(down, {0, 0, -1});
    check(k.up && !k.down, "a camera above Mario: screen-up is stick up");
    Observation under = egg(0, 0, 0, true);  // standing under a disk, camera level
    k = Smoke::stickKeysForWorld(under, {-1, 0, -1});
    check(k.up && k.left, "upside down, a level camera still maps screen directions");
    Observation degenerate = o;
    degenerate.camXx = 0; degenerate.camXy = 1;  // camera right along gravity
    k = Smoke::stickKeysForWorld(degenerate, {1, 0, 0});
    check(!k.up && !k.down && !k.left && !k.right, "degenerate camera axes give no keys");
}

void testGoodEggMission() {
    EggRun run;
    toGoodEgg(run);
    check(run.driver.result() == Result::Running && std::string(run.driver.phase()).find("Good Egg") == std::string::npos,
          "boot still running before the stage");
    run.frame(egg(-3265, -13081, -15332));
    check(run.logged("taking over from the synthetic stage entry"), "synthetic entry hands over on EggStarGalaxy");
    // The start: steering toward the first rail point (south-west, away from the camera and left).
    run.frames(egg(-3265, -13081, -15332), 5);
    check(run.logged("stars at mission start: 1"), "stars counted at the start");
    check(run.logged("on Disk Garden") && run.held(Button::StickUp) && run.held(Button::StickLeft),
          "walks toward the rail with the stick");
    // A demo stops input.
    Observation demo = egg(-3265, -13081, -15332);
    demo.demoActive = true;
    run.frame(demo);
    check(!run.held(Button::StickUp) && !run.held(Button::StickLeft), "a demo releases the stick");

    // Under the floating Luma at the stem's bottom (standing upside down): a
    // run resumed there starts from the nearest rail point, the last.
    const auto& route = Smoke::diskGardenRoute();
    EggRun bottom;
    toGoodEgg(bottom);
    bottom.frames(egg(route.back().x, route.back().y, route.back().z, true), 2);
    check(bottom.logged("Disk Garden rail: from point 28 of 29"), "resumes at the nearest rail point");
    Observation luma = actor(egg(-4675, -16517, -16385, true), "Luma", -4671, -16693, -16381, 0, 0);
    int a = bottom.count(Button::A, true);
    bottom.frames(luma, 30);
    check(bottom.count(Button::A, true) == a && !bottom.held(Button::StickUp) && !bottom.held(Button::StickDown),
          "under the Luma: waits, no A until the game offers the talk");
    bottom.frame(target(luma, "Talk.Start", 0, .5f, .5f, kSel));
    check(bottom.count(Button::A, true) == a + 1, "A when Talk.Start is offered");
    Observation page = target(luma, "Talk.Advance", 0, .5f, .5f, kSel);
    page.talkActive = true;
    bottom.frames(page, 46);
    check(bottom.count(Button::A, true) == a + 2, "talk pages advance with A, debounced");
    Observation sling = actor(egg(-4675, -16517, -16385, true), "SlingStar", -4679, -16727, -16337, 1, Smoke::kActorReady);
    bottom.frame(sling);
    check(bottom.logged("the Luma's talk ended"), "talk end noticed");
    int spins = bottom.count(Button::Spin, true);
    bottom.frames(sling, 2);
    check(bottom.count(Button::Spin, true) == spins + 1, "spin at the ready Sling Star");
    bottom.frames(sling, 20);
    check(bottom.count(Button::Spin, true) == spins + 1, "spins are rate limited");

    // A launch star captures Mario: spin; while bound, nothing.
    Observation captured = actor(egg(-4630, -17500, -16400, true), "LaunchStar", -4631, -17641, -16400, 2, 0);
    captured.playerOnGround = false;
    spins = run.count(Button::Spin, true);
    run.frames(captured, 40);
    check(run.count(Button::Spin, true) >= spins + 1, "spin when the launch star holds Mario");
    Observation flying = egg(-7000, -16000, -9000);
    flying.playerInBind = true;
    flying.playerOnGround = false;
    const size_t before = run.events.size();
    run.frames(flying, 60);
    bool quiet = true;
    for (size_t i = before; i < run.events.size(); ++i) {
        quiet = quiet && !run.events[i].down;
    }
    check(quiet, "no presses while bound in flight");

    // Hanging on a vine: spin every 20 frames, no stick.
    Observation vine = actor(egg(-18425, -15500, -8667), "Vine", -18425, -15480, -8667, 2, Smoke::kActorBound);
    vine.playerOnGround = false;
    spins = run.count(Button::Spin, true);
    run.frames(vine, 61);
    check(run.count(Button::Spin, true) >= spins + 3 && !run.held(Button::StickUp), "climbing: spins, no stick");

    // An unknown prompt is not answered.
    EggRun blocked;
    toGoodEgg(blocked);
    blocked.frame(egg(-3265, -13081, -15332));
    blocked.frame(prompt(egg(-3265, -13081, -15332), "System_Other", 2));
    check(blocked.driver.result() == Result::Blocked && blocked.quits == 1, "unexpected prompt BLOCKED");

    // Death fails with where it happened.
    EggRun died;
    toGoodEgg(died);
    died.frames(egg(-3265, -13081, -15332), 5);
    Observation dead = egg(-3265, -13081, -15332);
    dead.playerDead = true;
    died.frames(dead, 20);
    check(died.driver.result() == Result::Running && died.logged("death 1"), "a first death waits for the restart");
    Observation respawn = egg(-3265, -13081, -15332);
    died.frames(respawn, 31);
    check(died.logged("restarted at") && died.driver.result() == Result::Running, "the restart is noticed, the plan starts over");
    died.frame(dead);
    died.frames(respawn, 31);
    died.frame(dead);
    check(died.driver.result() == Result::Fail && died.driver.reason().find("Mario died 3 times") != std::string::npos &&
              died.driver.reason().find("synthetic stage-fixture entry") != std::string::npos,
          "a third death FAILs and the synthetic entry is named");

    // Not moving at all: recoveries (jumps and sidesteps), then FAIL.
    EggRun stuck;
    toGoodEgg(stuck);
    stuck.frames(egg(-3265, -13081, -15332), 3000);
    check(stuck.driver.result() == Result::Fail && stuck.driver.reason().find("stuck going to Disk Garden rail") != std::string::npos &&
              stuck.count(Button::A, true) >= 4,
          "stuck walking FAILs after jump and sidestep recoveries");

    // Leaving the galaxy without the star.
    EggRun left;
    toGoodEgg(left);
    left.frames(egg(-3265, -13081, -15332), 5);
    Observation dome = egg(0, 0, 0);
    dome.stage = "AstroDome";
    left.frame(dome);
    check(left.driver.result() == Result::Fail && left.driver.reason().find("without the Power Star") != std::string::npos,
          "leaving Good Egg without the star FAILs");

    // The galaxy route does not accept the synthetic shortcut.
    EggRun viaRoute(false);
    toGoodEgg(viaRoute);
    viaRoute.frames(egg(-3265, -13081, -15332), 2);
    check(viaRoute.driver.result() == Result::Fail && viaRoute.driver.reason().find("before the mission") != std::string::npos,
          "galaxy-route mode FAILs if Good Egg appears without the observatory route");
}

// From the Power Star to the saved file in the Terrace.
void testGoodEggReturn() {
    auto touch = [](EggRun& run) {
        toGoodEgg(run);
        run.frames(egg(-8400, -6700, -37800), 5);
        Observation star = actor(egg(-8400, -6700, -37800), "PowerStar", -8400, -6374, -37800, 1, Smoke::kActorReady);
        run.frames(star, 3);
        run.frame(with(egg(-8400, -6700, -37800), "PowerStar.Get"));
        run.frames(Observation{}, 10);
    };
    auto dome = [](int stars, bool recorded) {
        Observation o = egg(0, 0, 0);
        o.stage = "AstroDome";
        o.powerStars = stars;
        o.starEggStar1 = recorded;
        return o;
    };
    EggRun run;
    touch(run);
    check(run.driver.result() == Result::Running && std::string(run.driver.phase()) == "Good Egg: star get",
          "star touched: the star-get sequence plays");
    Observation back = dome(2, true);
    back.demoActive = true;
    run.frames(back, 30);
    check(run.logged("back in AstroDome"), "return noticed");
    const int a = run.count(Button::A, true);
    run.frame(prompt(back, "System_Save00", 2));
    run.frames(target(back, "Prompt.Yes", 0, .4f, .6f, kSel), 5);
    check(run.count(Button::A, true) == a && run.pointerMoves > 0, "points at Yes, no A before the game reports pointing");
    run.frames(target(back, "Prompt.Yes", 0, .4f, .6f, kSel | kPoint), 3);
    check(run.count(Button::A, true) == a + 1, "A on Yes");
    Observation saving = back;
    saving.saveSequence = true;
    run.frame(prompt(saving, "System_Save01", 1));
    run.frames(saving, 30);
    run.frame(prompt(saving, "System_Save02", 0));
    run.frames(saving, 45);
    check(run.count(Button::A, true) == a + 2, "A on the save-finished window");
    check(run.driver.result() == Result::Running, "not before the dome is playable");
    Observation ready = dome(2, true);
    run.frames(ready, 61);
    check(run.driver.result() == Result::Pass && run.quits == 1 &&
              run.driver.reason().find("file records the star (2 stars, was 1)") != std::string::npos,
          "PASS: star recorded, saved, back in the Terrace");

    EggRun unrecorded;
    touch(unrecorded);
    unrecorded.frames(dome(1, false), 5);
    unrecorded.frame(prompt(dome(1, false), "System_Save00", 2));
    unrecorded.frames(target(dome(1, false), "Prompt.Yes", 0, .4f, .6f, kSel | kPoint), 3);
    unrecorded.frame(prompt(dome(1, false), "System_Save02", 0));
    unrecorded.frames(dome(1, false), 130);
    check(unrecorded.driver.result() == Result::Fail && unrecorded.driver.reason().find("does not record") != std::string::npos,
          "a save without the star FAILs");

    EggRun unsaved;
    touch(unsaved);
    unsaved.frames(dome(2, true), 3000);
    check(unsaved.driver.result() == Result::Running, "no PASS without the save");
    unsaved.frames(dome(2, true), 7200);
    check(unsaved.driver.result() == Result::Fail && unsaved.driver.reason().find("save prompt not shown") != std::string::npos,
          "no save prompt: FAIL after the return limit");

    EggRun helped;
    touch(helped);
    Observation helpedDome = dome(2, true);
    helpedDome.physical.gameplay = 1;
    helped.frames(helpedDome, 3);
    helped.frame(prompt(helpedDome, "System_Save00", 2));
    helped.frames(target(helpedDome, "Prompt.Yes", 0, .4f, .6f, kSel | kPoint), 3);
    helped.frame(prompt(helpedDome, "System_Save02", 0));
    helped.frames(helpedDome, 130);
    check(helped.driver.result() == Result::Assisted, "physical input makes the pass ASSISTED");
}

// Planet objectives with static observations: what the driver presses.
void testGoodEggObjectives() {
    // Peanut: a chip floating out of reach overhead gets a jump.
    EggRun chips;
    toGoodEgg(chips);
    // Landing: the tour past all chips starts at its nearest point.
    chips.frames(egg(-9122, -15504, -2193), 2);
    check(chips.logged("on Peanut") && chips.logged("Peanut route: from point 0"), "the Peanut tour starts");
    // At the tour's end with a chip missed: that chip directly, jumping for it.
    EggRun missed;
    toGoodEgg(missed);
    Observation under = actor(egg(-11923, -15385, -2954), "StarChip", -11923, -15200, -2954, 1, Smoke::kActorReady);
    missed.frames(under, 3);
    check(missed.logged("Peanut route done with 0 of 5 chips") && missed.logged("jump for the chip"),
          "a missed chip 185 above: jump");
    // A boulder rolling at Mario along his way (tour point 0 to 1): he steps
    // around it rather than into it; one rolling away changes nothing.
    auto rolling = [](Observation o, float x, float y, float z, float dx, float dy, float dz) {
        o = actor(o, "Rock", x, y, z, 0, Smoke::kActorHostile);
        o.actors.back().dx = dx; o.actors.back().dy = dy; o.actors.back().dz = dz;
        return o;
    };
    // (-9122, -15504, -2193) -> (-9216, -15716, -2408): direction (-0.25, -0.69, -0.68).
    EggRun rock;
    toGoodEgg(rock);
    Observation start = egg(-9122, -15504, -2193);
    rock.frames(start, 3);
    rock.frames(rolling(start, -9310, -15930, -2620, 2.5f, 6.9f, 6.8f), 2);
    check(rock.logged("stepping around a boulder") || rock.logged("waiting for a boulder to pass"),
          "a boulder coming along Mario's way: he does not walk into it");
    EggRun away;
    toGoodEgg(away);
    away.frames(start, 3);
    away.frames(rolling(start, -9310, -15930, -2620, -2.5f, -6.9f, -6.8f), 2);
    check(!away.logged("stepping around") && !away.logged("waiting for a boulder"), "a boulder rolling away is ignored");
    // A crossing outside the old 45-frame lookahead must trigger braking
    // before Mario runs into the rock's path at full speed.
    EggRun early;
    toGoodEgg(early);
    early.frames(start, 3);
    early.frames(rolling(start, -9682, -15504, -3481, 4.0f, 0.0f, 9.2f), 2);
    check(early.logged("stepping around a boulder") || early.logged("waiting for a boulder to pass"),
          "a distant oncoming boulder is considered before entering its crossing");
    // Five chips got: the launch star they form, which floats above the ground.
    EggRun star;
    toGoodEgg(star);
    star.frames(egg(-12100, -15600, -3000), 2);
    for (int i = 0; i < 5; ++i) {
        star.frame(with(egg(-12100, -15600, -3000), "StarChip.Got"));
    }
    star.frames(egg(-12100, -15600, -3000), 2);
    check(star.logged("going to the Peanut launch star"), "after five chips: the launch star");
    const int spins = star.count(Button::Spin, true);
    Observation held = actor(egg(-12216, -15700, -3022), "LaunchStar", -12216.7f, -15424.5f, -3022.8f, 2, 0);
    star.frames(held, 3);
    check(star.count(Button::Spin, true) == spins + 1, "held by the launch star 300 above the feet: spin");

    // Fruit Peel: the head down (READY) and close: spin, then jump onto it.
    EggRun hammer;
    toGoodEgg(hammer);
    // (At the Fruit Peel route's point 27, where it stops: the spiral comes first.)
    Observation down = actor(egg(-18240, -10822, -10449), "HammerHead", -18140, -10800, -10420, 0,
                             Smoke::kActorReady | Smoke::kActorHostile);
    hammer.frames(down, 2);
    check(hammer.count(Button::Spin, true) == 1 && hammer.logged("jump onto the Hammer Head"),
          "Hammer Head's head down: spin and jump onto it");
    EggRun recentSpin;
    toGoodEgg(recentSpin);
    recentSpin.frames(actor(egg(-18240, -10822, -10449), "Karipon", -18230, -10822, -10449, 0,
                           Smoke::kActorHostile), 2);
    recentSpin.frames(egg(-18240, -10822, -10449), 31);
    const int beforeHead = recentSpin.count(Button::Spin, true);
    recentSpin.frames(down, 2);
    check(recentSpin.count(Button::Spin, true) == beforeHead + 1,
          "a previous Karipon spin does not suppress the Hammer Head stun beyond normal spin cooldown");

    Observation jumping = actor(egg(-18173, -10520, -10420), "HammerHead", -18140, -10800, -10420, 0,
                               Smoke::kActorReady | Smoke::kActorHostile);
    jumping.playerOnGround = false;
    hammer.frame(jumping);
    jumping.playerX += 13.0f;
    hammer.frame(jumping);
    check(hammer.held(Button::StickLeft), "jump approaching the head brakes horizontal momentum before overshooting");
    // Up again (not READY): stand off near its base, no jumping at it.
    Observation up = actor(egg(-18240, -10822, -10449), "HammerHead", -17716, -10500, -9460, 0, Smoke::kActorHostile);
    const int jumps = hammer.count(Button::A, true);
    hammer.frames(up, 60);
    check(hammer.count(Button::A, true) == jumps && hammer.logged("going to the Hammer Head's baiting spot"),
          "Hammer Head up: back off to the baiting spot ~600 from its base, no jumping at it");

    // Dino Piranha: ball ready behind it and within reach: spin; ball flying: move away.
    EggRun dino;
    toGoodEgg(dino);
    Observation fight = actor(egg(-8400, -6680, -37700), "DinoPiranha", -8400, -6690, -38100, 3, Smoke::kActorHostile);
    fight.actors.back().dz = -1.0f;  // facing away from Mario
    fight = actor(fight, "DinoBall", -8400, -6690, -37750, 1, Smoke::kActorReady);
    dino.frames(fight, 2);
    check(dino.count(Button::Spin, true) == 1 && dino.logged("Dino Piranha phase 3"), "tail ball in reach: spin");
    Observation flying = fight;
    flying.actors.back().flags = 0;
    dino.frames(flying, 3);
    check(dino.logged("going to away from Dino Piranha"), "ball flying back: keep away");
    // A ready Power Star 300 above: jump under it.
    Observation starOver = actor(egg(-8400, -6680, -37800), "PowerStar", -8400, -6374, -37800, 1, Smoke::kActorReady);
    dino.frames(starOver, 2);
    check(dino.logged("jump for the Power Star"), "Power Star overhead: jump");
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
    testReloadLuigi();
    testReleaseOnFailure();
    testStickKeys();
    testStoryRoute();
    testStoryFaults();
    testAssisted();
    testPlayableGuards();
    testGalaxy();
    testMilestones();
    testGoodEggGeometry();
    testGoodEggMission();
    testGoodEggReturn();
    testGoodEggObjectives();
    std::printf("native app smoke tests passed (%d checks)\n", checks);
    return 0;
}

// The smoke script (smoke.hpp). No SDK or Aurora headers: the frame seam
// feeds it observations and applies its presses.

#include "smoke.hpp"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern char** environ;

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kStrapFirstTap = 480;    // game accepts a button after 450
constexpr unsigned long kStrapTapInterval = 120;
constexpr unsigned long kTapFrames = 6;          // at least one KPAD read at 60 Hz
constexpr unsigned long kLogoDisplaySettle = 10;   // after TitleSequence.LogoDisplay (A+B is read from then)
constexpr unsigned long kBgmPrepareLimit = 1800;  // STM_TITLE must be prepared within 30 s
constexpr unsigned long kTitleHoldFrames = 20;
constexpr unsigned long kTitleRetryFrames = 300;
constexpr int kTitleAttempts = 3;
constexpr unsigned long kRflTapDelay = 90;
constexpr unsigned long kSaveBlockedFrames = 900;
// playable
constexpr unsigned long kPointingFramesToPress = 3;  // pointer reported over the target this long
constexpr unsigned long kAimLimit = 240;             // frames aiming at a shown target
constexpr unsigned long kTargetMissingLimit = 600;   // frames waiting for a target to be shown
constexpr unsigned long kMilestoneLimit = 3600;
constexpr unsigned long kDemoLimit = 600;
constexpr unsigned long kPrologueTapDelay = 30;
constexpr unsigned long kPrologueStallLimit = 3600;
constexpr unsigned long kMoveSettle = 120;
constexpr unsigned long kMoveHold = 90;
constexpr unsigned long kMoveAfter = 30;
constexpr float kMoveMinimum = 50.0f;
// gameplay
constexpr unsigned long kReadyFrames = 60;
constexpr unsigned long kReadyLimit = 3600;
constexpr unsigned long kIdleFrames = 120;
constexpr float kIdleDrift = 5.0f;
constexpr unsigned long kJumpLeaveLimit = 30;
constexpr unsigned long kJumpLandLimit = 180;
constexpr float kJumpMinimumRise = 60.0f;
constexpr unsigned long kStepSettle = 30;
constexpr unsigned long kStepHold = 45;
constexpr unsigned long kStepAfter = 20;
constexpr float kStepMinimum = 40.0f;
constexpr float kOppositeCosine = -0.5f;
constexpr unsigned long kPauseHold = 18;  // the game opens the menu at a 12-frame hold
constexpr unsigned long kPauseHoldSample = 10;  // before the game opens the menu at 12
constexpr unsigned long kPauseMenuSettle = 90;
constexpr unsigned long kPauseMenuLimit = 300;
constexpr unsigned long kPausePermitLimit = 600;
constexpr float kPausedDrift = 1.0f;
constexpr unsigned long kResumeSettle = 60;

// story route
constexpr unsigned long kCalibrateHold = 20;
constexpr float kCalibrateMinimum = 10.0f;
// Arrival radii per segment. Segment 0's town road is open (proven in real
// runs with 600). Segment 1's points are about 500 apart on a path with 100
// units of clearance: turning for the next point from up to 100 away keeps
// Mario within that clearance (a wider radius cut corners into a curb and a
// building in story-3 and story-6). The last waypoint lies inside the movie's
// trigger area: walk right up to it, or Mario can stop outside the area (the
// castle waypoint is 92 units inside its box).
constexpr float kWaypointRadius[2] = {600.0f, 100.0f};
constexpr float kFinalWaypointRadius[2] = {200.0f, 80.0f};
constexpr float kProgressStep = 50.0f;
constexpr unsigned long kStuckFrames = 180;
constexpr int kMaxRecoveries = 4;
constexpr unsigned long kSidestepFrames = 40;
constexpr unsigned long kSegmentLimit = 9000;
constexpr unsigned long kMovieStartLimit = 900;
// Movie.<X>.Start to .End (for PrologueB, or Stage.HeavensDoorGalaxy): the
// THP's frames (PrologueA.thp 5591, PrologueB.thp 7076, at 59.94 fps) plus
// MoviePlayingSequence's waits (PlayWait 75 for both, EndWait 60 for A and 0
// for B), the 0.1% frame-rate drift and about 47 frames of player start and
// teardown: 5779 measured for A in story-3, about 7205 expected for B. The
// margin covers those 188 frames at most and a loading hitch.
constexpr unsigned long kMovieFrames[2] = {5591, 7076};
constexpr unsigned long kMovieMargin = 400;
constexpr unsigned long kStoryReadyLimit = 7200;
constexpr unsigned long kStageLimit = 3600;
constexpr unsigned long kStageReadyFrames = 60;  // HeavensDoorGalaxy ready in a row before PASS
constexpr unsigned long kRouteLogInterval = 120;
constexpr float kMinimumUpY = 0.5f;     // gravity within 60 degrees of the stage's down
constexpr float kMinimumAxis = 1e-3f;   // shorter axes are degenerate
constexpr float kRestartX = -500.0f;  // GeneralPos "リスタート", where PrologueA leaves Mario
constexpr float kRestartZ = 6250.0f;
constexpr float kRestartTolerance = 2000.0f;

// Routes on PeachCastleGardenGalaxy scenario 1, from the stage placement
// (game worker's route reports): the town road from the start to the plaza
// trigger (SwitchCube l_id 5, PrologueA), then to the castle trigger
// (SwitchCube l_id 4 at (-7000, -8908), PrologueB).
const std::vector<PetariNative::App::Smoke::Waypoint> kRoutes[2] = {
    {{12650.0f, 10050.0f}, {11200.0f, 11450.0f}, {5650.0f, 8750.0f}, {4550.0f, 7550.0f}, {3150.0f, 8000.0f},
     {2400.0f, 6450.0f}, {-300.0f, 5250.0f}, {-650.0f, 4350.0f}},
    // After PrologueA and the attack (switch 1), from the restart point
    // (-500, 260, 6250): a walk found by the game worker on the stage's own
    // collision (PeachCastleGardenPlanet, PeachCastleTownAfterAttack and the 14
    // CrystalCageS placements): steps up at most 50 per 100 units, and floor
    // within 120 of the path's height everywhere within 100 units of it. It
    // stays north of a 70-80 unit curb (x -4836..-3450, z 2270..2380, where
    // Mario stalled in story-3 and story-6) and comes down the x -5800/-5900
    // corridor west of a building (x -5705..-5105, z 810..1060); the last
    // point is inside the castle trigger box. Static geometry only: meteors,
    // ships and Toads may still block or knock Mario about.
    {{-1000.0f, 6300.0f}, {-1500.0f, 5900.0f}, {-2000.0f, 5600.0f}, {-2500.0f, 5100.0f}, {-3000.0f, 5000.0f},
     {-3500.0f, 4800.0f}, {-4000.0f, 4600.0f}, {-4500.0f, 4600.0f}, {-5000.0f, 4200.0f}, {-5500.0f, 3700.0f},
     {-5800.0f, 3200.0f}, {-5800.0f, 2700.0f}, {-5800.0f, 2200.0f}, {-5800.0f, 1700.0f}, {-5800.0f, 1200.0f},
     {-5900.0f, 700.0f}, {-5900.0f, 200.0f}, {-5900.0f, -300.0f}, {-5900.0f, -800.0f}, {-5900.0f, -1300.0f},
     {-5900.0f, -1800.0f}, {-6100.0f, -2300.0f}, {-6400.0f, -2800.0f}, {-6400.0f, -3300.0f}, {-6400.0f, -3800.0f},
     {-6400.0f, -4300.0f}, {-6400.0f, -4800.0f}, {-6400.0f, -5300.0f}, {-6800.0f, -5800.0f}, {-7300.0f, -6300.0f},
     {-7500.0f, -6500.0f}},
};
const char* const kMovieStart[2] = {"Movie.PrologueA.Start", "Movie.PrologueB.Start"};
const char* const kMovieEnd[2] = {"Movie.PrologueA.End", "Movie.PrologueB.End"};

struct Vec {
    float x, y, z;
};
Vec sub(Vec a, Vec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float dot(Vec a, Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
float length(Vec a) {
    return std::sqrt(dot(a, a));
}
std::string text(Vec a) {
    return "(" + std::to_string(a.x) + ", " + std::to_string(a.y) + ", " + std::to_string(a.z) + ")";
}
Vec position(const PetariNative::App::Smoke::Observation& o) {
    return {o.playerX, o.playerY, o.playerZ};
}
// The "up" direction: against gravity.
Vec up(const PetariNative::App::Smoke::Observation& o) {
    const Vec g{o.gravityX, o.gravityY, o.gravityZ};
    const float n = length(g);
    return n > 1e-4f ? Vec{-g.x / n, -g.y / n, -g.z / n} : Vec{0.0f, 1.0f, 0.0f};
}
// Displacement across the ground: the gravity component removed.
Vec across(Vec d, Vec u) {
    const float h = dot(d, u);
    return {d.x - h * u.x, d.y - h * u.y, d.z - h * u.z};
}

Vec normalized(Vec a) {
    const float n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return n > 1e-6f ? Vec{a.x / n, a.y / n, a.z / n} : Vec{0.0f, 0.0f, 0.0f};
}

bool isAllowedPrompt(const std::string& messageId, int type) {
    // (The reload script answers no prompt; Driver::step checks the script.)
    return type == 2 && (messageId == "System_FileSelect001" || messageId == "System_FileSelect013");
}

}  // namespace

int exitStatus(Result result) {
    switch (result) {
    case Result::Pass:
        return 0;
    case Result::Fail:
        return 1;
    case Result::Blocked:
        return 2;
    case Result::Assisted:
        return 3;
    case Result::Running:
        break;
    }
    return 0;
}

const char* resultName(Result result) {
    switch (result) {
    case Result::Running:
        return "RUNNING";
    case Result::Pass:
        return "PASS";
    case Result::Fail:
        return "FAIL";
    case Result::Blocked:
        return "BLOCKED";
    case Result::Assisted:
        return "ASSISTED";
    }
    return "?";
}

const std::vector<Waypoint>& storyRoute(int segment) {
    return kRoutes[segment == 0 ? 0 : 1];
}

StickKeys stickKeysFor(float x, float y) {
    StickKeys keys;
    const float n = std::sqrt(x * x + y * y);
    if (n < 1e-6f) {
        return keys;
    }
    // Within 22.5 degrees of an axis: that key alone; between: both.
    constexpr float kSin22_5 = 0.38268343f;
    keys.up = y / n > kSin22_5;
    keys.down = y / n < -kSin22_5;
    keys.right = x / n > kSin22_5;
    keys.left = x / n < -kSin22_5;
    return keys;
}

Driver::Driver(unsigned long frameLimit, Script script) : mFrameLimit(frameLimit), mScript(script) {
    const char* player = std::getenv("PETARI_SMOKE_PLAYER");
    mChooseLuigi = player != nullptr && std::strcmp(player, "luigi") == 0;
}

bool Driver::seen(const std::string& milestone) const {
    for (const std::string& name : mSeen) {
        if (name == milestone) {
            return true;
        }
    }
    return false;
}

unsigned long Driver::seenCount(const std::string& milestone) const {
    unsigned long count = 0;
    for (const std::string& name : mSeen) {
        count += name == milestone;
    }
    return count;
}

bool Driver::aimAndPress(const Observation& observation, const char* id, Slot slot, Step& step) {
    const bool emptySlot = slot == Slot::Empty;
    const Observation::Target* target = nullptr;
    for (const Observation::Target& candidate : observation.targets) {
        if (candidate.id != id || !(candidate.flags & kTargetSelectable)) {
            continue;
        }
        if (emptySlot && !(candidate.flags & kTargetEmpty)) {
            continue;
        }
        if (slot == Slot::NonEmpty && (candidate.flags & kTargetEmpty)) {
            continue;
        }
        if (target == nullptr || candidate.index < target->index) {
            target = &candidate;
        }
    }
    if (target == nullptr) {
        mAimFrames = 0;
        mPointingFrames = 0;
        if (++mAimMissing >= kTargetMissingLimit) {
            finish(Result::Fail, std::string("no selectable ") + id +
                                     (emptySlot ? " (empty)" : slot == Slot::NonEmpty ? " (non-empty)" : "") + " shown for " +
                                     std::to_string(kTargetMissingLimit) + " frames",
                   step);
        }
        return false;
    }
    mAimMissing = 0;
    step.pointer = true;
    step.pointerU = target->u;
    step.pointerV = target->v;
    step.assertFocus = true;
    if (mAimFrames++ == 0) {
        note("point at " + std::string(id) + " " + std::to_string(target->index) + " (" + std::to_string(target->u) +
             ", " + std::to_string(target->v) + ")");
    }
    mPointingFrames = (target->flags & kTargetPointing) ? mPointingFrames + 1 : 0;
    if (mPointingFrames >= kPointingFramesToPress) {
        note("tap A: " + std::string(id) + " " + std::to_string(target->index));
        tap(Button::A, kTapFrames, step);
        mAimFrames = 0;
        mPointingFrames = 0;
        return true;
    }
    if (mAimFrames >= kAimLimit) {
        finish(Result::Fail, "the pointer never got over " + std::string(id) + " at (" + std::to_string(target->u) +
                                 ", " + std::to_string(target->v) + ")",
               step);
    }
    return false;
}

void Driver::playable(const Observation& observation, Step& step) {
    auto waitFor = [&](const char* milestone, unsigned long limit, Phase next) {
        if (seen(milestone)) {
            mPhase = next;
            mPhaseFrames = 0;
            mAimMissing = 0;
        } else if (mPhaseFrames >= limit) {
            finish(Result::Fail, std::string("no ") + milestone + " within " + std::to_string(limit) + " frames", step);
        }
    };
    switch (mPhase) {
    case Phase::ChooseSlot:
        if (aimAndPress(observation, "FileSelect.Slot", loadsSave() ? Slot::NonEmpty : Slot::Empty, step)) {
            // An existing file goes to its confirmation; a new one is created first.
            mPhase = loadsSave() ? Phase::WaitFileConfirm : Phase::WaitMiiSelect;
            mPhaseFrames = 0;
        }
        break;
    case Phase::WaitMiiSelect:
        waitFor("FileSelector.MiiSelect", kMilestoneLimit, Phase::ChooseMario);
        break;
    case Phase::ChooseMario:
        if (aimAndPress(observation, "MiiSelect.Mario", Slot::Any, step)) {
            mPhase = Phase::WaitFileConfirm;
            mPhaseFrames = 0;
        }
        break;
    case Phase::WaitFileConfirm:
        waitFor("FileSelector.FileConfirm", kMilestoneLimit, mChooseLuigi ? Phase::ChooseLuigi : Phase::ChooseStart);
        break;
    case Phase::ChooseLuigi: {
        // FileSelect.Bros: index 0 while Mario is selected, 1 for Luigi. Press it
        // until it reports Luigi (at most 3 presses, 60 frames apart: the button
        // hides while its decide animation plays).
        const Observation::Target* bros = nullptr;
        for (const Observation::Target& candidate : observation.targets) {
            if (candidate.id == "FileSelect.Bros") {
                bros = &candidate;
            }
        }
        if (bros != nullptr && bros->index == 1) {
            note("Luigi selected with FileSelect.Bros after " + std::to_string(mLuigiPresses) + " press(es)");
            mPhase = Phase::ChooseStart;
            mPhaseFrames = 0;
            mAimFrames = mPointingFrames = mAimMissing = 0;
            break;
        }
        if (mLuigiPresses >= 3) {
            finish(Result::Fail, "FileSelect.Bros did not switch to Luigi after 3 presses", step);
            break;
        }
        if (mLuigiPresses > 0 && mPhaseFrames - mLuigiPressFrame < 60) {
            break;  // the switch animates; wait before judging or pressing again
        }
        if (aimAndPress(observation, "FileSelect.Bros", Slot::Any, step)) {  // FAILs if it never appears (Luigi locked)
            mLuigiPresses++;
            mLuigiPressFrame = mPhaseFrames;
        }
        break;
    }
    case Phase::ChooseStart:
        if (aimAndPress(observation, "FileSelect.Start", Slot::Any, step)) {
            mPhase = Phase::WaitDemo;
            mPhaseFrames = 0;
        }
        break;
    case Phase::WaitDemo:
        waitFor("FileSelector.DemoStartWait", kDemoLimit, Phase::Prologue);
        mSinceProgress = 0;
        break;
    case Phase::Prologue:
        if (mScript == Script::Observe) {
            note("observe: file started; watching until the frame limit");
            mPhase = Phase::Ready;
            mPhaseFrames = 0;
            break;
        }
        if (mScript == Script::Galaxy && observation.scene == "Game" && observation.sceneReady &&
            observation.stage != "FileSelect") {
            mPhase = Phase::Ready;
            mPhaseFrames = 0;
            mReadyFrames = 0;
            break;
        }
        if (seen("Prologue.GameStart")) {
            mPhase = mScript == Script::Playable ? Phase::Move : Phase::Ready;
            mMoveFrame = 0;
            mPhaseFrames = 0;
            mReadyFrames = 0;
            break;
        }
        // A save past the prologue starts in the game without it.
        if (mScript != Script::Playable && !seen("Prologue.PictureBook") && gameplayReady(observation)) {
            note("in the game without a prologue");
            mPhase = Phase::Ready;
            mPhaseFrames = 0;
            break;
        }
        if (mPrologueTapAt >= 0 && mFrame >= static_cast<unsigned long>(mPrologueTapAt)) {
            note("tap A: prologue text");
            tap(Button::A, kTapFrames, step);
            mPrologueTapAt = -1;
        }
        // A start without the prologue (a Luigi file goes straight to the Gateway,
        // StorySequenceExecutor: !isDataMario) opens with a demo whose dialogue a
        // player advances with A; press it once a second while that demo runs.
        if (!seen("Prologue.PictureBook") && observation.scene == "Game" && observation.sceneReady && observation.demoActive &&
            mSinceProgress >= 300 && mSinceProgress % 60 == 0) {
            if (mDemoTaps++ == 0) note("tap A: advancing the opening demo (no prologue on this file)");
            tap(Button::A, kTapFrames, step);
        }
        if (mSinceProgress % 600 == 599) {
            // Why the game is not yet playable, for stalled starts.
            note("waiting in " + observation.scene + "/" + observation.stage + ": sceneReady " +
                 std::to_string(observation.sceneReady) + ", player " + std::to_string(observation.playerValid) + ", demo " +
                 std::to_string(observation.demoActive) + ", pause permitted " + std::to_string(observation.pausePermitted) +
                 ", talk " + std::to_string(observation.talkActive) + ", off control " + std::to_string(observation.playerOffControl) +
                 ", dead " + std::to_string(observation.playerDead));
        }
        if (++mSinceProgress >= kPrologueStallLimit) {
            finish(Result::Fail,
                   "no prologue milestone for " + std::to_string(kPrologueStallLimit) + " frames (last: " +
                       (mSeen.empty() ? std::string("none") : mSeen.back()) + ")",
                   step);
        }
        break;
    case Phase::Move:
        ++mMoveFrame;
        if (mMoveFrame == kMoveSettle) {
            if (!observation.playerValid) {
                finish(Result::Fail, "no player position after Prologue.GameStart", step);
                break;
            }
            mStartX = observation.playerX;
            mStartY = observation.playerY;
            mStartZ = observation.playerZ;
            note("hold stick up for " + std::to_string(kMoveHold) + " frames from (" + std::to_string(mStartX) + ", " +
                 std::to_string(mStartY) + ", " + std::to_string(mStartZ) + ")");
            tap(Button::StickUp, kMoveHold, step);
        } else if (mMoveFrame == kMoveSettle + kMoveHold + kMoveAfter) {
            if (!observation.playerValid) {
                finish(Result::Fail, "no player position after moving", step);
                break;
            }
            const float dx = observation.playerX - mStartX;
            const float dy = observation.playerY - mStartY;
            const float dz = observation.playerZ - mStartZ;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance >= kMoveMinimum) {
                finish(Result::Pass, "Mario moved " + std::to_string(distance) + " units with the stick", step);
            } else {
                finish(Result::Fail, "Mario did not move: " + std::to_string(distance) + " units after holding the stick",
                       step);
            }
        }
        break;
    default:
        gameplay(observation, step);
        break;
    }
}

bool Driver::gameplayReady(const Observation& observation) {
    const bool ready = observation.scene == "Game" && observation.sceneReady && observation.playerValid &&
                       !observation.demoActive && observation.pausePermitted;
    mReadyFrames = ready ? mReadyFrames + 1 : 0;
    return mReadyFrames >= kReadyFrames;
}

bool Driver::galaxy(const Observation& observation, Step& step) {
    if (mGalaxyPhase == GalaxyPhase::Complete) return true;
    const Vec pos = position(observation);
    auto next = [&](GalaxyPhase phase, const char* label) {
        steer({}, step);
        mGalaxyPhase = phase;
        mGalaxyFrames = mReadyFrames = mAimFrames = mAimMissing = mPointingFrames = 0;
        note(std::string("galaxy route: ") + label);
    };
    if (++mGalaxyFrames > (mGalaxyPhase == GalaxyPhase::Observatory ? 7200u : 3600u)) {
        finish(Result::Fail, "galaxy route timed out in step " + std::to_string(static_cast<int>(mGalaxyPhase)) +
               " at " + text(pos), step);
        return false;
    }
    if (observation.playerValid && observation.playerDead) {
        finish(Result::Fail, "Mario died on galaxy route at " + text(pos), step);
        return false;
    }
    const bool missionLoading = mGalaxyPhase == GalaxyPhase::Scenario || mGalaxyPhase == GalaxyPhase::Mission;
    if (!observation.stage.empty() && observation.stage != "AstroGalaxy" && observation.stage != "AstroDome" &&
        !(missionLoading && observation.stage == "EggStarGalaxy")) {
        finish(Result::Fail, "unexpected galaxy route stage " + observation.stage, step);
        return false;
    }
    if (observation.stage == "AstroGalaxy" && mGalaxyPhase != GalaxyPhase::Observatory) {
        finish(Result::Fail, "returned to observatory before Good Egg mission", step);
        return false;
    }
    if (mGalaxyPhase == GalaxyPhase::Observatory && observation.stage == "AstroDome") {
        if (mSignForward == 0.0f) {
            finish(Result::Fail, "entered dome before observatory route calibration", step);
            return false;
        }
        next(GalaxyPhase::Dome, "Terrace entered");
    }
    const bool observatoryReady = observation.scene == "Game" && observation.sceneReady && observation.playerValid &&
                                  !observation.demoActive && observation.pausePermitted && !observation.talkActive;
    if (mGalaxyPhase == GalaxyPhase::Observatory && !observatoryReady && mSignForward == 0.0f) {
        if (mReadyFrames >= 60) note("observatory calibration interrupted; waiting for gameplay again");
        mReadyFrames = 0;
    }
    for (const auto& target : observation.targets) {
        if (target.id == "Talk.Advance" && (target.flags & kTargetSelectable)) {
            steer({}, step);
            mReadyFrames = 0;
            if (mFrame >= mGalaxyTapAfter) {
                note("tap A: visible dialogue page");
                tap(Button::A, kTapFrames, step);
                mGalaxyTapAfter = mFrame + 45;
            }
            return false;
        }
    }
    if (observation.talkActive) {
        mReadyFrames = 0;
        steer({}, step);
        return false;
    }
    switch (mGalaxyPhase) {
    case GalaxyPhase::Observatory: {
        // Scene initialization can finish while the opening camera still stops
        // player movement. Pause permission requires GameSceneAction and no wipe.
        if (!observatoryReady) {
            steer({}, step);
            if (mGalaxyFrames % 120 == 0) {
                note("observatory waiting for control at " + text(pos) + ", player " +
                     std::to_string(observation.playerValid) + ", demo " + std::to_string(observation.demoActive) +
                     ", pause " + std::to_string(observation.pausePermitted));
            }
            return false;
        }
        const Vec u = up(observation);
        const Vec right = across(Vec{observation.camXx, observation.camXy, observation.camXz}, u);
        const Vec forward = across(Vec{observation.camZx, observation.camZy, observation.camZz}, u);
        if (!std::isfinite(length(pos)) || !std::isfinite(length(right)) || !std::isfinite(length(forward)) ||
            length(Vec{observation.gravityX, observation.gravityY, observation.gravityZ}) < kMinimumAxis ||
            u.y < kMinimumUpY || length(right) < kMinimumAxis || length(forward) < kMinimumAxis) {
            finish(Result::Fail, "invalid position, gravity or camera on observatory route", step);
            return false;
        }
        if (mSignForward == 0.0f) {
            ++mReadyFrames;
            if (mReadyFrames == 60 || mReadyFrames == 80 || mReadyFrames == 100) {
                note("observatory calibration " + std::string(mReadyFrames == 60 ? "start forward" :
                     mReadyFrames == 80 ? "end forward/start right" : "end right") + " at " + text(pos) +
                     ", grounded " + std::to_string(observation.playerOnGround) +
                     ", demo " + std::to_string(observation.demoActive) +
                     ", pause " + std::to_string(observation.pausePermitted) +
                     ", pad operating " + std::to_string(observation.padOperating));
            }
            if (mReadyFrames == 60 || mReadyFrames == 80) {
                if (mReadyFrames == 80) {
                    const Vec delta = sub(pos, Vec{mStartX, mStartY, mStartZ});
                    if (length(delta) < kCalibrateMinimum) {
                        finish(Result::Fail, "observatory forward calibration did not move", step);
                        return false;
                    }
                    mForwardX = dot(delta, forward) >= 0 ? 1.0f : -1.0f;
                }
                mStartX = pos.x; mStartY = pos.y; mStartZ = pos.z;
                steer(mReadyFrames == 60 ? StickKeys{true, false, false, false} : StickKeys{false, false, false, true}, step);
            } else if (mReadyFrames == 100) {
                const Vec delta = sub(pos, Vec{mStartX, mStartY, mStartZ});
                if (length(delta) < kCalibrateMinimum) {
                    finish(Result::Fail, "observatory right calibration did not move", step);
                    return false;
                }
                mSignForward = mForwardX;
                mSignRight = dot(delta, right) >= 0 ? 1.0f : -1.0f;
                steer({}, step);
                mBestDistance = 1e30f;
            }
            return false;
        }
        // Approach coordinates follow the observatory ground to the Terrace trigger.
        static const Waypoint route[] = {{2400, -3700}, {2400, -2000}, {2900, -2000}, {2900, -1600},
                                        {3000, -1500}, {3200, -1500}, {3200, -300}, {3200, 1000},
                                        {2900, 1150}, {2634, 1322}};
        constexpr unsigned lastWaypoint = sizeof(route) / sizeof(route[0]) - 1;
        const auto& target = route[mWaypoint];
        const Vec delta = across(Vec{target.x - pos.x, 0, target.z - pos.z}, u);
        const float distance = length(delta);
        if (mGalaxyFrames % 120 == 0) {
            note("observatory waypoint " + std::to_string(mWaypoint) + ": " + std::to_string(distance) +
                 " units away at " + text(pos));
        }
        if (distance < (mWaypoint == lastWaypoint ? 45.0f : 60.0f)) {
            steer({}, step);
            if (mWaypoint < lastWaypoint) {
                ++mWaypoint; mBestDistance = 1e30f; mStuckFrames = mRecoveries = 0;
                note("observatory waypoint " + std::to_string(mWaypoint) + " at " + text(pos));
            }
            return false;
        }
        if (distance < mBestDistance - 30) {
            mBestDistance = distance; mStuckFrames = 0;
        } else if (++mStuckFrames >= 180) {
            mStuckFrames = 0;
            if (++mRecoveries > 3) {
                finish(Result::Fail, "observatory route stuck at " + text(pos) + " waypoint " + std::to_string(mWaypoint), step);
                return false;
            }
            tap(Button::A, kTapFrames, step);
        }
        steer(stickKeysFor(dot(delta, normalized(right)) * mSignRight,
                           dot(delta, normalized(forward)) * mSignForward), step);
        break;
    }
    case GalaxyPhase::Dome:
        if (observation.sceneReady && observation.scenario != 1) {
            finish(Result::Fail, "entered a dome other than Terrace", step);
        } else if (gameplayReady(observation)) {
            next(GalaxyPhase::BlueStar, "aiming at Blue Star");
        }
        break;
    case GalaxyPhase::BlueStar:
        if (aimAndPress(observation, "Dome.BlueStar", Slot::Any, step))
            next(GalaxyPhase::Galaxy, "selecting Good Egg Galaxy");
        break;
    case GalaxyPhase::Galaxy:
        if (std::any_of(observation.targets.begin(), observation.targets.end(), [](const Observation::Target& target) {
                return target.id == "Galaxy.UnlockEggStarGalaxy" && (target.flags & kTargetSelectable);
            })) {
            if (aimAndPress(observation, "Galaxy.UnlockEggStarGalaxy", Slot::Any, step))
                next(GalaxyPhase::Reveal, "waiting for Good Egg reveal");
            break;
        }
        if (aimAndPress(observation, "Galaxy.EggStarGalaxy", Slot::Any, step))
            next(GalaxyPhase::Confirm, "confirming Good Egg Galaxy");
        break;
    case GalaxyPhase::Reveal:
        // Open state alone is insufficient: the observer publishes this target
        // only once the reveal has returned to Wait and pointing is valid.
        if (std::any_of(observation.targets.begin(), observation.targets.end(), [](const Observation::Target& target) {
                return target.id == "Galaxy.EggStarGalaxy" && (target.flags & kTargetSelectable);
            })) {
            next(GalaxyPhase::Galaxy, "Good Egg revealed; selecting galaxy");
        } else if (mGalaxyFrames >= kTargetMissingLimit) {
            finish(Result::Fail, "Good Egg reveal did not show selectable Galaxy.EggStarGalaxy within " +
                   std::to_string(kTargetMissingLimit) + " frames", step);
        }
        break;
    case GalaxyPhase::Confirm:
        if (aimAndPress(observation, "Galaxy.Start", Slot::Any, step))
            next(GalaxyPhase::Scenario, "selecting first mission");
        break;
    case GalaxyPhase::Scenario:
        if (aimAndPress(observation, "Scenario.First", Slot::Any, step))
            next(GalaxyPhase::Mission, "waiting for Good Egg mission 1");
        break;
    case GalaxyPhase::Mission:
        if (observation.stage != "EggStarGalaxy" || observation.scene != "Game" || !observation.sceneReady)
            mReadyFrames = 0;
        if (observation.stage == "EggStarGalaxy" && observation.scene == "Game" && observation.sceneReady) {
            if (observation.scenario != 1) {
                finish(Result::Fail, "Good Egg loaded the wrong mission", step);
            } else if (gameplayReady(observation)) {
                next(GalaxyPhase::Complete, "Good Egg mission 1 ready; checking gameplay");
                mPhaseFrames = 0;
                return true;
            }
        }
        break;
    case GalaxyPhase::Complete: return true;
    }
    return false;
}

void Driver::gameplay(const Observation& observation, Step& step) {
    if (mScript == Script::Observe) {
        // Only watch: each scene or stage change is logged; the frame limit passes.
        const std::string where = observation.scene + "/" + observation.stage + " scenario " + std::to_string(observation.scenario);
        if (where != mObserved) {
            note("observe: now in " + where + (observation.playerValid ? "" : " (no player)"));
            mObserved = where;
        }
        // Dialogue (the epilogue's talks) waits for A, as a player reads it: once a second.
        if (observation.talkActive && ++mObserveTalkFrames % 60 == 0) {
            if (mObserveTaps++ % 20 == 0) note("observe: tap A to advance a talk");
            tap(Button::A, kTapFrames, step);
        }
        return;
    }
    if (mScript == Script::Galaxy && mGalaxyPhase == GalaxyPhase::Complete &&
        (observation.scene != "Game" || observation.stage != "EggStarGalaxy" || observation.scenario != 1)) {
        finish(Result::Fail, "left Good Egg mission 1 during gameplay checks", step);
        return;
    }
    if (mScript == Script::Galaxy && observation.playerValid && observation.playerDead) {
        finish(Result::Fail, "Mario died during Good Egg gameplay checks", step);
        return;
    }
    // Waiting for gameplay checks the player itself. The story checks him in
    // story(): needed while walking, not during the movies and stage load
    // (after a movie's start milestone is handled).
    const bool checkedElsewhere = mPhase == Phase::Ready || mPhase == Phase::Calibrate || mPhase == Phase::Route ||
                                  mPhase == Phase::WaitMovie || mPhase == Phase::MovieEnd || mPhase == Phase::WaitStage;
    if (!checkedElsewhere && !observation.playerValid) {
        finish(Result::Fail, std::string("no player position while ") + phase(), step);
        return;
    }
    const Vec pos = position(observation);
    const Vec start{mStartX, mStartY, mStartZ};
    auto setStart = [&] {
        mStartX = pos.x;
        mStartY = pos.y;
        mStartZ = pos.z;
    };
    auto next = [&](Phase phase) {
        mPhase = phase;
        mPhaseFrames = 0;
    };
    switch (mPhase) {
    case Phase::Ready:
        if (mScript == Script::Galaxy && !galaxy(observation, step)) {
            break;
        }
        if (gameplayReady(observation) || (mReadyFrames >= kReadyFrames)) {
            note("gameplay ready at " + text(pos) + (observation.playerOnGround ? ", on the ground" : ", in the air"));
            setStart();
            if (mScript == Script::Story) {
                startSegment(mSegment);
            } else {
                next(Phase::Idle);
            }
        } else if (mPhaseFrames >= (mScript == Script::Story ? kStoryReadyLimit : kReadyLimit)) {
            finish(Result::Fail, std::string("gameplay never became ready (player ") +
                                     (observation.playerValid ? "present" : "missing") + ", demo " +
                                     (observation.demoActive ? "active" : "idle") + ", pause " +
                                     (observation.pausePermitted ? "permitted" : "not permitted") + ")",
                   step);
        }
        break;
    case Phase::Idle:
        if (mPhaseFrames >= kIdleFrames) {
            const float drift = length(sub(pos, start));
            note("idle " + std::to_string(kIdleFrames) + " frames: " + text(start) + " -> " + text(pos) + ", drift " +
                 std::to_string(drift));
            if (drift > kIdleDrift) {
                finish(Result::Fail, "Mario was not still with no input: drifted " + std::to_string(drift) + " units",
                       step);
            } else if (!observation.playerOnGround) {
                finish(Result::Fail, "Mario was not on the ground after idling", step);
            } else {
                setStart();
                mLeftGround = false;
                mMaxRise = 0.0f;
                note("tap A: jump from " + text(pos));
                tap(Button::A, kTapFrames, step);
                next(Phase::Jump);
            }
        }
        break;
    case Phase::Jump: {
        const float rise = dot(sub(pos, start), up(observation));
        mMaxRise = std::max(mMaxRise, rise);
        if (!observation.playerOnGround) {
            mLeftGround = true;
        }
        if (!mLeftGround && mPhaseFrames > kJumpLeaveLimit) {
            finish(Result::Fail, "Mario did not leave the ground within " + std::to_string(kJumpLeaveLimit) +
                                     " frames of A",
                   step);
        } else if (mLeftGround && observation.playerOnGround) {
            note("landed at " + text(pos) + " after " + std::to_string(mPhaseFrames) + " frames, highest " +
                 std::to_string(mMaxRise) + " units");
            if (mMaxRise < kJumpMinimumRise) {
                finish(Result::Fail, "the jump rose only " + std::to_string(mMaxRise) + " units", step);
            } else {
                next(Phase::Forward);
            }
        } else if (mPhaseFrames > kJumpLandLimit) {
            finish(Result::Fail, "Mario did not land within " + std::to_string(kJumpLandLimit) + " frames (highest " +
                                     std::to_string(mMaxRise) + " units)",
                   step);
        }
        break;
    }
    case Phase::Forward:
    case Phase::Backward: {
        const bool forward = mPhase == Phase::Forward;
        if (mPhaseFrames == kStepSettle) {
            setStart();
            note(std::string("hold stick ") + (forward ? "up" : "down") + " " + std::to_string(kStepHold) +
                 " frames from " + text(pos));
            tap(forward ? Button::StickUp : Button::StickDown, kStepHold, step);
        } else if (mPhaseFrames == kStepSettle + kStepHold + kStepAfter) {
            const Vec moved = across(sub(pos, start), up(observation));
            const float distance = length(moved);
            note(std::string("stick ") + (forward ? "up" : "down") + ": " + text(start) + " -> " + text(pos) + ", " +
                 std::to_string(distance) + " units across the ground");
            if (distance < kStepMinimum) {
                finish(Result::Fail, std::string("stick ") + (forward ? "up" : "down") + " moved Mario only " +
                                         std::to_string(distance) + " units",
                       step);
            } else if (forward) {
                mForwardX = moved.x;
                mForwardY = moved.y;
                mForwardZ = moved.z;
                next(Phase::Backward);
            } else {
                const Vec first{mForwardX, mForwardY, mForwardZ};
                const float cosine = dot(first, moved) / (length(first) * distance);
                note("up/down directions: cosine " + std::to_string(cosine));
                if (cosine > kOppositeCosine) {
                    finish(Result::Fail, "stick up and down did not move Mario in opposite directions (cosine " +
                                             std::to_string(cosine) + ")",
                           step);
                } else {
                    mPausePressed = false;
                    next(Phase::PauseOpen);
                }
            }
        }
        break;
    }
    case Phase::PauseOpen:
        // Evidence for the pause button: what the game reads mid-hold.
        if (mPausePressed && mPhaseFrames == kPauseHoldSample && mPadDuringHold.empty()) {
            mPadDuringHold = std::string("A ") + (observation.padA ? "held" : "up") + ", B " +
                             (observation.padB ? "held" : "up") + ", Plus " + (observation.padPlus ? "held" : "up") +
                             ", Minus " + (observation.padMinus ? "held" : "up") + ", operating " +
                             (observation.padOperating ? "yes" : "no") + ", pause " +
                             (observation.pausePermitted ? "permitted" : "not permitted");
            note("game buttons during the pause hold: " + mPadDuringHold);
        }
        if (!mPausePressed) {
            if (mPhaseFrames >= kStepSettle && observation.pausePermitted) {
                note("hold Plus (default binding) " + std::to_string(kPauseHold) + " frames: pause at " + text(pos));
                mPauseOpenCount = seenCount("PauseMenu.Open");
                mPadDuringHold.clear();
                tap(Button::Plus, kPauseHold, step);
                mPausePressed = true;
                mPhaseFrames = 0;
            } else if (mPhaseFrames >= kPausePermitLimit) {
                finish(Result::Fail, "pausing was not permitted for " + std::to_string(kPausePermitLimit) + " frames",
                       step);
            }
        } else if (seenCount("PauseMenu.Open") > mPauseOpenCount) {
            next(Phase::Paused);
        } else if (mPhaseFrames >= kPauseMenuLimit) {
            finish(Result::Fail, "no PauseMenu.Open within " + std::to_string(kPauseMenuLimit) +
                                     " frames of holding the Plus binding; game buttons during the hold: " +
                                     (mPadDuringHold.empty() ? std::string("not sampled") : mPadDuringHold),
                   step);
        }
        break;
    case Phase::Paused:
        if (mPhaseFrames == kPauseMenuSettle) {
            setStart();
            note("hold stick up " + std::to_string(kStepHold) + " frames while paused, from " + text(pos));
            tap(Button::StickUp, kStepHold, step);
        } else if (mPhaseFrames == kPauseMenuSettle + kStepHold + kStepAfter) {
            const float drift = length(sub(pos, start));
            note("while paused: " + text(start) + " -> " + text(pos) + ", " + std::to_string(drift) + " units");
            if (drift > kPausedDrift) {
                finish(Result::Fail, "Mario moved " + std::to_string(drift) + " units while the game was paused", step);
            } else {
                note("tap Plus: resume");
                mPauseCloseCount = seenCount("PauseMenu.Close");
                tap(Button::Plus, kTapFrames, step);
                next(Phase::PauseClose);
            }
        }
        break;
    case Phase::PauseClose:
        if (seenCount("PauseMenu.Close") > mPauseCloseCount) {
            next(Phase::Resume);
        } else if (mPhaseFrames >= kPauseMenuLimit) {
            finish(Result::Fail, "no PauseMenu.Close within " + std::to_string(kPauseMenuLimit) + " frames of Plus",
                   step);
        }
        break;
    case Phase::Resume:
        if (mPhaseFrames == kResumeSettle) {
            setStart();
            note("hold stick up " + std::to_string(kStepHold) + " frames after resuming, from " + text(pos));
            tap(Button::StickUp, kStepHold, step);
        } else if (mPhaseFrames == kResumeSettle + kStepHold + kStepAfter) {
            const float distance = length(across(sub(pos, start), up(observation)));
            note("after resuming: " + text(start) + " -> " + text(pos) + ", " + std::to_string(distance) + " units");
            if (distance < kStepMinimum) {
                finish(Result::Fail, "Mario did not move after resuming (" + std::to_string(distance) + " units)", step);
            } else {
                finish(Result::Pass,
                       std::string(mScript == Script::Galaxy ? "observatory, Terrace, Good Egg mission 1: " : mScript == Script::Reload ? "reloaded save: " : "") +
                           "idle, jump, opposite moves, pause and resume all checked",
                       step);
            }
        }
        break;
    default:
        story(observation, step);
        break;
    }
}

void Driver::steer(const StickKeys& keys, Step& step) {
    const struct {
        bool want, held;
        Button button;
    } keysNow[] = {{keys.up, mHeld.up, Button::StickUp},
                   {keys.down, mHeld.down, Button::StickDown},
                   {keys.left, mHeld.left, Button::StickLeft},
                   {keys.right, mHeld.right, Button::StickRight}};
    for (const auto& key : keysNow) {
        if (key.want != key.held) {
            step.presses.push_back({key.button, key.want});
            if (key.want) {
                step.assertFocus = true;
            }
        }
    }
    mHeld = keys;
}

void Driver::startSegment(int segment) {
    mSegment = segment;
    mWaypoint = 0;
    mSegmentFrames = 0;
    mRecoveries = 0;
    mStuckFrames = 0;
    mBestDistance = 1e30f;
    mPhase = Phase::Calibrate;
    mPhaseFrames = 0;
    note("story segment " + std::to_string(segment) + ": calibrating the camera");
}

void Driver::story(const Observation& observation, Step& step) {
    const Vec pos = position(observation);
    const std::string where = observation.playerValid ? text(pos) : std::string("(no player position)");
    // Steering works in the plane perpendicular to the observed gravity field
    // at Mario (Mario::getAirGravityVec): the camera axes and the direction to
    // each waypoint are projected onto it, so the steering holds whatever the
    // gravity's direction (on PeachCastleGarden it is -y everywhere).
    const Vec gravity{observation.gravityX, observation.gravityY, observation.gravityZ};
    const Vec u = up(observation);
    const Vec camZ{observation.camZx, observation.camZy, observation.camZz};
    const Vec camX{observation.camXx, observation.camXy, observation.camXz};
    const Vec camForwardPlane = across(camZ, u);
    const Vec camRightPlane = across(camX, u);
    const Vec camForward = normalized(camForwardPlane);
    const Vec camRight = normalized(camRightPlane);
    auto next = [&](Phase phase) {
        mPhase = phase;
        mPhaseFrames = 0;
        mStageReadyFrames = 0;
    };
    const bool walking = mPhase == Phase::Calibrate || mPhase == Phase::Route;
    // A movie can start before the last waypoint (the trigger is an area) or
    // some frames after Mario stands in it; its milestone counts even if the
    // player is already gone from the observation.
    if ((walking || mPhase == Phase::WaitMovie) && seenCount(kMovieStart[mSegment]) > 0) {
        steer(StickKeys{}, step);
        note(std::string(kMovieStart[mSegment]) + " at " + where);
        next(Phase::MovieEnd);
        return;
    }
    if (walking && !observation.playerValid) {
        finish(Result::Fail, std::string("no player position while ") + this->phase(), step);
        return;
    }
    if (observation.playerValid && observation.playerDead) {
        finish(Result::Fail, "Mario died at " + text(pos) + " (segment " + std::to_string(mSegment) + ")", step);
        return;
    }
    if (walking && observation.talkActive) {
        finish(Result::Fail, "a talk opened at " + text(pos) + " (segment " + std::to_string(mSegment) + ")", step);
        return;
    }
    if (walking) {
        auto finite = [](Vec v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!finite(pos) || !finite(gravity) || !finite(camZ) || !finite(camX)) {
            finish(Result::Fail, "non-finite observation on the story route (position " + text(pos) + ", gravity " +
                                     text(gravity) + ")",
                   step);
            return;
        }
        if (length(gravity) < kMinimumAxis) {
            finish(Result::Fail, "no gravity direction at " + text(pos) + " (gravity " + text(gravity) + ")", step);
            return;
        }
        // The waypoints are on the stage's ground: gravity tilted beyond 60
        // degrees (a wall or a ceiling) would make them meaningless.
        if (u.y < kMinimumUpY) {
            finish(Result::Fail, "route limitation, not a game fault: gravity tilted more than 60 degrees (up " + text(u) +
                                     ") at " + text(pos) + ", where the ground-plane waypoints do not apply",
                   step);
            return;
        }
        if (length(camForwardPlane) < kMinimumAxis || length(camRightPlane) < kMinimumAxis) {
            finish(Result::Fail, "camera axis along gravity (camera Z " + text(camZ) + ", X " + text(camX) + ", up " +
                                     text(u) + ")",
                   step);
            return;
        }
    }
    if (walking && ++mSegmentFrames > kSegmentLimit) {
        finish(Result::Fail, "segment " + std::to_string(mSegment) + " took over " + std::to_string(kSegmentLimit) +
                                 " frames (waypoint " + std::to_string(mWaypoint) + ", at " + text(pos) + ")",
               step);
        return;
    }

    switch (mPhase) {
    case Phase::Calibrate:
        if (mPhaseFrames == 1 && mSegment == 1) {
            // MoviePlayingSequence puts Mario at the stage's restart point.
            const float offset = length(Vec{pos.x - kRestartX, 0.0f, pos.z - kRestartZ});
            note("after PrologueA Mario is at " + text(pos) + ", " + std::to_string(offset) +
                 " units from the restart point");
            if (offset > kRestartTolerance) {
                finish(Result::Fail, "after PrologueA Mario is " + std::to_string(offset) +
                                         " units from the restart point (-500, 6250)",
                       step);
                return;
            }
        }
        if (mPhaseFrames == 1) {
            mStartX = pos.x;
            mStartY = pos.y;
            mStartZ = pos.z;
            steer(StickKeys{true, false, false, false}, step);
        } else if (mPhaseFrames == 1 + kCalibrateHold) {
            const Vec moved = across(sub(pos, Vec{mStartX, mStartY, mStartZ}), u);
            mForwardX = moved.x;
            mForwardY = moved.y;
            mForwardZ = moved.z;
            mStartX = pos.x;
            mStartY = pos.y;
            mStartZ = pos.z;
            steer(StickKeys{false, false, false, true}, step);
        } else if (mPhaseFrames == 1 + 2 * kCalibrateHold) {
            steer(StickKeys{}, step);
            const Vec upMove{mForwardX, mForwardY, mForwardZ};
            const Vec rightMove = across(sub(pos, Vec{mStartX, mStartY, mStartZ}), u);
            if (length(upMove) < kCalibrateMinimum || length(rightMove) < kCalibrateMinimum) {
                finish(Result::Fail, "calibration: the stick moved Mario only " + std::to_string(length(upMove)) + " (up) and " +
                                         std::to_string(length(rightMove)) + " (right) units",
                       step);
                return;
            }
            mSignForward = dot(upMove, camForward) >= 0.0f ? 1.0f : -1.0f;
            mSignRight = dot(rightMove, camRight) >= 0.0f ? 1.0f : -1.0f;
            note("calibration: stick up moves along " + std::string(mSignForward > 0 ? "+" : "-") +
                 "camera Z, stick right along " + (mSignRight > 0 ? "+" : "-") + "camera X");
            if (kRoutes[mSegment].empty()) {
                finish(Result::Fail, "no route for story segment " + std::to_string(mSegment) + " yet", step);
                return;
            }
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            next(Phase::Route);
        }
        break;
    case Phase::Route: {
        const std::vector<Waypoint>& route = kRoutes[mSegment];
        const Waypoint target = route[mWaypoint];
        // The waypoint at Mario's height, then flattened onto the local plane.
        const Vec toTarget = across(Vec{target.x - pos.x, 0.0f, target.z - pos.z}, u);
        const float distance = length(toTarget);
        const bool last = mWaypoint + 1 >= route.size();
        if (distance < (last ? kFinalWaypointRadius : kWaypointRadius)[mSegment]) {
            note("segment " + std::to_string(mSegment) + " waypoint " + std::to_string(mWaypoint) + " reached at " +
                 text(pos));
            if (last) {
                steer(StickKeys{}, step);
                next(Phase::WaitMovie);
                break;
            }
            ++mWaypoint;
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            mRecoveries = 0;
            break;
        }
        if (mPhaseFrames % kRouteLogInterval == 0) {
            note("segment " + std::to_string(mSegment) + " waypoint " + std::to_string(mWaypoint) + ": " +
                 std::to_string(distance) + " units away, at " + text(pos));
        }
        // Progress, or recover.
        if (distance < mBestDistance - kProgressStep) {
            mBestDistance = distance;
            mStuckFrames = 0;
        } else if (++mStuckFrames >= kStuckFrames && mFrame >= mRecoverUntil) {
            if (++mRecoveries > kMaxRecoveries) {
                finish(Result::Fail, "stuck near segment " + std::to_string(mSegment) + " waypoint " +
                                         std::to_string(mWaypoint) + " at " + text(pos) + ", " +
                                         std::to_string(distance) + " units away",
                       step);
                return;
            }
            mStuckFrames = 0;
            mBestDistance = distance;
            if (mRecoveries % 2 == 1) {
                note("stuck: sidestep at " + text(pos));
                mRecoverUntil = mFrame + kSidestepFrames;
            } else {
                note("stuck: jump at " + text(pos));
                tap(Button::A, kTapFrames, step);
            }
        }
        const Vec dir = normalized(toTarget);
        float x = dot(dir, camRight) * mSignRight;
        float y = dot(dir, camForward) * mSignForward;
        if (mFrame < mRecoverUntil) {
            // Sidestep: at right angles to the waypoint direction, alternating sides.
            const float side = (mRecoveries / 2) % 2 == 0 ? 1.0f : -1.0f;
            const float sx = -y * side, sy = x * side;
            x = sx;
            y = sy;
        }
        steer(stickKeysFor(x, y), step);
        break;
    }
    case Phase::WaitMovie:
        if (mPhaseFrames >= kMovieStartLimit) {
            finish(Result::Fail, std::string("no ") + kMovieStart[mSegment] + " within " +
                                     std::to_string(kMovieStartLimit) + " frames of the last waypoint (at " + where + ")",
                   step);
        }
        break;
    case Phase::MovieEnd:
        // PrologueB leads to another stage: the stage change may come before
        // MovieStarter sees the movie end, so the new stage also ends it.
        if (mSegment == 1 && seenCount(kMovieEnd[1]) == 0 && seen("Stage.HeavensDoorGalaxy")) {
            note("Stage.HeavensDoorGalaxy after " + std::to_string(mPhaseFrames) +
                 " frames, without Movie.PrologueB.End (the stage changed first)");
            next(Phase::WaitStage);
        } else if (seenCount(kMovieEnd[mSegment]) > 0) {
            note(std::string(kMovieEnd[mSegment]) + " after " + std::to_string(mPhaseFrames) + " frames");
            if (mSegment == 0) {
                mSegment = 1;
                mReadyFrames = 0;
                next(Phase::Ready);
            } else {
                next(Phase::WaitStage);
            }
        } else if (mPhaseFrames >= kMovieFrames[mSegment] + kMovieMargin) {
            finish(Result::Fail, std::string("no ") + kMovieEnd[mSegment] + " within " +
                                     std::to_string(kMovieFrames[mSegment] + kMovieMargin) + " frames",
                   step);
        }
        break;
    case Phase::WaitStage: {
        // Ready for a run of frames, so the stage has updated and drawn, not
        // just finished initialising.
        const bool ready = seen("Stage.HeavensDoorGalaxy") && observation.scene == "Game" &&
                           observation.stage == "HeavensDoorGalaxy" && observation.sceneReady;
        if (!ready && mStageReadyFrames > 0) {
            note("HeavensDoorGalaxy not ready again after " + std::to_string(mStageReadyFrames) + " ready frames");
        }
        mStageReadyFrames = ready ? mStageReadyFrames + 1 : 0;
        if (mStageReadyFrames >= kStageReadyFrames) {
            finish(Result::Pass,
                   "story route reached HeavensDoorGalaxy (ready " + std::to_string(kStageReadyFrames) + " frames, " +
                       std::to_string(mPhaseFrames) + " frames after PrologueB)",
                   step);
        } else if (mPhaseFrames >= kStageLimit) {
            finish(Result::Fail, "HeavensDoorGalaxy not ready within " + std::to_string(kStageLimit) +
                                     " frames of PrologueB (scene " + observation.scene + ", stage " + observation.stage + ")",
                   step);
        }
        break;
    }
    default:
        break;
    }
}

const char* Driver::phase() const {
    switch (mPhase) {
    case Phase::Boot:
        return "boot";
    case Phase::Logo:
        return "logo";
    case Phase::WaitTitle:
        return "waiting for the title";
    case Phase::TitleReady:
        return "title shown";
    case Phase::Holding:
    case Phase::WaitTitleEnd:
        return "pressing A+B on the title";
    case Phase::WaitFileSelect:
        return "waiting for file select";
    case Phase::ChooseSlot:
        return "choosing an empty file";
    case Phase::WaitMiiSelect:
        return "creating the file";
    case Phase::ChooseMario:
        return "choosing the Mario icon";
    case Phase::WaitFileConfirm:
        return "confirming the icon";
    case Phase::ChooseLuigi:
        return "switching to Luigi";
    case Phase::ChooseStart:
        return "choosing Start";
    case Phase::WaitDemo:
        return "starting the file";
    case Phase::Prologue:
        return "prologue";
    case Phase::Move:
        return "moving Mario";
    case Phase::Ready:
        if (mScript == Script::Galaxy) {
            switch (mGalaxyPhase) {
            case GalaxyPhase::Observatory: return "walking to Terrace";
            case GalaxyPhase::Dome: return "waiting for Terrace";
            case GalaxyPhase::BlueStar: return "pointing at Blue Star";
            case GalaxyPhase::Galaxy: return "selecting Good Egg Galaxy";
            case GalaxyPhase::Reveal: return "waiting for Good Egg reveal";
            case GalaxyPhase::Confirm: return "confirming Good Egg Galaxy";
            case GalaxyPhase::Scenario: return "selecting Good Egg mission 1";
            case GalaxyPhase::Mission: return "loading Good Egg mission 1";
            case GalaxyPhase::Complete: break;
            }
        }
        return "waiting for gameplay";
    case Phase::Idle:
        return "idling";
    case Phase::Jump:
        return "jumping";
    case Phase::Forward:
        return "moving forward";
    case Phase::Backward:
        return "moving back";
    case Phase::PauseOpen:
        return "pausing";
    case Phase::Paused:
        return "paused";
    case Phase::PauseClose:
        return "resuming";
    case Phase::Resume:
        return "moving after resuming";
    case Phase::Calibrate:
        return "calibrating the camera";
    case Phase::Route:
        return "walking the story route";
    case Phase::WaitMovie:
        return "waiting for the story movie";
    case Phase::MovieEnd:
        return "watching the story movie";
    case Phase::WaitStage:
        return "waiting for HeavensDoorGalaxy";
    case Phase::Done:
        return "done";
    }
    return "?";
}

void Driver::note(const std::string& line) {
    mLog.push_back(line);
}

void Driver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.presses.push_back({button, true});
    step.assertFocus = true;
    mReleases.push_back({mFrame + holdFrames, button});
}

void Driver::pressTitle(Step& step) {
    ++mTitleAttempts;
    note("hold A+B: title (attempt " + std::to_string(mTitleAttempts) + ")");
    tap(Button::A, kTitleHoldFrames, step);
    tap(Button::B, kTitleHoldFrames, step);
    mPhase = Phase::WaitTitleEnd;
    mPhaseFrames = 0;
}

void Driver::finish(Result result, const std::string& reason, Step& step) {
    // Nothing stays held once the result is decided.
    for (const Release& release : mReleases) {
        step.presses.push_back({release.button, false});
    }
    mReleases.clear();
    steer(StickKeys{}, step);
    mResult = result;
    mReason = reason;
    if (mAssistInputs > 0) {
        const std::string assisted = std::to_string(mAssistInputs) + " physical gameplay input" +
                                     (mAssistInputs == 1 ? "" : "s") + ", first " + mFirstAssist;
        if (result == Result::Pass) {
            mResult = Result::Assisted;
            mReason = reason + "; ASSISTED, not unattended: " + assisted;
        } else {
            mReason = reason + " (also assisted: " + assisted + ")";
        }
    } else if (result == Result::Pass && (mPointerInputs > 0 || mFocusInputs > 0)) {
        mReason = reason + " (no physical gameplay input; the pointer moved " + std::to_string(mPointerInputs) +
                  " times and focus changed " + std::to_string(mFocusInputs) + " times, which this check does not "
                  "count as assistance)";
    }
    mPhase = Phase::Done;
    step.requestQuit = true;
    note(std::string(resultName(mResult)) + ": " + mReason);
}

void Driver::notePhysical(const Observation& observation) {
    const PhysicalInputs& now = observation.physical;
    if (!mPhysicalBaseSet) {
        // Input before the script's first frame is not the run's.
        mPhysicalSeen = now;
        mPhysicalBaseSet = true;
        return;
    }
    constexpr unsigned long kLoggedInputs = 20;
    if (now.gameplay > mPhysicalSeen.gameplay) {
        const unsigned long added = now.gameplay - mPhysicalSeen.gameplay;
        std::string where = observation.playerValid
                                ? " at (" + std::to_string(observation.playerX) + ", " +
                                      std::to_string(observation.playerY) + ", " + std::to_string(observation.playerZ) + ")"
                                : std::string();
        const std::string last(now.last, strnlen(now.last, sizeof(now.last)));
        // Only the latest event between two frames is known, so a batch is
        // reported by its frame and its latest input, not its first key edge.
        const std::string batch = std::to_string(added) + " new, latest " + last;
        if (mAssistInputs == 0) {
            mFirstAssist = "seen at frame " + std::to_string(mFrame) + " while " + phase() + where + " (" + batch + ")";
        }
        if (mAssistInputs < kLoggedInputs) {
            note("physical input: " + batch + ", while " + phase() + where);
        } else if (mAssistInputs < kLoggedInputs + added) {
            note("physical input: further inputs are counted, not logged");
        }
        mAssistInputs += added;
    }
    if (now.pointer > mPhysicalSeen.pointer) {
        if (mPointerInputs == 0) {
            note(std::string("physical pointer motion while ") + phase() + " (not counted as assistance)");
        }
        mPointerInputs += now.pointer - mPhysicalSeen.pointer;
    }
    if (now.focus > mPhysicalSeen.focus) {
        note(std::string("window focus changed while ") + phase());
        mFocusInputs += now.focus - mPhysicalSeen.focus;
    }
    mPhysicalSeen = now;
}

Step Driver::step(const Observation& observation) {
    Step step;
    mLog.clear();
    ++mFrame;
    ++mPhaseFrames;

    // Releases are always delivered, also after the result is decided.
    for (auto it = mReleases.begin(); it != mReleases.end();) {
        if (it->frame <= mFrame) {
            step.presses.push_back({it->button, false});
            it = mReleases.erase(it);
        } else {
            ++it;
        }
    }
    if (mResult != Result::Running) {
        return step;
    }
    notePhysical(observation);

    // What changed on screen.
    if (observation.scene != mLastScene || observation.stage != mLastStage) {
        note("scene " + (observation.scene.empty() ? std::string("(none)") : observation.scene) +
             (observation.stage.empty() ? "" : " stage " + observation.stage));
        mLastScene = observation.scene;
        mLastStage = observation.stage;
    }
    if (observation.strap != mLastStrap) {
        note(observation.strap ? "strap reminder shown" : "strap reminder gone");
        mLastStrap = observation.strap;
    }
    const int video = (observation.videoConfigured ? 2 : 0) + (observation.videoBlack ? 1 : 0);
    if (video != mLastVideo) {
        note(std::string("video ") + (observation.videoConfigured ? "configured" : "not configured") +
             (observation.videoBlack ? ", black" : ", showing"));
        mLastVideo = video;
    }
    if (observation.saveSequence != mLastSave) {
        note(observation.saveSequence ? "save-data sequence active" : "save-data sequence finished");
        mLastSave = observation.saveSequence;
    }

    bool fileSelect = false;
    for (const std::string& milestone : observation.milestones) {
        note("milestone " + milestone);
        mSeen.push_back(milestone);
        mSinceProgress = 0;
        // Once the file's demo started (possibly earlier in this same
        // observation, before the phase moves on), each ready page or letter
        // gets its tap; an earlier pending tap is kept.
        if (mScript != Script::Title && seen("FileSelector.DemoStartWait") && !seen("Prologue.GameStart") &&
            (milestone == "ProloguePictureBook.PageReady" || milestone == "PrologueLetter.Ready") &&
            mPrologueTapAt < 0) {
            mPrologueTapAt = static_cast<long>(mFrame + kPrologueTapDelay);
        }
        if (loadsSave() && milestone == "FileSelector.Create") {
            finish(Result::Fail, "the reload created a new file (FileSelector.Create)", step);
            return step;
        }
        if (milestone == "TitleSequence.BgmPrepare") {
            mBgmPrepareAt = static_cast<long>(mFrame);
        } else if (milestone == "TitleSequence.LogoDisplay") {
            mLogoDisplayAt = static_cast<long>(mFrame);
        } else if (milestone == "FileSelector.TitleEnd") {
            if (mPhase == Phase::WaitTitleEnd || mPhase == Phase::Holding) {
                mPhase = Phase::WaitFileSelect;
                mPhaseFrames = 0;
            }
        } else if (milestone == "FileSelector.RFLError") {
            mRflTapAt = static_cast<long>(mFrame + kRflTapDelay);
        } else if (milestone == "FileSelector.FileSelect") {
            // Files selectable: TitleEnd leads straight here (FileSelectStart
            // is only on the return and cancel paths).
            fileSelect = true;
        }
    }
    // Only on the way in from the title; returning to file select later (after
    // a cancelled prompt) does not restart the script.
    if (fileSelect && mScript != Script::Title && mPhase < Phase::ChooseSlot) {
        note(loadsSave() ? "file select reached; loading a saved file" : "file select reached; creating a file");
        mPhase = Phase::ChooseSlot;
        mPhaseFrames = 0;
        mAimMissing = 0;
    } else if (fileSelect && mScript == Script::Title) {
        finish(Result::Pass,
               "file select reached after " + std::to_string(mTitleAttempts) + " title press(es), frame " +
                   std::to_string(mFrame),
               step);
        return step;
    }

    // Prompts (playable): answer the allow-listed yes/no ones, stop at any other.
    if (mScript != Script::Title) {
        for (const Observation::Prompt& prompt : observation.prompts) {
            note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
            // Creating the file saves it (FileSelector::exeCreate), and so does
            // confirming the icon (exeMiiCreateWait -> storeSetMiiIdUserFile ->
            // startSaveAllUserFileSequence): SaveDataHandleSequence::trySave
            // shows the blocking "saving" window System_Save01 until the save
            // ends. It takes no input; only there it is expected and left alone.
            const bool creating = mPhase == Phase::WaitMiiSelect && seen("FileSelector.Create");
            const bool storingIcon = mPhase == Phase::WaitFileConfirm && mIconConfirmed;
            // The story route may pass a save point: the blocking saving window,
            // after the file started, is left alone there.
            if (mScript == Script::Story && prompt.messageId == "System_Save01" && prompt.type == 1 &&
                seen("FileSelector.DemoStartWait")) {
                note("saving window System_Save01 during the story: no input");
                continue;
            }
            if (mScript == Script::Observe && seen("FileSelector.DemoStartWait")) {
                finish(Result::Pass, "observed up to prompt " + prompt.messageId + " (type " + std::to_string(prompt.type) +
                                         ") in " + observation.scene + "/" + observation.stage,
                       step);
                return step;
            }
            if (mScript == Script::Reload && prompt.messageId == "System_Save01") {
                finish(Result::Fail, "the reload rewrote the save (System_Save01 appeared)", step);
                return step;
            }
            if (prompt.messageId == "System_Save01" && prompt.type == 1 && (creating || storingIcon)) {
                note("saving window System_Save01: no input");
                continue;
            }
            if (loadsSave() || !isAllowedPrompt(prompt.messageId, prompt.type)) {
                finish(Result::Blocked,
                       "prompt " + prompt.messageId + " (type " + std::to_string(prompt.type) + ") is not on the allow-list",
                       step);
                return step;
            }
            mPrompt = prompt.messageId;
            mAimFrames = 0;
            mAimMissing = 0;
            mPointingFrames = 0;
        }
    }

    // After the file's demo started, the game moves on to other scenes and stages.
    const bool anyScene = mScript != Script::Title && seen("FileSelector.DemoStartWait");

    // Anything the script does not expect ends the run with a reason.
    if (!anyScene && !observation.scene.empty() && observation.scene != "Logo" && observation.scene != "Game") {
        finish(Result::Fail, "unexpected scene " + observation.scene, step);
        return step;
    }
    if (!anyScene && observation.scene == "Game" && !observation.stage.empty() && observation.stage != "FileSelect") {
        finish(Result::Fail, "unexpected stage " + observation.stage, step);
        return step;
    }
    mSaveFrames = observation.saveSequence ? mSaveFrames + 1 : 0;
    if (mSaveFrames >= kSaveBlockedFrames) {
        finish(Result::Blocked,
               std::string("save-data sequence active for ") + std::to_string(kSaveBlockedFrames) +
                   " frames while " + phase() + "; a prompt probably waits (Yes/No needs the pointer)",
               step);
        return step;
    }
    if (mFrame >= mFrameLimit && mScript == Script::Observe && seen("FileSelector.DemoStartWait")) {
        finish(Result::Pass, "observed " + std::to_string(mFrameLimit) + " frames, last in " + mObserved, step);
        return step;
    }
    if (mFrame >= mFrameLimit) {
        finish(Result::Fail, "frame limit " + std::to_string(mFrameLimit) + " reached while " + phase(), step);
        return step;
    }

    if (mBgmPrepareAt >= 0 && mLogoDisplayAt < 0 && mFrame - mBgmPrepareAt >= kBgmPrepareLimit) {
        finish(Result::Fail,
               "stuck in the title's BgmPrepare for " + std::to_string(kBgmPrepareLimit) +
                   " frames (STM_TITLE not prepared)",
               step);
        return step;
    }

    if (mRflTapAt >= 0 && mFrame >= static_cast<unsigned long>(mRflTapAt)) {
        note("tap A: dismiss the Mii error window");
        tap(Button::A, kTapFrames, step);
        mRflTapAt = -1;
    }

    if (!mPrompt.empty()) {
        if (aimAndPress(observation, "Prompt.Yes", Slot::Any, step)) {
            note("answered " + mPrompt + ": yes");
            if (mPrompt == "System_FileSelect013") {
                mIconConfirmed = true;
            }
            mPrompt.clear();
        }
        return step;
    }

    switch (mPhase) {
    case Phase::Boot:
        if (observation.scene == "Logo") {
            mPhase = Phase::Logo;
            mPhaseFrames = 0;
        } else if (observation.scene == "Game") {
            mPhase = Phase::WaitTitle;
            mPhaseFrames = 0;
        }
        break;
    case Phase::Logo:
        if (observation.scene == "Game") {
            mPhase = Phase::WaitTitle;
            mPhaseFrames = 0;
            break;
        }
        if (observation.strap) {
            ++mStrapFrames;
            if (mStrapFrames >= kStrapFirstTap && (mStrapFrames - kStrapFirstTap) % kStrapTapInterval == 0) {
                note("tap A: strap reminder");
                tap(Button::A, kTapFrames, step);
            }
        } else {
            mStrapFrames = 0;
        }
        break;
    case Phase::WaitTitle:
        if (observation.scene == "Game" && observation.stage == "FileSelect" && observation.sceneReady &&
            mLogoDisplayAt >= 0) {
            mPhase = Phase::TitleReady;
            mPhaseFrames = 0;
        }
        break;
    case Phase::TitleReady:
        if (mFrame >= static_cast<unsigned long>(mLogoDisplayAt) + kLogoDisplaySettle) {
            pressTitle(step);
        }
        break;
    case Phase::Holding:
    case Phase::WaitTitleEnd:
        if (mPhaseFrames >= kTitleRetryFrames) {
            if (mTitleAttempts >= kTitleAttempts) {
                finish(Result::Fail, "the title did not accept A+B after " + std::to_string(mTitleAttempts) + " attempts",
                       step);
                return step;
            }
            pressTitle(step);
        }
        break;
    case Phase::WaitFileSelect:
    case Phase::Done:
        break;
    default:
        playable(observation, step);
        break;
    }
    return step;
}

// --- Process-wide state ---

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int> gExitStatus{0};
std::atomic<long long> gLastBeat{0};  // steady-clock ns
std::atomic<unsigned long> gFrame{0};
std::atomic<const char*> gPhase{"before the first frame"};
std::atomic<long long> gQuitAt{0};
std::atomic<bool> gQuitWithSave{false};

std::mutex gHangReportLock;
void (*gHangDump)(const char* reason) = nullptr;
std::string gHangSampleDirectory;

long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// Runs /usr/bin/sample on this process (all threads' stacks) into a file;
// waits for it, killing it if it takes far longer than asked.
void sampleProcess(const std::string& directory) {
    const char* enabled = std::getenv("PETARI_HANG_SAMPLE");
    if (enabled != nullptr && enabled[0] == '0') {
        return;
    }
    std::string dir = directory;
    if (dir.empty()) {
        const char* tmp = std::getenv("TMPDIR");
        dir = tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp";
    }
    if (dir.back() != '/') {
        dir += '/';
    }
    const std::string pid = std::to_string(::getpid());
    const std::string file = dir + "petari-hang-" + pid + ".sample.txt";
    const char* argv[] = {"/usr/bin/sample", pid.c_str(), "3", "-mayDie", "-file", file.c_str(), nullptr};
    pid_t child = 0;
    if (posix_spawn(&child, argv[0], nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
        std::fprintf(stderr, "PETARI SMOKE HANG: could not run /usr/bin/sample\n");
        return;
    }
    std::fprintf(stderr, "PETARI SMOKE HANG: sampling every thread for 3 s into %s\n", file.c_str());
    std::fflush(stderr);
    for (int waited = 0;; waited += 100) {
        int status = 0;
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child || done < 0) {
            break;
        }
        if (waited >= 30000) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            std::fprintf(stderr, "PETARI SMOKE HANG: sample did not finish in 30 s\n");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

[[noreturn]] void exitAfterHangReport(int status, const char* reason) {
    // The report reads a hung process; never let it keep the process alive.
    std::thread([status] {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        std::fprintf(stderr, "PETARI SMOKE HANG: report abandoned after 60 s\n");
        std::_Exit(status);
    }).detach();
    void (*dump)(const char*);
    std::string directory;
    {
        std::lock_guard<std::mutex> guard(gHangReportLock);
        dump = gHangDump;
        directory = gHangSampleDirectory;
    }
    sampleProcess(directory);
    if (dump != nullptr) {
        dump(reason);
    }
    std::fflush(stderr);
    std::_Exit(status);
}

}  // namespace

bool enabledFromEnvironment(Script* script) {
    const char* value = std::getenv("PETARI_SMOKE");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    if (std::strcmp(value, "title") == 0) {
        *script = Script::Title;
        return true;
    }
    if (std::strcmp(value, "playable") == 0) {
        *script = Script::Playable;
        return true;
    }
    if (std::strcmp(value, "gameplay") == 0) {
        *script = Script::Gameplay;
        return true;
    }
    if (std::strcmp(value, "reload") == 0) {
        *script = Script::Reload;
        return true;
    }
    if (std::strcmp(value, "galaxy") == 0) {
        *script = Script::Galaxy;
        return true;
    }
    if (std::strcmp(value, "story") == 0) {
        *script = Script::Story;
        return true;
    }
    if (std::strcmp(value, "observe") == 0) {
        *script = Script::Observe;
        return true;
    }
    std::fprintf(stderr, "PETARI SMOKE: unknown script \"%s\" (known: title, playable, gameplay, reload, story, observe); not running\n", value);
    return false;
}

int processExitStatus() {
    return gExitStatus.load();
}

void setProcessResult(Result result) {
    gExitStatus.store(exitStatus(result));
}

void startWatchdog(unsigned stallSeconds, unsigned shutdownSeconds) {
    gLastBeat.store(nowNs());
    std::thread([stallSeconds, shutdownSeconds] {
        const long long stall = static_cast<long long>(stallSeconds) * 1000000000LL;
        const long long shutdown = static_cast<long long>(shutdownSeconds) * 1000000000LL;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const long long now = nowNs();
            const long long quitAt = gQuitAt.load();
            if (quitAt != 0 && now - quitAt > shutdown) {
                std::fprintf(stderr,
                             "PETARI SMOKE TIMEOUT: the game did not exit %u s after the power button (frame %lu, "
                             "save-data sequence %s at the request)\n",
                             shutdownSeconds, gFrame.load(), gQuitWithSave.load() ? "active" : "idle");
                std::fflush(stderr);
                exitAfterHangReport(125, "smoke watchdog: no exit after the power button");
            }
            if (now - gLastBeat.load() > stall) {
                std::fprintf(stderr, "PETARI SMOKE TIMEOUT: no frame for %u s (frame %lu, %s)\n", stallSeconds,
                             gFrame.load(), gPhase.load());
                std::fflush(stderr);
                exitAfterHangReport(124, "smoke watchdog: no frame reached the seam");
            }
        }
    }).detach();
}

void setHangReport(void (*dump)(const char* reason), const std::string& sampleDirectory) {
    std::lock_guard<std::mutex> guard(gHangReportLock);
    gHangDump = dump;
    gHangSampleDirectory = sampleDirectory;
}

void heartbeat(unsigned long frame, const char* phase) {
    gFrame.store(frame);
    gPhase.store(phase);
    gLastBeat.store(nowNs());
}

void noteQuitRequested(bool saveSequenceActive) {
    gQuitWithSave.store(saveSequenceActive);
    gQuitAt.store(nowNs());
}

}  // namespace PetariNative::App::Smoke

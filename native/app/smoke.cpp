// The smoke script (smoke.hpp). No SDK or Aurora headers: the frame seam
// feeds it observations and applies its presses.

#include "smoke.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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
    }
    return "?";
}

Driver::Driver(unsigned long frameLimit, Script script) : mFrameLimit(frameLimit), mScript(script) {}

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
        if (aimAndPress(observation, "FileSelect.Slot", mScript == Script::Reload ? Slot::NonEmpty : Slot::Empty, step)) {
            // An existing file goes to its confirmation; a new one is created first.
            mPhase = mScript == Script::Reload ? Phase::WaitFileConfirm : Phase::WaitMiiSelect;
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
        waitFor("FileSelector.FileConfirm", kMilestoneLimit, Phase::ChooseStart);
        break;
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
    const bool ready = observation.playerValid && !observation.demoActive && observation.pausePermitted;
    mReadyFrames = ready ? mReadyFrames + 1 : 0;
    return mReadyFrames >= kReadyFrames;
}

void Driver::gameplay(const Observation& observation, Step& step) {
    if (mPhase != Phase::Ready && !observation.playerValid) {
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
        if (gameplayReady(observation) || (mReadyFrames >= kReadyFrames)) {
            note("gameplay ready at " + text(pos) + (observation.playerOnGround ? ", on the ground" : ", in the air"));
            setStart();
            next(Phase::Idle);
        } else if (mPhaseFrames >= kReadyLimit) {
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
                       std::string(mScript == Script::Reload ? "reloaded save: " : "") +
                           "idle, jump, opposite moves, pause and resume all checked",
                       step);
            }
        }
        break;
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
    case Phase::ChooseStart:
        return "choosing Start";
    case Phase::WaitDemo:
        return "starting the file";
    case Phase::Prologue:
        return "prologue";
    case Phase::Move:
        return "moving Mario";
    case Phase::Ready:
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
    mResult = result;
    mReason = reason;
    mPhase = Phase::Done;
    step.requestQuit = true;
    note(std::string(resultName(result)) + ": " + reason);
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
        if (mScript == Script::Reload && milestone == "FileSelector.Create") {
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
        note(mScript == Script::Reload ? "file select reached; loading a saved file" : "file select reached; creating a file");
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
            if (mScript == Script::Reload && prompt.messageId == "System_Save01") {
                finish(Result::Fail, "the reload rewrote the save (System_Save01 appeared)", step);
                return step;
            }
            if (prompt.messageId == "System_Save01" && prompt.type == 1 && (creating || storingIcon)) {
                note("saving window System_Save01: no input");
                continue;
            }
            if (mScript == Script::Reload || !isAllowedPrompt(prompt.messageId, prompt.type)) {
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

long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
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
    std::fprintf(stderr, "PETARI SMOKE: unknown script \"%s\" (known: title, playable, gameplay, reload); not running\n", value);
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
                std::_Exit(125);
            }
            if (now - gLastBeat.load() > stall) {
                std::fprintf(stderr, "PETARI SMOKE TIMEOUT: no frame for %u s (frame %lu, %s)\n", stallSeconds,
                             gFrame.load(), gPhase.load());
                std::fflush(stderr);
                std::_Exit(124);
            }
        }
    }).detach();
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

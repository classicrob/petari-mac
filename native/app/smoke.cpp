// The smoke script (smoke.hpp). No SDK or Aurora headers: the frame seam
// feeds it observations and applies its presses.

#include "smoke.hpp"

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

bool isAllowedPrompt(const std::string& messageId, int type) {
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

bool Driver::aimAndPress(const Observation& observation, const char* id, bool emptySlot, Step& step) {
    const Observation::Target* target = nullptr;
    for (const Observation::Target& candidate : observation.targets) {
        if (candidate.id != id || !(candidate.flags & kTargetSelectable)) {
            continue;
        }
        if (emptySlot && !(candidate.flags & kTargetEmpty)) {
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
            finish(Result::Fail, std::string("no selectable ") + id + (emptySlot ? " (empty)" : "") + " shown for " +
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
        if (aimAndPress(observation, "FileSelect.Slot", true, step)) {
            mPhase = Phase::WaitMiiSelect;
            mPhaseFrames = 0;
        }
        break;
    case Phase::WaitMiiSelect:
        waitFor("FileSelector.MiiSelect", kMilestoneLimit, Phase::ChooseMario);
        break;
    case Phase::ChooseMario:
        if (aimAndPress(observation, "MiiSelect.Mario", false, step)) {
            mPhase = Phase::WaitFileConfirm;
            mPhaseFrames = 0;
        }
        break;
    case Phase::WaitFileConfirm:
        waitFor("FileSelector.FileConfirm", kMilestoneLimit, Phase::ChooseStart);
        break;
    case Phase::ChooseStart:
        if (aimAndPress(observation, "FileSelect.Start", false, step)) {
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
            mPhase = Phase::Move;
            mMoveFrame = 0;
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
        if (mScript == Script::Playable && seen("FileSelector.DemoStartWait") && !seen("Prologue.GameStart") &&
            (milestone == "PictureBook.PageReady" || milestone == "PrologueLetter.Ready") && mPrologueTapAt < 0) {
            mPrologueTapAt = static_cast<long>(mFrame + kPrologueTapDelay);
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
        } else if (milestone == "FileSelector.FileSelectStart") {
            fileSelect = true;
        }
    }
    if (fileSelect && mScript == Script::Playable && mPhase != Phase::ChooseSlot) {
        note("file select reached; creating a file");
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
    if (mScript == Script::Playable) {
        for (const Observation::Prompt& prompt : observation.prompts) {
            note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
            if (!isAllowedPrompt(prompt.messageId, prompt.type)) {
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
    const bool anyScene = mScript == Script::Playable && seen("FileSelector.DemoStartWait");

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
        if (aimAndPress(observation, "Prompt.Yes", false, step)) {
            note("answered " + mPrompt + ": yes");
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
    std::fprintf(stderr, "PETARI SMOKE: unknown script \"%s\" (known: title, playable); not running\n", value);
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

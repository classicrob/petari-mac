// The smoke script (smoke.hpp). No SDK or Aurora headers: the frame seam
// feeds it observations and applies its presses.

#include "smoke.hpp"

#include <atomic>
#include <chrono>
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

Driver::Driver(unsigned long frameLimit) : mFrameLimit(frameLimit) {}

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
    if (fileSelect) {
        finish(Result::Pass,
               "file select reached after " + std::to_string(mTitleAttempts) + " title press(es), frame " +
                   std::to_string(mFrame),
               step);
        return step;
    }

    // Anything the script does not expect ends the run with a reason.
    if (!observation.scene.empty() && observation.scene != "Logo" && observation.scene != "Game") {
        finish(Result::Fail, "unexpected scene " + observation.scene, step);
        return step;
    }
    if (observation.scene == "Game" && !observation.stage.empty() && observation.stage != "FileSelect") {
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

bool enabledFromEnvironment() {
    const char* value = std::getenv("PETARI_SMOKE");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    if (std::strcmp(value, "title") == 0) {
        return true;
    }
    std::fprintf(stderr, "PETARI SMOKE: unknown script \"%s\" (known: title); not running\n", value);
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

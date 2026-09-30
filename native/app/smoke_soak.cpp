// Long-session soak script (smoke_soak.hpp).

#include "smoke_soak.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kTapFrames = 6;
constexpr unsigned long kPauseHold = 18;          // the game opens the menu at a 12-frame hold
constexpr unsigned long kPausePermitLimit = 1800; // frames to wait for pausing to be permitted
constexpr unsigned long kPauseMenuLimit = 300;    // frames for PauseMenu.Open after the hold
constexpr unsigned long kSettle = 30;
constexpr unsigned long kSceneChangeLimit = 5400; // frames from the menu choice to the new stage
constexpr unsigned long kReadyLimit = 5400;
constexpr unsigned long kReadyFrames = 60;
constexpr unsigned long kTargetMissingLimit = 600;
constexpr unsigned long kAimLimit = 600;
constexpr unsigned long kPointingFramesToPress = 3;
constexpr unsigned long kTalkTapInterval = 45;
constexpr unsigned long kWalkFrames = 45;
constexpr int kKeyPromptTaps = 10;                // A taps offered to a key window
constexpr unsigned long kKeyPromptFirstTap = 30;  // frames after it appears

double numberFromEnvironment(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const double number = std::strtod(value, &end);
    return end != nullptr && *end == '\0' && number > 0 ? number : fallback;
}

}  // namespace

bool soakEnabledFromEnvironment(SoakConfig* config) {
    const char* smoke = std::getenv("PETARI_SMOKE");
    if (smoke == nullptr || std::string(smoke) != "soak") {
        return false;
    }
    config->stages.clear();
    if (const char* list = std::getenv("PETARI_SOAK_STAGES"); list != nullptr && list[0] != '\0') {
        std::stringstream in(list);
        std::string item;
        while (std::getline(in, item, ',')) {
            const std::size_t colon = item.find(':');
            SoakConfig::Entry entry;
            entry.stage = item.substr(0, colon);
            entry.scenario = colon == std::string::npos ? 1 : std::atoi(item.c_str() + colon + 1);
            if (!entry.stage.empty() && entry.scenario > 0) {
                config->stages.push_back(entry);
            }
        }
    } else if (const char* stage = std::getenv("PETARI_STAGE"); stage != nullptr && stage[0] != '\0') {
        config->stages.push_back({stage, static_cast<int>(numberFromEnvironment("PETARI_SCENARIO", 1))});
    }
    if (config->stages.empty()) {
        std::fprintf(stderr, "PETARI SMOKE: soak needs PETARI_SOAK_STAGES or PETARI_STAGE; not running\n");
        return false;
    }
    config->minutes = numberFromEnvironment("PETARI_SOAK_MINUTES", 60);
    config->stageTailFrames = static_cast<unsigned long>(numberFromEnvironment("PETARI_STAGE_IDLE_FRAMES", 600));
    return true;
}

SoakDriver::SoakDriver(unsigned long cycleFrameLimit, const SoakConfig& config)
    : mConfig(config), mCycleFrameLimit(cycleFrameLimit), mStart(std::chrono::steady_clock::now()) {
    const SoakConfig::Entry& first = mConfig.stages.front();
    if (mConfig.setStage != nullptr) {
        mConfig.setStage(first.stage, first.scenario);  // before the first file load
    }
    StageConfig stage;
    stage.stage = first.stage;
    stage.scenario = first.scenario;
    stage.tailFrames = mConfig.stageTailFrames;
    mStage = std::make_unique<StageDriver>(mCycleFrameLimit, stage);
}

const char* SoakDriver::phase() const {
    switch (mPhase) {
    case Phase::Stage: return "soak: stage cycle";
    case Phase::ExitStage: return "soak: pause menu, back to the observatory";
    case Phase::Observatory: return "soak: in the observatory";
    case Phase::EndGame: return "soak: pause menu, end game (save, title)";
    case Phase::WaitTitle: return "soak: waiting for the title";
    case Phase::Done: return "soak: done";
    }
    return "soak";
}

void SoakDriver::next(Phase phase) {
    mPhase = phase;
    mPhaseFrames = 0;
    mMenu = MenuStep::OpenPause;
    mAnswering = false;
    mKeyPromptTaps = 0;
    mAimFrames = mPointingFrames = mAimMissing = 0;
    mReadyFrames = 0;
}

void SoakDriver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.assertFocus = true;
    step.presses.push_back({button, true});
    mReleases.push_back({mFrame + holdFrames, button});
}

void SoakDriver::finish(Result result, const std::string& reason, Step& step) {
    mResult = result;
    mReason = reason;
    mPhase = Phase::Done;
    step.requestQuit = true;
    const double minutes = std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count() / 60.0;
    note(std::string(resultName(result)) + ": " + reason + " (cycle " + std::to_string(mCycle) + ", " +
         std::to_string(static_cast<int>(minutes)) + " min)");
}

unsigned long SoakDriver::seenCount(const char* milestone) const {
    return static_cast<unsigned long>(std::count(mSeen.begin(), mSeen.end(), milestone));
}

bool SoakDriver::aimAndPress(const Observation& o, const char* id, Step& step) {
    const Observation::Target* target = nullptr;
    for (const Observation::Target& candidate : o.targets) {
        if (candidate.id == id && (candidate.flags & kTargetSelectable) && (target == nullptr || candidate.index < target->index)) {
            target = &candidate;
        }
    }
    if (target == nullptr) {
        mAimFrames = mPointingFrames = 0;
        if (++mAimMissing >= kTargetMissingLimit) {
            finish(Result::Fail, std::string("no selectable ") + id + " shown for " + std::to_string(kTargetMissingLimit) + " frames",
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
        note("point at " + std::string(id));
    }
    mPointingFrames = (target->flags & kTargetPointing) ? mPointingFrames + 1 : 0;
    if (mPointingFrames >= kPointingFramesToPress) {
        note("tap A: " + std::string(id));
        tap(Button::A, kTapFrames, step);
        mAimFrames = mPointingFrames = 0;
        return true;
    }
    if (mAimFrames >= kAimLimit) {
        finish(Result::Fail, "the pointer never got over " + std::string(id), step);
    }
    return false;
}

bool SoakDriver::handleTalk(const Observation& o, Step& step) {
    bool talkTarget = false;
    for (const Observation::Target& target : o.targets) {
        talkTarget = talkTarget || (target.id == "Talk.Advance" && (target.flags & kTargetSelectable));
    }
    if (!o.talkActive && !talkTarget) {
        return false;
    }
    if (mFrame >= mTalkTapAt) {
        note("talk open; tap A");
        tap(Button::A, kTapFrames, step);
        mTalkTapAt = mFrame + kTalkTapInterval;
    }
    return true;
}

// Pause, PauseMenu.Back, prompts answered, until the stage is `toStage`
// (a prefix: "Astro" is any part of the observatory; leaving a galaxy returns
// Mario to the dome he entered it from, AstroDome, or the hub, AstroGalaxy).
bool SoakDriver::menuExit(const Observation& o, const char* toStage, Step& step) {
    for (const Observation::Prompt& prompt : o.prompts) {
        note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
        if (prompt.type == 2) {
            mAnswering = true;
            mKeyPromptTaps = 0;
            mAimFrames = mPointingFrames = mAimMissing = 0;
        } else if (prompt.type == 0) {
            // A key window ("saved") takes A only once it has appeared.
            mKeyPromptTaps = kKeyPromptTaps;
            mKeyTapAt = mFrame + kKeyPromptFirstTap;
        }
    }
    if (mKeyPromptTaps > 0 && mFrame >= mKeyTapAt) {
        if (!o.saveSequence && mKeyPromptTaps < kKeyPromptTaps) {
            mKeyPromptTaps = 0;  // closed after an earlier tap
        } else {
            note("tap A: key prompt");
            tap(Button::A, kTapFrames, step);
            --mKeyPromptTaps;
            mKeyTapAt = mFrame + kTalkTapInterval;
        }
    }
    if (o.scene == "Game" && o.stage.rfind(toStage, 0) == 0 && o.stage != mExitFrom) {
        note(std::string("reached ") + toStage + " " + std::to_string(mPhaseFrames) + " frames after the menu");
        return true;
    }
    if (mAnswering) {
        if (aimAndPress(o, "Prompt.Yes", step)) {
            note("answered yes");
            mAnswering = false;
        }
        return false;
    }
    switch (mMenu) {
    case MenuStep::OpenPause:
        mExitFrom = o.stage;
        if (mPhaseFrames >= kSettle && o.pausePermitted) {
            mPauseOpenBase = seenCount("PauseMenu.Open");
            tap(Button::Plus, kPauseHold, step);
            mMenu = MenuStep::WaitPause;
            mPhaseFrames = 0;
        } else if (mPhaseFrames >= kPausePermitLimit) {
            finish(Result::Fail, "pausing was not permitted for " + std::to_string(kPausePermitLimit) + " frames", step);
        }
        break;
    case MenuStep::WaitPause:
        if (seenCount("PauseMenu.Open") > mPauseOpenBase) {
            mMenu = MenuStep::PressBack;
            mPhaseFrames = 0;
        } else if (mPhaseFrames >= kPauseMenuLimit) {
            finish(Result::Fail, "no PauseMenu.Open within " + std::to_string(kPauseMenuLimit) + " frames of holding Plus", step);
        }
        break;
    case MenuStep::PressBack:
        if (aimAndPress(o, "PauseMenu.Back", step)) {
            mMenu = MenuStep::Answer;
            mPhaseFrames = 0;
        }
        break;
    case MenuStep::Answer:
        if (mPhaseFrames >= kSceneChangeLimit) {
            finish(Result::Fail, std::string("still on ") + o.stage + " " + std::to_string(kSceneChangeLimit) +
                                     " frames after choosing PauseMenu.Back (expected " + toStage + "*)",
                   step);
        }
        break;
    }
    return false;
}

void SoakDriver::startCycle(const Observation& o, Step& step) {
    ++mCycle;
    mEntry = (mEntry + 1) % mConfig.stages.size();
    const SoakConfig::Entry& entry = mConfig.stages[mEntry];
    if (mConfig.setStage != nullptr) {
        mConfig.setStage(entry.stage, entry.scenario);
    }
    StageConfig stage;
    stage.stage = entry.stage;
    stage.scenario = entry.scenario;
    stage.tailFrames = mConfig.stageTailFrames;
    mStage = std::make_unique<StageDriver>(mCycleFrameLimit, stage);
    note("cycle " + std::to_string(mCycle) + ": " + entry.stage + " scenario " + std::to_string(entry.scenario));
    next(Phase::Stage);
    mSeen.clear();
    // This frame's observation belongs to the new stage cycle (its title milestones).
    const Step inner = mStage->step(o);
    for (const std::string& line : mStage->log()) {
        note("stage: " + line);
    }
    step.presses.insert(step.presses.end(), inner.presses.begin(), inner.presses.end());
    step.assertFocus = step.assertFocus || inner.assertFocus;
    if (inner.pointer) {
        step.pointer = true;
        step.pointerU = inner.pointerU;
        step.pointerV = inner.pointerV;
    }
}

Step SoakDriver::step(const Observation& o) {
    Step step;
    mLog.clear();
    ++mFrame;
    ++mPhaseFrames;
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

    if (mPhase == Phase::Stage) {
        const Step inner = mStage->step(o);
        for (const std::string& line : mStage->log()) {
            note("stage: " + line);
        }
        step.presses.insert(step.presses.end(), inner.presses.begin(), inner.presses.end());
        step.assertFocus = step.assertFocus || inner.assertFocus;
        if (inner.pointer) {
            step.pointer = true;
            step.pointerU = inner.pointerU;
            step.pointerV = inner.pointerV;
        }
        const Result result = mStage->result();
        if (result == Result::Running) {
            return step;
        }
        if (result != Result::Pass) {
            finish(result, "cycle " + std::to_string(mCycle) + " stage " + mStage->reason(), step);
            return step;
        }
        const double minutes = std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count() / 60.0;
        if (minutes >= mConfig.minutes) {
            finish(Result::Pass, std::to_string(mCycle) + " cycles in " + std::to_string(static_cast<int>(minutes)) + " minutes", step);
            return step;
        }
        note("cycle " + std::to_string(mCycle) + " stage passed; leaving through the pause menu");
        next(Phase::ExitStage);
        return step;
    }

    for (const std::string& milestone : o.milestones) {
        mSeen.push_back(milestone);
    }
    if (o.stage != mLastStage) {
        note("stage " + (o.stage.empty() ? std::string("(none)") : o.stage) + " (scene " + o.scene + ")");
        mLastStage = o.stage;
    }

    switch (mPhase) {
    case Phase::ExitStage:
        if (menuExit(o, "Astro", step)) {
            next(Phase::Observatory);
        }
        break;
    case Phase::Observatory: {
        if (o.playerValid && o.playerDead) {
            finish(Result::Fail, "died in the observatory", step);
            break;
        }
        if (handleTalk(o, step)) {
            mReadyFrames = 0;
            break;
        }
        const bool ready = o.sceneReady && o.playerValid && !o.demoActive && o.pausePermitted;
        if (mReadyFrames < kReadyFrames) {
            mReadyFrames = ready ? mReadyFrames + 1 : 0;
            if (mReadyFrames == kReadyFrames) {
                note("observatory ready after " + std::to_string(mPhaseFrames) + " frames");
                mPhaseFrames = 0;
            } else if (mPhaseFrames >= kReadyLimit) {
                finish(Result::Fail, "the observatory was not ready within " + std::to_string(kReadyLimit) + " frames", step);
            }
            break;
        }
        // Idle, then a short walk forward and back.
        if (mPhaseFrames == mConfig.observatoryIdleFrames) {
            tap(Button::StickUp, kWalkFrames, step);
        } else if (mPhaseFrames == mConfig.observatoryIdleFrames + kWalkFrames + kSettle) {
            tap(Button::StickDown, kWalkFrames, step);
        } else if (mPhaseFrames >= mConfig.observatoryIdleFrames + 2 * (kWalkFrames + kSettle)) {
            next(Phase::EndGame);
        }
        break;
    }
    case Phase::EndGame:
        if (menuExit(o, "FileSelect", step)) {
            // The new cycle's reload script sees this frame, and with it the
            // title's first milestones.
            startCycle(o, step);
        }
        break;
    case Phase::WaitTitle:
        startCycle(o, step);
        break;
    case Phase::Stage:
    case Phase::Done:
        break;
    }
    return step;
}

}  // namespace PetariNative::App::Smoke

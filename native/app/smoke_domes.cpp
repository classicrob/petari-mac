// The dome tour (smoke_domes.hpp). No SDK or Aurora headers: the frame seam
// feeds it observations and applies its presses.

#include "smoke_domes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kTapFrames = 6;
constexpr unsigned long kPointingFramesToPress = 3;
constexpr unsigned long kAimLimit = 240;
constexpr unsigned long kTargetMissingLimit = 900;
constexpr unsigned long kReadyFrames = 60;
constexpr unsigned long kReadyLimit = 5400;
constexpr unsigned long kLoadLimit = 5400;
constexpr unsigned long kTalkTapInterval = 45;
constexpr unsigned long kCalibrateHold = 20;
constexpr float kCalibrateMinimum = 10.0f;
constexpr float kArrive = 70.0f;
constexpr float kArriveWarp = 45.0f;
constexpr float kArriveHeight = 300.0f;
constexpr unsigned long kStuckFrames = 180;
constexpr int kRecoveries = 3;
constexpr unsigned long kRouteLimit = 5400;   // per waypoint
constexpr unsigned long kWarpLimit = 900;
constexpr float kWarpJump = 800.0f;
constexpr unsigned long kEnterLimit = 900;
constexpr unsigned long kMapSettle = 30;
constexpr unsigned long kMoveHold = 45;
constexpr unsigned long kMoveAfter = 15;
constexpr unsigned long kSettle = 20;
constexpr unsigned long kPauseHold = 18;
constexpr unsigned long kPauseMenuLimit = 300;
constexpr unsigned long kPausePermitLimit = 900;
constexpr unsigned long kReturnLimit = 3600;

struct Vec {
    float x, y, z;
};
Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
Vec scale(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec position(const Observation& o) { return {o.playerX, o.playerY, o.playerZ}; }
Vec up(const Observation& o) {
    const Vec g{-o.gravityX, -o.gravityY, -o.gravityZ};
    const float l = length(g);
    return l > 1e-3f ? scale(g, 1.0f / l) : Vec{0.0f, 1.0f, 0.0f};
}
// d without its component along unit u.
Vec across(Vec d, Vec u) { return sub(d, scale(u, dot(d, u))); }
std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}
std::string text(Vec a) { return "(" + number(a.x) + ", " + number(a.y) + ", " + number(a.z) + ")"; }

const char* const kDomeNames[] = {"", "Terrace", "Fountain", "Kitchen", "Bedroom", "Engine Room", "Garden"};

}  // namespace

bool domesEnabledFromEnvironment(DomesConfig* config) {
    const char* smoke = std::getenv("PETARI_SMOKE");
    if (smoke == nullptr || std::strcmp(smoke, "domes") != 0) return false;
    const char* dome = std::getenv("PETARI_DOME");
    const long number = dome != nullptr ? std::strtol(dome, nullptr, 10) : 0;
    if (number < 1 || number > 6 || domeRoute(static_cast<int>(number)).empty()) {
        std::fputs("PETARI SMOKE: script domes needs PETARI_DOME (1..6, a dome with a planned route); not running\n", stderr);
        return false;
    }
    config->dome = static_cast<int>(number);
    config->extras.clear();
    if (const char* missions = std::getenv("PETARI_DOME_MISSIONS")) {
        std::string list = missions;
        size_t start = 0;
        while (start < list.size()) {
            size_t end = list.find(',', start);
            if (end == std::string::npos) end = list.size();
            const std::string item = list.substr(start, end - start);
            const size_t colon = item.find(':');
            if (colon != std::string::npos && colon > 0) {
                const long scenario = std::strtol(item.c_str() + colon + 1, nullptr, 10);
                if (scenario >= 1 && scenario <= 8) config->extras.push_back({item.substr(0, colon), static_cast<int>(scenario)});
            }
            start = end + 1;
        }
    }
    return true;
}

DomesDriver::DomesDriver(unsigned long frameLimit, const DomesConfig& config)
    : mBoot(frameLimit, Script::Reload), mConfig(config), mFrameLimit(frameLimit) {}

const char* DomesDriver::phase() const {
    switch (mPhase) {
    case Phase::Boot: return mBoot.phase();
    case Phase::Observatory: return "domes: waiting for the observatory";
    case Phase::Calibrate: return "domes: calibrating the stick";
    case Phase::Route: return "domes: walking to the dome";
    case Phase::Warp: return "domes: waiting for a warp pod";
    case Phase::EnterDome: return "domes: entering the dome";
    case Phase::DomeReady: return "domes: waiting in the dome";
    case Phase::BlueStar: return "domes: pointing at the Blue Star";
    case Phase::GalaxyMap: return "domes: selecting a galaxy";
    case Phase::Confirm: return "domes: confirming the galaxy";
    case Phase::Scenario: return "domes: selecting the mission";
    case Phase::Load: return "domes: loading the mission";
    case Phase::Ready: return "domes: waiting for gameplay";
    case Phase::Move: return "domes: movement check";
    case Phase::PauseOpen: return "domes: opening the pause menu";
    case Phase::PauseBack: return "domes: choosing Back to the Comet Observatory";
    case Phase::Answer: return "domes: returning to the dome";
    case Phase::ReturnDome: return "domes: back in the dome";
    case Phase::Done: return "done";
    }
    return "?";
}

void DomesDriver::next(Phase phase) {
    mPhase = phase;
    mPhaseFrames = 0;
    mAimFrames = mAimMissing = mPointingFrames = 0;
    mReadyFrames = 0;
}

void DomesDriver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.presses.push_back({button, true});
    step.assertFocus = true;
    mReleases.push_back({mFrame + holdFrames, button});
}

void DomesDriver::steer(const StickKeys& keys, Step& step) {
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
            if (key.want) step.assertFocus = true;
        }
    }
    mHeld = keys;
}

unsigned long DomesDriver::seenCount(const char* milestone) const {
    return static_cast<unsigned long>(std::count(mSeen.begin(), mSeen.end(), milestone));
}

bool DomesDriver::aimAndPress(const Observation& o, const std::string& id, int index, Step& step) {
    const Observation::Target* target = nullptr;
    for (const Observation::Target& candidate : o.targets) {
        if (candidate.id == id && (candidate.flags & kTargetSelectable) && (index < 0 || candidate.index == index)) {
            target = &candidate;
            break;
        }
    }
    const std::string label = id + (index >= 0 ? " " + std::to_string(index) : "");
    if (target == nullptr) {
        mAimFrames = mPointingFrames = 0;
        if (++mAimMissing >= kTargetMissingLimit) {
            finish(Result::Fail, "no selectable " + label + " shown for " + std::to_string(kTargetMissingLimit) + " frames", step);
        }
        return false;
    }
    mAimMissing = 0;
    step.pointer = true;
    step.pointerU = target->u;
    step.pointerV = target->v;
    step.assertFocus = true;
    if (mAimFrames++ == 0) note("point at " + label);
    mPointingFrames = (target->flags & kTargetPointing) ? mPointingFrames + 1 : 0;
    if (mPointingFrames >= kPointingFramesToPress) {
        note("tap A: " + label);
        tap(Button::A, kTapFrames, step);
        mAimFrames = mPointingFrames = 0;
        return true;
    }
    if (mAimFrames >= kAimLimit) {
        finish(Result::Fail, "the pointer never got over " + label, step);
    }
    return false;
}

bool DomesDriver::handleTalk(const Observation& o, Step& step) {
    bool talkTarget = false;
    for (const Observation::Target& target : o.targets) {
        talkTarget = talkTarget || (target.id == "Talk.Advance" && (target.flags & kTargetSelectable));
    }
    if (!o.talkActive && !talkTarget) return false;
    steer({}, step);
    mReadyFrames = 0;
    if (mFrame >= mTalkTapAt) {
        note("talk open; tap A");
        tap(Button::A, kTapFrames, step);
        mTalkTapAt = mFrame + kTalkTapInterval;
    }
    return true;
}

bool DomesDriver::gameplayReady(const Observation& o) {
    const bool ready = o.scene == "Game" && o.sceneReady && o.playerValid && !o.demoActive && o.pausePermitted && !o.talkActive;
    mReadyFrames = ready ? mReadyFrames + 1 : 0;
    return mReadyFrames >= kReadyFrames;
}

void DomesDriver::closeVisit(const std::string& status) {
    if (mVisit >= mVisits.size()) return;
    Visit& visit = mVisits[mVisit];
    visit.status = status;
    note("DOMES VISIT dome " + std::to_string(mConfig.dome) + " galaxy " + visit.galaxy + " scenario " +
         std::to_string(visit.scenario) + ": " + status + " (load " + std::to_string(visit.loadFrames) + " frames, ready after " +
         std::to_string(visit.readyFrames) + ", moved " + number(visit.moved) + ")");
}

void DomesDriver::finish(Result result, const std::string& reason, Step& step) {
    for (const Release& release : mReleases) step.presses.push_back({release.button, false});
    mReleases.clear();
    steer({}, step);
    if (result != Result::Pass && mVisit < mVisits.size() && mVisits[mVisit].status.empty() && mPhase >= Phase::Confirm) {
        closeVisit("FAIL " + reason);
    }
    mResult = result;
    std::string summary;
    int passed = 0;
    for (const Visit& visit : mVisits) {
        passed += visit.status == "PASS" ? 1 : 0;
        summary += (summary.empty() ? "" : ", ") + visit.galaxy + ":" + std::to_string(visit.scenario) + " " +
                   (visit.status.empty() ? "not reached" : visit.status == "PASS" ? "PASS" : "FAIL");
    }
    mReason = std::string("dome ") + std::to_string(mConfig.dome) + " (" + kDomeNames[mConfig.dome] + "), " +
              std::to_string(passed) + "/" + std::to_string(mVisits.size()) + " visits passed: " + reason;
    note("DOMES SUMMARY dome " + std::to_string(mConfig.dome) + ": " + (summary.empty() ? "no galaxies reached" : summary));
    mPhase = Phase::Done;
    step.requestQuit = true;
    note(std::string(resultName(mResult)) + ": " + mReason);
}

Step DomesDriver::step(const Observation& o) {
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
    if (mResult != Result::Running) return step;

    if (mPhase == Phase::Boot) {
        for (const std::string& milestone : o.milestones) {
            if (milestone == "FileSelector.DemoStartWait") mDemoStarted = true;
        }
        const bool entered = mDemoStarted && o.scene == "Game" && !o.stage.empty() && o.stage != "FileSelect";
        if (!entered) {
            Step boot = mBoot.step(o);
            mLog = mBoot.log();
            step.presses.insert(step.presses.end(), boot.presses.begin(), boot.presses.end());
            step.assertFocus = step.assertFocus || boot.assertFocus;
            step.pointer = boot.pointer;
            step.pointerU = boot.pointerU;
            step.pointerV = boot.pointerV;
            if (mBoot.result() != Result::Running) {
                mResult = mBoot.result();
                mReason = "boot (reload script): " + mBoot.reason();
                mPhase = Phase::Done;
                step.requestQuit = true;
            }
            return step;
        }
        step.presses.push_back({Button::A, false});
        step.presses.push_back({Button::B, false});
        mLastScene = o.scene;
        mLastStage = o.stage;
        mPhysicalBase = o.physical.gameplay;
        note("file loaded into " + o.stage + " scenario " + std::to_string(o.scenario) + "; dome " +
             std::to_string(mConfig.dome) + " (" + kDomeNames[mConfig.dome] + ")");
        if (o.stage != "AstroGalaxy") {
            finish(Result::Fail, "the file loaded into " + o.stage + ", not the observatory", step);
            return step;
        }
        next(Phase::Observatory);
        return step;
    }

    if (o.scene != mLastScene || o.stage != mLastStage) {
        note("scene " + (o.scene.empty() ? std::string("(none)") : o.scene) + (o.stage.empty() ? "" : " stage " + o.stage) +
             " scenario " + std::to_string(o.scenario) + " (selected " + std::to_string(o.selectedScenario) + ")");
        mLastScene = o.scene;
        mLastStage = o.stage;
    }
    for (const std::string& milestone : o.milestones) {
        mSeen.push_back(milestone);
    }
    for (const Observation::Prompt& prompt : o.prompts) {
        note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
        if (mPhase == Phase::Answer && prompt.type == 2) {
            mAnswering = true;
            mAimFrames = mPointingFrames = mAimMissing = 0;
        } else {
            finish(Result::Blocked, "unexpected system prompt " + prompt.messageId + " while " + phase(), step);
            return step;
        }
    }
    if (mFrame >= mFrameLimit) {
        finish(Result::Fail, "frame limit " + std::to_string(mFrameLimit) + " reached while " + phase(), step);
        return step;
    }
    if (o.playerValid && o.playerDead) {
        finish(Result::Fail, "Mario died while " + std::string(phase()) + " at " + text(position(o)), step);
        return step;
    }

    if (mPhase <= Phase::EnterDome) {
        route(o, step);
    } else {
        dome(o, step);
    }
    if (mResult == Result::Pass && o.physical.gameplay > mPhysicalBase) {
        mResult = Result::Assisted;
        mReason += "; ASSISTED: " + std::to_string(o.physical.gameplay - mPhysicalBase) + " physical gameplay input(s)";
    }
    return step;
}

void DomesDriver::route(const Observation& o, Step& step) {
    const std::vector<DomeWaypoint>& points = domeRoute(mConfig.dome);
    if (o.scene == "Game" && o.stage == "AstroDome") {
        steer({}, step);
        if (!o.sceneReady) return;
        if (o.scenario != mConfig.dome) {
            finish(Result::Fail, "entered dome " + std::to_string(o.scenario) + ", not " + std::to_string(mConfig.dome), step);
            return;
        }
        note("entered dome " + std::to_string(mConfig.dome) + " (" + kDomeNames[mConfig.dome] + ") at route waypoint " +
             std::to_string(mWaypoint) + "/" + std::to_string(points.size()));
        next(Phase::DomeReady);
        return;
    }
    if (o.stage != "AstroGalaxy") {
        if (o.scene == "Game" && o.sceneReady) {
            finish(Result::Fail, "left the observatory for " + o.stage + " on the route", step);
        }
        return;
    }
    if (handleTalk(o, step)) return;
    const Vec pos = position(o);
    const Vec u = up(o);
    const Vec right = across(Vec{o.camXx, o.camXy, o.camXz}, u);
    const Vec forward = across(Vec{o.camZx, o.camZy, o.camZz}, u);

    if (mPhase == Phase::Observatory) {
        steer({}, step);
        if (gameplayReady(o)) {
            note("observatory ready at " + text(pos));
            next(Phase::Calibrate);
            mCalX = pos.x; mCalY = pos.y; mCalZ = pos.z;
        } else if (mPhaseFrames >= kReadyLimit) {
            finish(Result::Fail, "the observatory never became playable", step);
        }
        return;
    }
    if (!o.playerValid) return;
    if (mPhase == Phase::Calibrate) {
        if (mPhaseFrames == 1) {
            steer({true, false, false, false}, step);
        } else if (mPhaseFrames == 1 + kCalibrateHold) {
            const Vec delta = sub(pos, Vec{mCalX, mCalY, mCalZ});
            if (length(delta) < kCalibrateMinimum) {
                finish(Result::Fail, "forward calibration did not move Mario", step);
                return;
            }
            mSignForward = dot(delta, forward) >= 0 ? 1.0f : -1.0f;
            mCalX = pos.x; mCalY = pos.y; mCalZ = pos.z;
            steer({false, false, false, true}, step);
        } else if (mPhaseFrames == 1 + 2 * kCalibrateHold) {
            const Vec delta = sub(pos, Vec{mCalX, mCalY, mCalZ});
            if (length(delta) < kCalibrateMinimum) {
                finish(Result::Fail, "right calibration did not move Mario", step);
                return;
            }
            mSignRight = dot(delta, right) >= 0 ? 1.0f : -1.0f;
            steer({}, step);
            note("calibrated: forward " + number(mSignForward) + ", right " + number(mSignRight) + " at " + text(pos));
            next(Phase::Route);
            mBestDistance = 1e30f;
        }
        return;
    }
    if (mPhase == Phase::Warp) {
        steer({}, step);
        const float moved = length(sub(pos, Vec{mWarpX, mWarpY, mWarpZ}));
        if (moved >= kWarpJump && o.playerOnGround && !o.playerInBind) {
            note("warp pod carried Mario " + number(moved) + " units to " + text(pos));
            ++mWaypoint;
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            mRecoveries = 0;
            next(Phase::Route);
        } else if (mPhaseFrames >= kWarpLimit) {
            finish(Result::Fail, "the warp pod at waypoint " + std::to_string(mWaypoint) + " did not carry Mario (at " + text(pos) + ")",
                   step);
        }
        return;
    }
    if (mPhase == Phase::EnterDome) {
        steer({}, step);
        if (mPhaseFrames >= kEnterLimit) {
            finish(Result::Fail, "route ended at " + text(pos) + " but the dome was not entered", step);
        }
        return;
    }
    // Route.
    if (mWaypoint >= points.size()) {
        next(Phase::EnterDome);
        return;
    }
    const DomeWaypoint& target = points[mWaypoint];
    const Vec delta = across(Vec{target.x - pos.x, target.y - pos.y, target.z - pos.z}, u);
    const float distance = length(delta);
    const float height = std::fabs(dot(Vec{target.x - pos.x, target.y - pos.y, target.z - pos.z}, u));
    if (mPhaseFrames % 120 == 0) {
        note("route waypoint " + std::to_string(mWaypoint) + ": " + number(distance) + " away, height " + number(height) + ", at " + text(pos));
    }
    if (distance < (target.action == DomeWaypoint::Warp ? kArriveWarp : kArrive) && height < kArriveHeight) {
        if (target.action == DomeWaypoint::Jump) {
            note("jump at waypoint " + std::to_string(mWaypoint) + " from " + text(pos));
            tap(Button::A, 12, step);
        }
        if (target.action == DomeWaypoint::Warp) {
            steer({}, step);
            note("on warp pod (waypoint " + std::to_string(mWaypoint) + ") at " + text(pos));
            mWarpX = pos.x; mWarpY = pos.y; mWarpZ = pos.z;
            next(Phase::Warp);
            return;
        }
        ++mWaypoint;
        mBestDistance = 1e30f;
        mStuckFrames = 0;
        mRecoveries = 0;
        mPhaseFrames = 0;
        note("route waypoint " + std::to_string(mWaypoint) + " at " + text(pos));
        if (mWaypoint >= points.size()) {
            // The last point is inside the dome's entrance area: keep walking
            // the same way until the scene changes.
            next(Phase::EnterDome);
        }
        return;
    }
    if (mPhaseFrames >= kRouteLimit) {
        finish(Result::Fail, "route waypoint " + std::to_string(mWaypoint) + " not reached within " + std::to_string(kRouteLimit) +
                                 " frames, at " + text(pos),
               step);
        return;
    }
    if (distance < mBestDistance - 30.0f) {
        mBestDistance = distance;
        mStuckFrames = 0;
    } else if (++mStuckFrames >= kStuckFrames) {
        mStuckFrames = 0;
        if (++mRecoveries > kRecoveries) {
            finish(Result::Fail, "route stuck at " + text(pos) + " before waypoint " + std::to_string(mWaypoint) + " " +
                                     text(Vec{target.x, target.y, target.z}),
                   step);
            return;
        }
        note("stuck before waypoint " + std::to_string(mWaypoint) + " at " + text(pos) + "; jump");
        tap(Button::A, kTapFrames, step);
    }
    const float l = length(right) > 1e-3f && length(forward) > 1e-3f ? 1.0f : 0.0f;
    if (l == 0.0f) {
        steer({}, step);
        return;
    }
    steer(stickKeysFor(dot(delta, scale(right, 1.0f / length(right))) * mSignRight,
                       dot(delta, scale(forward, 1.0f / length(forward))) * mSignForward),
          step);
}

void DomesDriver::dome(const Observation& o, Step& step) {
    const Visit* visit = current();
    const bool inDome = o.scene == "Game" && o.stage == "AstroDome";
    switch (mPhase) {
    case Phase::DomeReady:
    case Phase::ReturnDome:
        if (handleTalk(o, step)) return;
        if (!inDome) {
            finish(Result::Fail, "expected the dome, found " + o.stage, step);
            return;
        }
        if (gameplayReady(o)) {
            next(Phase::BlueStar);
        } else if (mPhaseFrames >= kReadyLimit) {
            finish(Result::Fail, "the dome never became playable", step);
        }
        return;
    case Phase::BlueStar:
        if (aimAndPress(o, "Dome.BlueStar", -1, step)) next(Phase::GalaxyMap);
        return;
    case Phase::GalaxyMap: {
        std::vector<std::string> galaxies;
        std::string unlock;
        for (const Observation::Target& target : o.targets) {
            if (!(target.flags & kTargetSelectable)) continue;
            if (target.id.rfind("Galaxy.Unlock", 0) == 0) {
                unlock = target.id;
            } else if (target.id.rfind("Galaxy.", 0) == 0 && target.id != "Galaxy.Start") {
                galaxies.push_back(target.id.substr(7));
            }
        }
        if (!unlock.empty()) {
            // A galaxy still waiting for its reveal: the save left an opening demo.
            finish(Result::Fail, "the galaxy map shows an unrevealed galaxy (" + unlock + "): a pending opening demo", step);
            return;
        }
        if (!mMapRecorded) {
            if (galaxies.empty()) {
                if (mPhaseFrames >= kTargetMissingLimit) finish(Result::Fail, "the galaxy map showed no selectable galaxy", step);
                return;
            }
            if (mPhaseFrames < kMapSettle) return;  // let every miniature publish
            std::sort(galaxies.begin(), galaxies.end());
            galaxies.erase(std::unique(galaxies.begin(), galaxies.end()), galaxies.end());
            std::string list;
            for (const std::string& galaxy : galaxies) {
                list += (list.empty() ? "" : ", ") + galaxy;
                mVisits.push_back({galaxy, 1});
            }
            for (const DomesConfig::Mission& extra : mConfig.extras) {
                if (std::find(galaxies.begin(), galaxies.end(), extra.galaxy) != galaxies.end()) {
                    mVisits.push_back({extra.galaxy, extra.scenario});
                } else {
                    note("extra mission " + extra.galaxy + ":" + std::to_string(extra.scenario) + " is not on this dome's map; skipped");
                }
            }
            mMapRecorded = true;
            note("DOMES MAP dome " + std::to_string(mConfig.dome) + " (" + kDomeNames[mConfig.dome] + "): " + list);
            visit = current();
        }
        if (visit == nullptr) {
            finish(Result::Pass, "every galaxy on the map selected, loaded and ready", step);
            return;
        }
        if (aimAndPress(o, "Galaxy." + visit->galaxy, -1, step)) next(Phase::Confirm);
        return;
    }
    case Phase::Confirm:
        if (aimAndPress(o, "Galaxy.Start", -1, step)) next(Phase::Scenario);
        return;
    case Phase::Scenario:
        if (aimAndPress(o, "Scenario.Star", visit->scenario, step)) next(Phase::Load);
        return;
    case Phase::Load:
        if (o.scene == "Game" && o.stage == visit->galaxy && o.sceneReady) {
            mVisits[mVisit].loadFrames = mPhaseFrames;
            if (o.selectedScenario != visit->scenario) {
                finish(Result::Fail, visit->galaxy + " loaded selected scenario " + std::to_string(o.selectedScenario) + " (placed " +
                                         std::to_string(o.scenario) + "), not " + std::to_string(visit->scenario),
                       step);
                return;
            }
            note("loaded " + visit->galaxy + " scenario " + std::to_string(o.selectedScenario) + " (placed " +
                 std::to_string(o.scenario) + ") after " + std::to_string(mPhaseFrames) + " frames");
            next(Phase::Ready);
        } else if (o.scene == "Game" && o.sceneReady && o.stage != "AstroDome" && o.stage != visit->galaxy) {
            finish(Result::Fail, "selected " + visit->galaxy + " but " + o.stage + " loaded", step);
        } else if (mPhaseFrames >= kLoadLimit) {
            finish(Result::Fail, visit->galaxy + " did not finish loading within " + std::to_string(kLoadLimit) + " frames", step);
        }
        return;
    case Phase::Ready:
        if (o.stage != visit->galaxy) {
            finish(Result::Fail, "left " + visit->galaxy + " for " + o.stage + " before it became ready", step);
            return;
        }
        if (handleTalk(o, step)) return;
        if (gameplayReady(o)) {
            mVisits[mVisit].readyFrames = mPhaseFrames;
            note("ready in " + visit->galaxy + " at " + text(position(o)) + (o.playerOnGround ? ", on the ground" : ", in the air"));
            mMoveX = o.playerX; mMoveY = o.playerY; mMoveZ = o.playerZ;
            tap(Button::StickUp, kMoveHold, step);
            next(Phase::Move);
        } else if (mPhaseFrames >= kReadyLimit) {
            finish(Result::Fail, std::string("gameplay never became ready in ") + visit->galaxy + " (player " +
                                     (o.playerValid ? "present" : "missing") + ", demo " + (o.demoActive ? "active" : "idle") +
                                     ", pause " + (o.pausePermitted ? "permitted" : "not permitted") + ")",
                   step);
        }
        return;
    case Phase::Move:
        if (o.stage != visit->galaxy) {
            finish(Result::Fail, "left " + visit->galaxy + " during the movement check", step);
            return;
        }
        if (mPhaseFrames >= kMoveHold + kMoveAfter) {
            const Vec moved = across(sub(position(o), Vec{mMoveX, mMoveY, mMoveZ}), up(o));
            mVisits[mVisit].moved = length(moved);
            note("movement check: stick up " + std::to_string(kMoveHold) + " frames moved " + number(length(moved)) + " units" +
                 (length(moved) >= 40.0f ? "" : " (warn: little movement)"));
            next(Phase::PauseOpen);
        }
        return;
    case Phase::PauseOpen:
        if (handleTalk(o, step)) return;
        if (mPauseOpenBase == 0) {
            if (mPhaseFrames > kSettle && o.pausePermitted) {
                mPauseOpenBase = seenCount("PauseMenu.Open") + 1;
                mPhaseFrames = 0;
                tap(Button::Plus, kPauseHold, step);
            } else if (mPhaseFrames >= kPausePermitLimit) {
                finish(Result::Fail, "pausing not permitted in " + visit->galaxy, step);
            }
        } else if (seenCount("PauseMenu.Open") >= mPauseOpenBase) {
            mPauseOpenBase = 0;
            next(Phase::PauseBack);
        } else if (mPhaseFrames >= kPauseMenuLimit) {
            finish(Result::Fail, "no PauseMenu.Open after holding Plus in " + visit->galaxy, step);
        }
        return;
    case Phase::PauseBack:
        if (aimAndPress(o, "PauseMenu.Back", -1, step)) next(Phase::Answer);
        return;
    case Phase::Answer:
        if (inDome && o.sceneReady) {
            if (o.scenario != mConfig.dome) {
                finish(Result::Fail, "the galaxy exit returned to dome " + std::to_string(o.scenario), step);
                return;
            }
            closeVisit("PASS");
            ++mVisit;
            mAnswering = false;
            next(Phase::ReturnDome);
            return;
        }
        if (mAnswering && aimAndPress(o, "Prompt.Yes", -1, step)) {
            note("answered yes");
            mAnswering = false;
        }
        if (mPhaseFrames >= kReturnLimit) {
            finish(Result::Fail, "not back in the dome " + std::to_string(kReturnLimit) + " frames after Back to the Comet Observatory",
                   step);
        }
        return;
    default:
        return;
    }
}

}  // namespace PetariNative::App::Smoke

// The dome tour (smoke_domes.hpp). No SDK or Aurora headers: the frame seam
// feeds it observations and applies its presses.

#include "smoke_domes.hpp"
#include <petari/efb_dump_mark.hpp>

#include <algorithm>
#include <set>
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
constexpr unsigned long kJumpHold = 12;
constexpr unsigned long kJumpSpinDelay = 16;
constexpr float kKickReach = 200.0f;          // wall contact within this (across gravity) of the recorded one
constexpr float kKickReachHeight = 400.0f;    // and within this along gravity
// A held through a wall-kick chain's hop and kicks, as in the recorded route
// (27-33 frames): the jump and each kick rise higher while A is held.
constexpr unsigned long kKickHold = 30;
constexpr unsigned long kKickRepress = 2;  // frames between releasing a held A and the kick press
// Before a kick chain's hop, walk into the first wall this long: a wall kick
// leaves opposite to Mario's heading, so he must face the wall, whatever his approach.
constexpr unsigned long kKickSettle = 10;
constexpr float kKickLandingHeight = 60.0f;  // grounded this near the chain's landing level: it is done
constexpr size_t kKickResumeWindow = 15;     // route points after the chain considered for resuming
constexpr float kClingStill = 0.5f;           // per-frame movement of Mario clinging to a wall
constexpr unsigned long kKickGroundLimit = 20;  // grounded this long: the kick chain fell short
// After a kick chain lands, grounded this far below its landing level within
// kChainWatch frames: Mario walked off a ledge the chain should not have ended on
// (at the Engine Room chimney's corner the second kick can leave along either
// wall face, depending on a few units of approach); retry the chain from its Hop.
constexpr float kChainFallHeight = 200.0f;
constexpr unsigned long kChainWatch = 600;
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

const char* const kDomeNames[] = {"", "Terrace", "Fountain", "Kitchen", "Bedroom", "Engine Room", "Garden", "Grand Finale"};

}  // namespace

namespace {
// PETARI_DOME_ROUTE=<csv of x,y,z,action> (action Walk, Warp or Jump): an
// experimental route instead of the planned one, for route development.
std::vector<DomeWaypoint> gRouteOverride;
bool gRouteOverridden = false;

void loadRouteOverride() {
    const char* path = std::getenv("PETARI_DOME_ROUTE");
    if (path == nullptr || path[0] == '\0') return;
    std::FILE* in = std::fopen(path, "r");
    if (in == nullptr) {
        std::fprintf(stderr, "PETARI SMOKE: cannot read PETARI_DOME_ROUTE %s\n", path);
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), in) != nullptr) {
        float x, y, z;
        char action[16] = {};
        if (std::sscanf(line, "%f,%f,%f,%15s", &x, &y, &z, action) == 4) {
            const DomeWaypoint::Action kind = std::strncmp(action, "Warp", 4) == 0 ? DomeWaypoint::Warp
                                              : std::strncmp(action, "Jump", 4) == 0 ? DomeWaypoint::Jump
                                              : std::strncmp(action, "Hop", 3) == 0 ? DomeWaypoint::Hop
                                              : std::strncmp(action, "Spin", 4) == 0 ? DomeWaypoint::Spin
                                              : std::strncmp(action, "Launch", 6) == 0 ? DomeWaypoint::Launch
                                              : std::strncmp(action, "Talk", 4) == 0 ? DomeWaypoint::Talk
                                              : std::strncmp(action, "Kick", 4) == 0 ? DomeWaypoint::Kick
                                                                                    : DomeWaypoint::Walk;
            gRouteOverride.push_back({x, y, z, kind});
        }
    }
    std::fclose(in);
    gRouteOverridden = true;
    std::fprintf(stderr, "PETARI SMOKE: dome route from %s (%zu points)\n", path, gRouteOverride.size());
}

const std::vector<DomeWaypoint>& routeFor(int dome) {
    return gRouteOverridden ? gRouteOverride : domeRoute(dome);
}
}  // namespace

bool domesEnabledFromEnvironment(DomesConfig* config) {
    loadRouteOverride();
    const char* smoke = std::getenv("PETARI_SMOKE");
    if (smoke == nullptr || std::strcmp(smoke, "domes") != 0) return false;
    const char* dome = std::getenv("PETARI_DOME");
    const long number = dome != nullptr ? std::strtol(dome, nullptr, 10) : 0;
    if (number < 1 || number > 7 || routeFor(static_cast<int>(number)).empty()) {
        std::fputs("PETARI SMOKE: script domes needs PETARI_DOME (1..7, a destination with a planned route); not running\n", stderr);
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
    case Phase::Launch: return "domes: using a Launch Star";
    case Phase::Talk: return "domes: talking to the Grand Finale Luma";
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
    if (phase == Phase::Scenario && !mVisits.empty() && mVisit < mVisits.size()) {
        // Opt-in image dumps (PETARI_SURFACE_DUMP and friends); inert otherwise. One label per galaxy.
        static std::set<std::string> labels;
        PetariNative::EfbDump::mark(labels.insert("dome-select-" + mVisits[mVisit].galaxy).first->c_str());
    }
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
    if (mKickAt != 0 && mFrame >= mKickAt) {
        tap(Button::A, kKickHold, step);
        mKickAt = 0;
    }
    if (mSpinAt != 0 && mFrame >= mSpinAt) {
        tap(Button::Spin, kTapFrames, step);
        mSpinAt = 0;
    }

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
        if ((mPhase == Phase::Answer || mPhase == Phase::Talk) && prompt.type == 2) {
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
    const std::vector<DomeWaypoint>& points = routeFor(mConfig.dome);
    if (mConfig.dome == 7 && o.stage == "PeachCastleFinalGalaxy") {
        steer({}, step);
        if (mVisits.empty()) mVisits.push_back({"PeachCastleFinalGalaxy", 1});
        const bool starShown = std::any_of(o.targets.begin(), o.targets.end(), [](const Observation::Target& target) {
            return target.id == "Scenario.Star" && (target.flags & kTargetSelectable);
        });
        if (starShown) next(Phase::Scenario);
        else if (o.scene == "Game" && o.sceneReady) {
            finish(Result::Fail, "Grand Finale entered without observed scenario selection", step);
        }
        return;
    }
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
    if (mPhase != Phase::Talk && handleTalk(o, step)) return;
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
    if (mPhase == Phase::Talk) {
        steer({}, step);
        mTalkWasActive = mTalkWasActive || o.talkActive;
        const bool yesShown = std::any_of(o.targets.begin(), o.targets.end(), [](const Observation::Target& target) {
            return target.id == "Prompt.Yes" && (target.flags & kTargetSelectable);
        });
        if (yesShown) {
            if (aimAndPress(o, "Prompt.Yes", -1, step)) note("accepted Grand Finale Luma invitation");
        } else if (o.talkActive) {
            handleTalk(o, step);
        } else if (mTalkWasActive) {
            ++mWaypoint;
            mTalkWasActive = false;
            mBestDistance = 1e30f;
            next(Phase::Route);
        } else {
            const bool startShown = std::any_of(o.targets.begin(), o.targets.end(), [](const Observation::Target& target) {
                return target.id == "Talk.Start" && (target.flags & kTargetSelectable);
            });
            if (startShown && mFrame >= mTalkTapAt) {
                note("start Grand Finale Luma talk");
                tap(Button::A, kTapFrames, step);
                mTalkTapAt = mFrame + kTalkTapInterval;
            }
        }
        if (mPhase == Phase::Talk && mPhaseFrames >= kReadyLimit) finish(Result::Fail, "Grand Finale Luma talk did not finish", step);
        return;
    }
    if (mPhase == Phase::Launch) {
        // Finish walking onto the recorded capture point: some stars catch
        // Mario as he walks into them (the Engine Room approach).
        const DomeWaypoint* star = mWaypoint < points.size() ? &points[mWaypoint] : nullptr;
        const Vec toStar = star != nullptr ? across(Vec{star->x - pos.x, star->y - pos.y, star->z - pos.z}, u) : Vec{};
        if (star != nullptr && !o.playerInBind && length(toStar) > 15.0f && length(right) > 1e-3f && length(forward) > 1e-3f &&
            length(sub(pos, Vec{mWarpX, mWarpY, mWarpZ})) < kWarpJump) {
            steer(stickKeysFor(dot(toStar, scale(right, 1.0f / length(right))) * mSignRight,
                               dot(toStar, scale(forward, 1.0f / length(forward))) * mSignForward),
                  step);
        } else {
            steer({}, step);
        }
        const float moved = length(sub(pos, Vec{mWarpX, mWarpY, mWarpZ}));
        if (moved >= kWarpJump && o.playerOnGround && !o.playerInBind) {
            note("Launch Star landed at " + text(pos));
            ++mWaypoint;
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            mRecoveries = 0;
            next(Phase::Route);
        } else if (moved < kWarpJump) {
            if (mPhaseFrames % 60 == 1 && o.playerOnGround && !o.playerInBind) tap(Button::A, kJumpHold, step);
            if (mPhaseFrames % 30 == 16) tap(Button::Spin, kTapFrames, step);
        }
        if (mPhase == Phase::Launch && mPhaseFrames >= kReadyLimit) finish(Result::Fail, "Launch Star did not reach its destination", step);
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
    if (mChainHop != static_cast< size_t >(-1)) {
        const float drop = dot(Vec{mChainLandX - pos.x, mChainLandY - pos.y, mChainLandZ - pos.z}, u);
        if (o.playerOnGround && drop > kChainFallHeight) {
            const size_t hop = mChainHop;
            mChainHop = static_cast< size_t >(-1);
            if (++mKickRetries > kRecoveries) {
                finish(Result::Fail, "fell " + number(drop) + " below the wall-kick chain's landing level at " + text(pos) +
                                         "; no retries left",
                       step);
                return;
            }
            note("fell " + number(drop) + " below the wall-kick chain's landing level at " + text(pos) + "; retrying the chain from waypoint " +
                 std::to_string(hop));
            mWaypoint = hop;
            mAwaitJumpLanding = false;
            mKickGrounded = 0;
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            mRecoveries = 0;
            mPhaseFrames = 0;
            return;
        }
        if (++mChainWatch > kChainWatch) mChainHop = static_cast< size_t >(-1);
    }
    const Vec last{mLastX, mLastY, mLastZ};
    mLastX = pos.x; mLastY = pos.y; mLastZ = pos.z;
    const auto steerAlong = [&](Vec leg) {
        leg = across(leg, u);
        if (length(leg) < 1e-3f || length(right) < 1e-3f || length(forward) < 1e-3f) {
            steer({}, step);
            return;
        }
        steer(stickKeysFor(dot(leg, scale(right, 1.0f / length(right))) * mSignRight,
                           dot(leg, scale(forward, 1.0f / length(forward))) * mSignForward),
              step);
    };
    if (mKickSettle > 0) {
        // Walking into the first wall before the chain's hop (target is the Hop).
        const DomeWaypoint& wall = points[mWaypoint + 1];
        if (++mKickSettle <= kKickSettle) {
            steerAlong(Vec{wall.x - target.x, wall.y - target.y, wall.z - target.z});
            return;
        }
        mKickSettle = 0;
        note("jump and spin at waypoint " + std::to_string(mWaypoint) + " from " + text(pos) + " (facing the wall)");
        mAwaitJumpLanding = true;
        tap(Button::A, kKickHold, step);
        ++mWaypoint;
        mBestDistance = 1e30f;
        mStuckFrames = 0;
        mPhaseFrames = 0;
        return;
    }
    if (target.action == DomeWaypoint::Kick) {
        // Steer along the recorded leg (into the wall), not at the contact
        // point: Mario rises past it, so the direction to it is unstable.
        const DomeWaypoint& from = points[mWaypoint > 0 ? mWaypoint - 1 : 0];
        const Vec leg = across(Vec{target.x - from.x, target.y - from.y, target.z - from.z}, u);
        const Vec contact = sub(Vec{target.x, target.y, target.z}, pos);
        const bool clinging = !o.playerOnGround && !o.playerInBind && length(sub(pos, last)) < kClingStill;
        if (mPhaseFrames % 3 == 0) note("wall kick leg " + std::to_string(mWaypoint) + " at " + text(pos) + (o.playerOnGround ? " grounded" : ""));
        if (clinging && length(across(contact, u)) < kKickReach && std::fabs(dot(contact, u)) < kKickReachHeight) {
            note("wall kick at waypoint " + std::to_string(mWaypoint) + " from " + text(pos));
            // A may still be held from the hop or last kick: release it now and
            // press again shortly (the recording re-pressed after 6 clinging frames).
            const auto held = std::find_if(mReleases.begin(), mReleases.end(), [](const Release& r) { return r.button == Button::A; });
            if (held != mReleases.end()) {
                step.presses.push_back({Button::A, false});
                mReleases.erase(held);
                mKickAt = mFrame + kKickRepress;
            } else {
                tap(Button::A, kKickHold, step);
            }
            ++mWaypoint;
            mKickGrounded = 0;
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            mPhaseFrames = 0;
            return;
        }
        mKickGrounded = o.playerOnGround ? mKickGrounded + 1 : 0;
        size_t landing = mWaypoint;
        while (landing < points.size() && points[landing].action == DomeWaypoint::Kick) ++landing;
        if (mKickGrounded >= 2 && landing < points.size() &&
            std::fabs(dot(Vec{points[landing].x - pos.x, points[landing].y - pos.y, points[landing].z - pos.z}, u)) < kKickLandingHeight) {
            // Kicked onto the landing level early (a kick can end in a ledge grab on
            // either side of the chimney): continue from the nearest of the next
            // route points on that level, not necessarily the first.
            size_t resume = landing;
            float best = 1e30f;
            for (size_t i = landing; i < points.size() && i < landing + kKickResumeWindow; ++i) {
                const Vec d{points[i].x - pos.x, points[i].y - pos.y, points[i].z - pos.z};
                if (points[i].action != DomeWaypoint::Walk || std::fabs(dot(d, u)) >= kKickLandingHeight) continue;
                if (length(across(d, u)) < best) {
                    best = length(across(d, u));
                    resume = i;
                }
            }
            note("wall-kick chain reached its landing level at " + text(pos) + "; continuing to waypoint " + std::to_string(resume));
            size_t hop = mWaypoint;
            while (hop > 0 && points[hop].action != DomeWaypoint::Hop) --hop;
            if (points[hop].action == DomeWaypoint::Hop) {
                mChainHop = hop;
                mChainLandX = pos.x; mChainLandY = pos.y; mChainLandZ = pos.z;
                mChainWatch = 0;
            }
            mWaypoint = resume;
            mKickGrounded = 0;
            mAwaitJumpLanding = false;
            mBestDistance = 1e30f;
            mStuckFrames = 0;
            mPhaseFrames = 0;
            return;
        }
        if (mKickGrounded >= kKickGroundLimit) {
            // Fell back without a kick: walk back to the chain's Hop and retry.
            size_t hop = mWaypoint;
            while (hop > 0 && points[hop].action != DomeWaypoint::Hop) --hop;
            mKickGrounded = 0;
            if (points[hop].action != DomeWaypoint::Hop || ++mKickRetries > kRecoveries) {
                finish(Result::Fail, "wall kick at waypoint " + std::to_string(mWaypoint) + " " + text(Vec{target.x, target.y, target.z}) +
                                         " not reached, at " + text(pos),
                       step);
                return;
            }
            note("wall kick at waypoint " + std::to_string(mWaypoint) + " fell short at " + text(pos) + "; retrying from waypoint " +
                 std::to_string(hop));
            mWaypoint = hop;
            mAwaitJumpLanding = false;
            mPhaseFrames = 0;
            return;
        }
        if (mPhaseFrames >= kRouteLimit) {
            finish(Result::Fail, "wall kick at waypoint " + std::to_string(mWaypoint) + " not reached within " + std::to_string(kRouteLimit) +
                                     " frames, at " + text(pos),
                   step);
            return;
        }
        steerAlong(leg);
        return;
    }
    if (distance < (target.action == DomeWaypoint::Warp ? kArriveWarp : kArrive) && height < kArriveHeight) {
        if (mAwaitJumpLanding && target.action != DomeWaypoint::Spin && !o.playerOnGround) {
            steer({}, step);
            if (mPhaseFrames >= kRouteLimit) finish(Result::Fail, "jump destination never became grounded", step);
            return;
        }
        if (target.action != DomeWaypoint::Spin) mAwaitJumpLanding = false;
        if (target.action == DomeWaypoint::Spin) tap(Button::Spin, kTapFrames, step);
        if (target.action == DomeWaypoint::Jump || target.action == DomeWaypoint::Hop) {
            const bool kickNext = mWaypoint + 1 < points.size() && points[mWaypoint + 1].action == DomeWaypoint::Kick;
            if (kickNext && target.action == DomeWaypoint::Hop) {
                mKickSettle = 1;
                steerAlong(Vec{points[mWaypoint + 1].x - target.x, points[mWaypoint + 1].y - target.y, points[mWaypoint + 1].z - target.z});
                return;
            }
            mAwaitJumpLanding = true;
            // A held for full height, then a spin near the apex for the extra lift
            // the observatory's terrace steps (up to about 310 units) need.
            note("jump and spin at waypoint " + std::to_string(mWaypoint) + " from " + text(pos));
            tap(Button::A, kJumpHold, step);
            if (target.action == DomeWaypoint::Jump) mSpinAt = mFrame + kJumpSpinDelay;
        }
        if (target.action == DomeWaypoint::Launch || target.action == DomeWaypoint::Talk) {
            steer({}, step);
            note("route interaction at " + text(pos));
            mWarpX = pos.x; mWarpY = pos.y; mWarpZ = pos.z;
            mTalkWasActive = false;
            next(target.action == DomeWaypoint::Launch ? Phase::Launch : Phase::Talk);
            return;
        }
        if (target.action == DomeWaypoint::Warp) {
            steer({}, step);
            note("on warp pod (waypoint " + std::to_string(mWaypoint) + ") at " + text(pos));
            mWarpX = pos.x; mWarpY = pos.y; mWarpZ = pos.z;
            next(Phase::Warp);
            return;
        }
        if (target.action != DomeWaypoint::Hop) mKickRetries = 0;
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
    case Phase::BlueStar: {
        const bool shown = std::any_of(o.targets.begin(), o.targets.end(), [](const Observation::Target& target) {
            return target.id == "Dome.BlueStar" && (target.flags & kTargetSelectable);
        });
        // Some dome entrance cameras put the star outside the screen. Walk
        // toward the room centre until it is visible, just as a player does.
        const Vec delta = across(sub(Vec{0, o.playerY, 0}, position(o)), up(o));
        if (!shown && o.playerValid && !o.demoActive && !o.talkActive && mPhaseFrames > kSettle && length(delta) > 180.0f) {
            const Vec right = across(Vec{o.camXx, o.camXy, o.camXz}, up(o));
            const Vec forward = across(Vec{o.camZx, o.camZy, o.camZz}, up(o));
            if (length(right) > 1e-3f && length(forward) > 1e-3f) {
                steer(stickKeysFor(dot(delta, scale(right, 1.0f / length(right))) * mSignRight,
                                   dot(delta, scale(forward, 1.0f / length(forward))) * mSignForward), step);
            }
        } else {
            steer({}, step);
        }
        if (mPhaseFrames % 120 == 0) {
            note("dome idle at " + text(position(o)) + ", camera forward " + text(Vec{o.camZx, o.camZy, o.camZz}) +
                 ", targets " + std::to_string(o.targets.size()));
        }
        if (aimAndPress(o, "Dome.BlueStar", -1, step)) next(Phase::GalaxyMap);
        return;
    }
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
        if (aimAndPress(o, "Scenario.Star", visit->scenario, step)) {
            std::vector<int> shown;
            for (const auto& target : o.targets) {
                if (target.id == "Scenario.Star" && (target.flags & kTargetSelectable)) shown.push_back(target.index);
            }
            std::sort(shown.begin(), shown.end());
            shown.erase(std::unique(shown.begin(), shown.end()), shown.end());
            std::string list;
            for (int scenario : shown) list += (list.empty() ? "" : ",") + std::to_string(scenario);
            note("DOMES STARS " + visit->galaxy + ": " + list);
            next(Phase::Load);
        }
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
            PetariNative::EfbDump::mark("dome-ready");  // opt-in image dump (PETARI_XFB_DUMP); inert otherwise
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
        if (mConfig.dome == 7 && o.scene == "Game" && o.stage == "AstroGalaxy" && o.sceneReady) {
            closeVisit("PASS");
            ++mVisit;
            finish(Result::Pass, "Grand Finale selected through its Luma, loaded, ready and returned", step);
            return;
        }
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

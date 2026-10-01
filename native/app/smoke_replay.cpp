#include "smoke_replay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "smoke_goodegg.hpp"  // Point3, stickKeysForWorld: Mario::calcMoveDir's stick mapping

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kTapFrames = 6;
constexpr unsigned long kJumpHold = 12;
constexpr unsigned long kKickHold = 30;
constexpr unsigned long kKickRepress = 2;
constexpr unsigned long kKickSettle = 10;
constexpr unsigned long kKickGroundLimit = 20;
constexpr unsigned long kJumpSpinDelay = 16;
constexpr float kArrive = 70.0f;
constexpr float kArriveHeight = 300.0f;
constexpr float kClingStill = 0.5f;
constexpr float kKickReach = 200.0f;
constexpr float kKickReachHeight = 400.0f;
constexpr float kFarFromRoute = 1500.0f;      // off the route (a fall, a knock-back): resync
constexpr unsigned long kStuckFrames = 180;
constexpr int kRecoveries = 3;
constexpr int kResyncLimit = 12;
constexpr unsigned long kWaypointLimit = 3600;
constexpr float kMoved = 800.0f;              // a Launch or Warp carried Mario
constexpr unsigned long kCarryLimit = 1800;
constexpr unsigned long kTalkTapInterval = 45;
constexpr unsigned long kDemoPatience = 300;  // then A once a second (a player skipping text)
constexpr unsigned long kNoticeRead = 45;
constexpr unsigned long kNoticeRetap = 60;
constexpr int kNoticeTaps = 5;
constexpr unsigned long kPointingFramesToPress = 3;
constexpr unsigned long kAimLimit = 240;
constexpr unsigned long kPromptKeyRead = 45;
constexpr unsigned long kReturnLimit = 5400;
constexpr unsigned long kObservatoryReady = 120;
constexpr size_t kResyncAhead = 80;
constexpr float kHostileSpin = 250.0f;        // spin at a published hostile actor this close
constexpr unsigned long kSpinCooldown = 40;
constexpr unsigned long kHandoverReady = 60;  // playable frames before the replay takes over
constexpr size_t kStartWindow = 200;          // route points searched for the start

struct Vec {
    float x, y, z;
};
Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
Vec scale(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec across(Vec d, Vec u) { return sub(d, scale(u, dot(d, u))); }
Vec position(const Observation& o) { return {o.playerX, o.playerY, o.playerZ}; }
Vec pointOf(const DomeWaypoint& w) { return {w.x, w.y, w.z}; }
Vec up(const Observation& o) {
    const Vec g{-o.gravityX, -o.gravityY, -o.gravityZ};
    const float l = length(g);
    return l > 1e-3f ? scale(g, 1.0f / l) : Vec{0.0f, 1.0f, 0.0f};
}
std::string number(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", v);
    return text;
}
std::string text(Vec a) { return "(" + number(a.x) + ", " + number(a.y) + ", " + number(a.z) + ")"; }
const char* actionName(DomeWaypoint::Action a) {
    switch (a) {
    case DomeWaypoint::Walk: return "Walk";
    case DomeWaypoint::Warp: return "Warp";
    case DomeWaypoint::Jump: return "Jump";
    case DomeWaypoint::Launch: return "Launch";
    case DomeWaypoint::Talk: return "Talk";
    case DomeWaypoint::Hop: return "Hop";
    case DomeWaypoint::Spin: return "Spin";
    case DomeWaypoint::Kick: return "Kick";
    }
    return "?";
}
bool shown(const Observation& o, const char* id) {
    for (const Observation::Target& t : o.targets) {
        if (t.id == id && (t.flags & kTargetSelectable)) return true;
    }
    return false;
}

}  // namespace

std::vector<DomeWaypoint> parseReplayRoute(const std::string& text) {
    std::vector<DomeWaypoint> route;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        float x, y, z;
        char action[16] = {};
        if (std::sscanf(line.c_str(), "%f,%f,%f,%15s", &x, &y, &z, action) != 4) continue;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
        const std::string a = action;
        const DomeWaypoint::Action kind = a == "Warp" ? DomeWaypoint::Warp
                                          : a == "Jump" ? DomeWaypoint::Jump
                                          : a == "Hop" ? DomeWaypoint::Hop
                                          : a == "Spin" ? DomeWaypoint::Spin
                                          : a == "Launch" ? DomeWaypoint::Launch
                                          : a == "Kick" ? DomeWaypoint::Kick
                                                        : DomeWaypoint::Walk;
        route.push_back({x, y, z, kind});
    }
    return route;
}

bool replayEnabledFromEnvironment(ReplayConfig* config) {
    const char* smoke = std::getenv("PETARI_SMOKE");
    if (smoke == nullptr || std::strcmp(smoke, "replay") != 0) return false;
    const char* stage = std::getenv("PETARI_STAGE");
    const char* scenario = std::getenv("PETARI_SCENARIO");
    const char* path = std::getenv("PETARI_REPLAY_ROUTE");
    if (stage == nullptr || stage[0] == '\0' || scenario == nullptr || path == nullptr) {
        std::fputs("PETARI SMOKE: replay needs PETARI_STAGE, PETARI_SCENARIO and PETARI_REPLAY_ROUTE; not running\n", stderr);
        return false;
    }
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    config->stage = stage;
    config->scenario = std::atoi(scenario);
    config->routePath = path;
    config->route = parseReplayRoute(buffer.str());
    if (!in || config->route.size() < 2 || config->scenario < 1) {
        std::fprintf(stderr, "PETARI SMOKE: replay route %s unreadable or shorter than two points; not running\n", path);
        return false;
    }
    return true;
}

ReplayDriver::ReplayDriver(unsigned long frameLimit, const ReplayConfig& config)
    : mBoot(frameLimit + 1, Script::Reload), mConfig(config), mFrameLimit(frameLimit) {}

const char* ReplayDriver::phase() const {
    switch (mPhase) {
    case Phase::Boot: return mBoot.phase();
    case Phase::Route: return "replay: following the recorded route";
    case Phase::Launch: return "replay: using a Launch Star";
    case Phase::Warp: return "replay: waiting to be carried";
    case Phase::Return: return "replay: star collected, returning to the observatory";
    case Phase::Done: return "replay: done";
    }
    return "replay";
}

void ReplayDriver::next(Phase phase) {
    mPhase = phase;
    mPhaseFrames = 0;
}

void ReplayDriver::finish(Result result, const std::string& reason, Step& step) {
    mResult = result;
    mReason = reason;
    mPhase = Phase::Done;
    steer({}, step);
    note(std::string(result == Result::Pass ? "PASS: " : "FAIL: ") + reason);
    step.requestQuit = true;
}

void ReplayDriver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.presses.push_back({button, true});
    step.assertFocus = true;
    mReleases.push_back({mFrame + holdFrames, button});
}

void ReplayDriver::steer(const StickKeys& keys, Step& step) {
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

void ReplayDriver::steerToward(const Observation& o, float x, float y, float z, Step& step) {
    steer(stickKeysForWorld(o, Point3{x, y, z}), step);
}

void ReplayDriver::resync(const Observation& o, size_t from, size_t to, const char* why) {
    const Vec pos = position(o);
    size_t best = mWaypoint;
    float bestDistance = 1e30f;
    for (size_t i = from; i < std::min(to, mConfig.route.size()); ++i) {
        const float d = length(sub(pointOf(mConfig.route[i]), pos));
        if (d < bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    note(std::string("resync (") + why + "): route point " + std::to_string(best) + "/" + std::to_string(mConfig.route.size()) +
         ", " + number(bestDistance) + " away, from " + std::to_string(mWaypoint) + " at " + text(pos));
    mWaypoint = best;
    mBestDistance = 1e30f;
    mStuckFrames = mWaypointFrames = mKickSettle = mKickGrounded = 0;
    mAwaitLanding = false;
    ++mResyncs;
}

void ReplayDriver::advance(const char* why, const Observation& o) {
    ++mWaypoint;
    mBestDistance = 1e30f;
    mStuckFrames = mWaypointFrames = 0;
    mRecoveries = 0;
    if (mWaypoint % 100 == 0 || mWaypoint >= mConfig.route.size()) {
        note(std::string("route point ") + std::to_string(mWaypoint) + "/" + std::to_string(mConfig.route.size()) + " (" + why +
             ") at " + text(position(o)));
    }
}

Step ReplayDriver::step(const Observation& o) {
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
        step = mBoot.step(o);
        for (const std::string& line : mBoot.log()) {
            if (line.rfind("physical ", 0) != 0 && line.rfind("window focus", 0) != 0) mLog.push_back(line);
        }
        if (mBoot.result() != Result::Running) {
            mResult = mBoot.result() == Result::Blocked ? Result::Blocked : Result::Fail;
            mReason = std::string("before the replay: ") + mBoot.reason();
            mPhase = Phase::Done;
            step.requestQuit = true;
            return step;
        }
        // Hand over once Mario stands in the stage, playable, for a second: the
        // arrival (intro demo, the flight in) is the game's, as in the recording.
        const bool ready = o.scene == "Game" && o.stage == mConfig.stage && o.sceneReady && o.playerValid && o.playerOnGround &&
                           !o.playerInBind && !o.demoActive && o.pausePermitted && !o.talkActive;
        mReadyFrames = ready ? mReadyFrames + 1 : 0;
        if (o.scene == "Game" && o.stage == mConfig.stage && o.sceneReady && o.scenario != mConfig.scenario && o.scenario > 0) {
            finish(Result::Fail, "entered scenario " + std::to_string(o.scenario) + ", not the recorded " +
                                     std::to_string(mConfig.scenario),
                   step);
            return step;
        }
        if (mReadyFrames < kHandoverReady) return step;
        step.presses.clear();
        for (Button b : {Button::A, Button::B, Button::StickUp, Button::StickDown, Button::StickLeft, Button::StickRight,
                         Button::Spin}) {
            step.presses.push_back({b, false});
        }
        step.requestQuit = false;
        mHeld = {};
        mReleases.clear();
        mLastStage = o.stage;
        next(Phase::Route);
        note("replay of " + mConfig.routePath + " (" + std::to_string(mConfig.route.size()) + " points) in " + mConfig.stage +
             " scenario " + std::to_string(o.scenario) + " (requested " + std::to_string(mConfig.scenario) + ")");
        mReadyFrames = 0;
        resync(o, 0, kStartWindow, "start");
        return step;
    }

    for (const std::string& milestone : o.milestones) {
        note("milestone " + milestone);
        if (milestone == "PowerStar.Get" || milestone == "GrandStar.Get") {
            if (!mStar) {
                mStar = true;
                note("star collected at route point " + std::to_string(mWaypoint) + "/" + std::to_string(mConfig.route.size()) +
                     ", deaths " + std::to_string(mDeaths));
                steer({}, step);
                next(Phase::Return);
            }
        } else if (milestone == "InformationObserver.Close") {
            mNotice.clear();
        } else if (milestone.rfind("InformationObserver", 0) == 0) {
            mNotice = milestone;
            mNoticeFrame = mFrame;
            mNoticeTaps = 0;
        }
    }
    if (o.stage != mLastStage) {
        note("stage " + (o.stage.empty() ? std::string("(none)") : o.stage) + " scene " + o.scene);
        mLastStage = o.stage;
    }
    if (mFrame >= mFrameLimit) {
        finish(Result::Fail, "frame limit " + std::to_string(mFrameLimit) + " reached while " + phase() + " (route point " +
                                 std::to_string(mWaypoint) + "/" + std::to_string(mConfig.route.size()) + ")",
               step);
        return step;
    }
    if (interactions(o, step)) return step;

    switch (mPhase) {
    case Phase::Route: route(o, step); break;
    case Phase::Launch:
    case Phase::Warp: launchOrWarp(o, step); break;
    case Phase::Return: returnToObservatory(o, step); break;
    default: break;
    }
    return step;
}

bool ReplayDriver::interactions(const Observation& o, Step& step) {
    // Prompts: yes/no gets Yes (a person replaying the mission said yes to go on:
    // save, a Luma's offer); a key prompt gets A after reading; blocking waits.
    for (const Observation::Prompt& prompt : o.prompts) {
        note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
        if (prompt.type == 2) {
            mPrompt = "Prompt.Yes";
            mAimFrames = mPointingFrames = 0;
        } else if (prompt.type == 0) {
            mPromptKeyAt = mFrame + kPromptKeyRead;
        }
    }
    if (!mPrompt.empty()) {
        steer({}, step);
        const Observation::Target* yes = nullptr;
        for (const Observation::Target& t : o.targets) {
            if (t.id == mPrompt && (t.flags & kTargetSelectable)) yes = &t;
        }
        if (yes == nullptr) {
            if (++mAimFrames >= kAimLimit) finish(Result::Fail, "prompt answer " + mPrompt + " never shown", step);
            return true;
        }
        step.pointer = true;
        step.pointerU = yes->u;
        step.pointerV = yes->v;
        step.assertFocus = true;
        mPointingFrames = (yes->flags & kTargetPointing) ? mPointingFrames + 1 : 0;
        if (mPointingFrames >= kPointingFramesToPress) {
            note("tap A: " + mPrompt);
            tap(Button::A, kTapFrames, step);
            mPrompt.clear();
        } else if (++mAimFrames >= kAimLimit) {
            finish(Result::Fail, "the pointer never got over " + mPrompt, step);
        }
        return true;
    }
    if (mPromptKeyAt != 0 && mFrame >= mPromptKeyAt) {
        mPromptKeyAt = 0;
        note("tap A: key prompt");
        tap(Button::A, kTapFrames, step);
        return true;
    }
    if (!mNotice.empty()) {
        steer({}, step);
        if (mFrame - mNoticeFrame >= kNoticeRead && (mNoticeTaps == 0 || mFrame - mNoticeTapFrame >= kNoticeRetap)) {
            if (mNoticeTaps >= kNoticeTaps) {
                finish(Result::Fail, "information notice " + mNotice + " did not close after " + std::to_string(kNoticeTaps) + " A presses",
                       step);
                return true;
            }
            ++mNoticeTaps;
            mNoticeTapFrame = mFrame;
            note("tap A: information notice " + mNotice);
            tap(Button::A, kTapFrames, step);
        }
        return true;
    }
    if (shown(o, "Talk.Advance")) {
        steer({}, step);
        if (mFrame >= mTalkTapAt) {
            mTalkTapAt = mFrame + kTalkTapInterval;
            note("tap A: talk page");
            tap(Button::A, kTapFrames, step);
        }
        return true;
    }
    if (o.talkActive) {
        steer({}, step);
        return true;
    }
    if (o.demoActive) {
        steer({}, step);
        if (++mDemoFrames >= kDemoPatience && (mDemoFrames - kDemoPatience) % 60 == 0) {
            if (mDemoFrames == kDemoPatience) note("tap A: advancing a long demo");
            tap(Button::A, kTapFrames, step);
        }
        return true;
    }
    mDemoFrames = 0;
    if (o.playerValid && o.playerLife >= 0 && o.playerLife != mLastLife) {
        if (mLastLife >= 0) {
            // With the nearest published hostile, so a hit can be attributed.
            const Observation::Actor* nearest = nullptr;
            float d = 1e30f;
            for (const Observation::Actor& a : o.actors) {
                const float l = length(sub(Vec{a.x, a.y, a.z}, position(o)));
                if ((a.flags & kActorHostile) && l < d) {
                    d = l;
                    nearest = &a;
                }
            }
            note("life " + std::to_string(mLastLife) + " -> " + std::to_string(o.playerLife) + " at " + text(position(o)) +
                 (nearest != nullptr ? ", nearest hostile " + nearest->kind + " " + number(d) + " away" : ", no hostile published"));
        }
        mLastLife = o.playerLife;
    }
    if (o.playerValid && o.playerDead) {
        steer({}, step);
        if (!mWasDead) {
            mWasDead = true;
            ++mDeaths;
            note("Mario died at " + text(position(o)) + " (death " + std::to_string(mDeaths) + ")");
        }
        return true;
    }
    return false;
}

void ReplayDriver::launchOrWarp(const Observation& o, Step& step) {
    if (!o.playerValid) {
        steer({}, step);
        return;
    }
    const Vec pos = position(o);
    const Vec u = up(o);
    const float moved = length(sub(pos, Vec{mFromX, mFromY, mFromZ}));
    if (moved >= kMoved && o.playerOnGround && !o.playerInBind) {
        note(std::string(mPhase == Phase::Launch ? "launch" : "carried") + " landed " + number(moved) + " away at " + text(pos));
        advance(mPhase == Phase::Launch ? "launch" : "warp", o);
        next(Phase::Route);
        return;
    }
    if (mPhase == Phase::Launch && moved < kMoved) {
        const DomeWaypoint& star = mConfig.route[mWaypoint];
        const Vec toStar = across(sub(pointOf(star), pos), u);
        if (!o.playerInBind && length(toStar) > 15.0f) {
            steerToward(o, toStar.x, toStar.y, toStar.z, step);
        } else {
            steer({}, step);
        }
        if (mPhaseFrames % 60 == 1 && o.playerOnGround && !o.playerInBind) tap(Button::A, kJumpHold, step);
        if (mPhaseFrames % 30 == 16) tap(Button::Spin, kTapFrames, step);
    } else {
        steer({}, step);
    }
    if (mPhaseFrames >= kCarryLimit) {
        finish(Result::Fail, std::string(mPhase == Phase::Launch ? "the Launch Star at route point " : "the carry at route point ") +
                                 std::to_string(mWaypoint) + " " + text(pointOf(mConfig.route[mWaypoint])) +
                                 " did not move Mario (at " + text(pos) + ")",
               step);
    }
}

void ReplayDriver::route(const Observation& o, Step& step) {
    if (!o.playerValid) {
        steer({}, step);
        return;
    }
    const Vec pos = position(o);
    const Vec u = up(o);
    if (mWasDead) {
        // Respawned: the checkpoint can be anywhere earlier on the route.
        mWasDead = false;
        resync(o, 0, mConfig.route.size(), "respawn");
    }
    if (mWaypoint >= mConfig.route.size()) {
        // The route ends where the recording's star grab began: the star is near.
        steer({}, step);
        if (mPhaseFrames % 120 == 0) note("route finished; waiting for the star at " + text(pos));
        if (++mWaypointFrames >= kWaypointLimit) finish(Result::Fail, "route finished without PowerStar.Get at " + text(pos), step);
        return;
    }
    if (mSpinAt != 0 && mFrame >= mSpinAt) {
        tap(Button::Spin, kTapFrames, step);
        mSpinAt = 0;
    }
    if (mKickAt != 0 && mFrame >= mKickAt) {
        tap(Button::A, kKickHold, step);
        mKickAt = 0;
    }
    for (const Observation::Actor& a : o.actors) {
        if ((a.flags & kActorHostile) && length(sub(Vec{a.x, a.y, a.z}, pos)) < kHostileSpin && mFrame - mLastSpin >= kSpinCooldown) {
            mLastSpin = mFrame;
            note("spin at " + a.kind + " " + number(length(sub(Vec{a.x, a.y, a.z}, pos))) + " away");
            tap(Button::Spin, kTapFrames, step);
            break;
        }
    }
    const std::vector<DomeWaypoint>& points = mConfig.route;
    const DomeWaypoint& target = points[mWaypoint];
    const Vec last{mLastX, mLastY, mLastZ};
    mLastX = pos.x; mLastY = pos.y; mLastZ = pos.z;
    ++mWaypointFrames;

    if (length(sub(pointOf(target), pos)) > kFarFromRoute && target.action != DomeWaypoint::Kick && !mAwaitLanding &&
        o.playerOnGround) {
        if (mResyncs >= kResyncLimit) {
            finish(Result::Fail, "left the route " + std::to_string(mResyncs) + " times; last at " + text(pos), step);
            return;
        }
        resync(o, mWaypoint > 10 ? mWaypoint - 10 : 0, mWaypoint + kResyncAhead, "off the route");
        return;
    }

    const auto steerAlong = [&](Vec leg) { steerToward(o, leg.x, leg.y, leg.z, step); };
    if (mKickSettle > 0) {
        const DomeWaypoint& wall = points[mWaypoint + 1];
        if (++mKickSettle <= kKickSettle) {
            steerAlong(sub(pointOf(wall), pointOf(target)));
            return;
        }
        mKickSettle = 0;
        mAwaitLanding = true;
        tap(Button::A, kKickHold, step);
        advance("hop into a wall", o);
        return;
    }
    if (target.action == DomeWaypoint::Kick) {
        const DomeWaypoint& from = points[mWaypoint > 0 ? mWaypoint - 1 : 0];
        const Vec contact = sub(pointOf(target), pos);
        const bool clinging = !o.playerOnGround && !o.playerInBind && length(sub(pos, last)) < kClingStill;
        if (clinging && length(across(contact, u)) < kKickReach && std::fabs(dot(contact, u)) < kKickReachHeight) {
            note("wall kick at route point " + std::to_string(mWaypoint) + " from " + text(pos));
            const auto held = std::find_if(mReleases.begin(), mReleases.end(), [](const Release& r) { return r.button == Button::A; });
            if (held != mReleases.end()) {
                step.presses.push_back({Button::A, false});
                mReleases.erase(held);
                mKickAt = mFrame + kKickRepress;
            } else {
                tap(Button::A, kKickHold, step);
            }
            advance("wall kick", o);
            mKickGrounded = 0;
            return;
        }
        mKickGrounded = o.playerOnGround ? mKickGrounded + 1 : 0;
        size_t landing = mWaypoint;
        while (landing < points.size() && points[landing].action == DomeWaypoint::Kick) ++landing;
        if (mKickGrounded >= 2 && landing < points.size() && std::fabs(dot(sub(pointOf(points[landing]), pos), u)) < 60.0f) {
            note("wall-kick chain reached its landing level at " + text(pos));
            resync(o, landing, landing + 15, "landing level");
            return;
        }
        if (mKickGrounded >= kKickGroundLimit) {
            size_t hop = mWaypoint;
            while (hop > 0 && points[hop].action != DomeWaypoint::Hop) --hop;
            mKickGrounded = 0;
            if (points[hop].action != DomeWaypoint::Hop || ++mKickRetries > kRecoveries) {
                finish(Result::Fail, "wall kick at route point " + std::to_string(mWaypoint) + " " + text(pointOf(target)) +
                                         " not reached, at " + text(pos),
                       step);
                return;
            }
            note("wall kick at route point " + std::to_string(mWaypoint) + " fell short; retrying from " + std::to_string(hop));
            mWaypoint = hop;
            mAwaitLanding = false;
            mWaypointFrames = 0;
            return;
        }
        steerAlong(sub(pointOf(target), pointOf(from)));
        return;
    }

    const Vec delta = across(sub(pointOf(target), pos), u);
    const float distance = length(delta);
    const float height = std::fabs(dot(sub(pointOf(target), pos), u));
    if (distance < kArrive && height < kArriveHeight) {
        if (mAwaitLanding && target.action != DomeWaypoint::Spin && !o.playerOnGround) {
            steer({}, step);
            return;
        }
        if (target.action != DomeWaypoint::Spin) mAwaitLanding = false;
        switch (target.action) {
        case DomeWaypoint::Spin:
            tap(Button::Spin, kTapFrames, step);
            break;
        case DomeWaypoint::Hop:
        case DomeWaypoint::Jump: {
            const bool kickNext = mWaypoint + 1 < points.size() && points[mWaypoint + 1].action == DomeWaypoint::Kick;
            if (kickNext && target.action == DomeWaypoint::Hop) {
                mKickSettle = 1;
                steerAlong(sub(pointOf(points[mWaypoint + 1]), pointOf(target)));
                return;
            }
            mAwaitLanding = true;
            tap(Button::A, kJumpHold, step);
            if (target.action == DomeWaypoint::Jump) mSpinAt = mFrame + kJumpSpinDelay;
            if (target.action == DomeWaypoint::Hop) mKickRetries = 0;
            break;
        }
        case DomeWaypoint::Launch:
        case DomeWaypoint::Warp:
            steer({}, step);
            mFromX = pos.x; mFromY = pos.y; mFromZ = pos.z;
            note(std::string(target.action == DomeWaypoint::Launch ? "launch star" : "carry") + " at route point " +
                 std::to_string(mWaypoint) + " " + text(pos));
            next(target.action == DomeWaypoint::Launch ? Phase::Launch : Phase::Warp);
            return;
        default:
            mKickRetries = 0;
            break;
        }
        advance(actionName(target.action), o);
        return;
    }
    if (mWaypointFrames >= kWaypointLimit) {
        finish(Result::Fail, "route point " + std::to_string(mWaypoint) + " " + text(pointOf(target)) + " not reached within " +
                                 std::to_string(kWaypointLimit) + " frames, at " + text(pos),
               step);
        return;
    }
    if (distance < mBestDistance - 30.0f) {
        mBestDistance = distance;
        mStuckFrames = 0;
    } else if (++mStuckFrames >= kStuckFrames) {
        mStuckFrames = 0;
        if (++mRecoveries > kRecoveries) {
            if (mResyncs >= kResyncLimit) {
                finish(Result::Fail, "stuck before route point " + std::to_string(mWaypoint) + " " + text(pointOf(target)) + " at " +
                                         text(pos),
                       step);
                return;
            }
            mRecoveries = 0;
            resync(o, mWaypoint > 10 ? mWaypoint - 10 : 0, mWaypoint + kResyncAhead, "stuck");
            return;
        }
        note("stuck before route point " + std::to_string(mWaypoint) + " at " + text(pos) + "; jump");
        tap(Button::A, kJumpHold, step);
    }
    steerAlong(delta);
}

void ReplayDriver::returnToObservatory(const Observation& o, Step& step) {
    steer({}, step);
    const bool observatory = o.stage == "AstroGalaxy" || o.stage == "AstroDome";
    if (!observatory && !o.stage.empty() && o.stage != mConfig.stage) {
        finish(Result::Fail, "after the star the game went to " + o.stage + ", not the observatory", step);
        return;
    }
    const bool ready = observatory && o.scene == "Game" && o.sceneReady && o.playerValid && !o.demoActive && o.pausePermitted &&
                       !o.talkActive && !o.saveSequence;
    mReadyFrames = ready ? mReadyFrames + 1 : 0;
    if (mReadyFrames >= kObservatoryReady) {
        finish(Result::Pass, "star collected in " + mConfig.stage + " scenario " + std::to_string(mConfig.scenario) +
                                 " following the recorded route (deaths " + std::to_string(mDeaths) + ", resyncs " +
                                 std::to_string(mResyncs) + "), then back in " + o.stage + " and playable",
               step);
        return;
    }
    if (mPhaseFrames >= kReturnLimit) {
        finish(Result::Fail, "star collected, but no playable observatory within " + std::to_string(kReturnLimit) +
                                 " frames (stage " + o.stage + ", scene " + o.scene + ")",
               step);
    }
}

}  // namespace PetariNative::App::Smoke

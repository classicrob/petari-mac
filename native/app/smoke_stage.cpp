// The stage smoke script (smoke_stage.hpp). No SDK or Aurora headers: the
// frame seam feeds it observations and applies its presses.

#include "smoke_stage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace PetariNative::App::Smoke {

namespace {

constexpr unsigned long kTapFrames = 6;
constexpr unsigned long kLoadLimit = 5400;
constexpr unsigned long kReadyFrames = 60;
constexpr unsigned long kReadyLimit = 5400;
constexpr unsigned long kTalkTapInterval = 45;
constexpr unsigned long kTalkLimit = 1800;
constexpr unsigned long kIdleFrames = 120;
constexpr float kIdleDrift = 5.0f;
constexpr unsigned long kSettle = 20;
constexpr unsigned long kWalkHold = 30;
constexpr unsigned long kWalkAfter = 15;
constexpr float kWalkMinimum = 40.0f;
constexpr unsigned long kJumpLeaveLimit = 30;
constexpr unsigned long kJumpLandLimit = 240;
constexpr float kJumpMinimumRise = 60.0f;
constexpr unsigned long kSpinWait = 60;
constexpr unsigned long kCameraWait = 45;
constexpr float kCameraMinimumDegrees = 5.0f;
constexpr unsigned long kPauseHold = 18;  // the game opens the menu at a 12-frame hold
constexpr unsigned long kPausePermitLimit = 600;
constexpr unsigned long kPauseMenuLimit = 300;
constexpr unsigned long kPauseMenuSettle = 90;
constexpr unsigned long kPausedHold = 45;
constexpr float kPausedDrift = 1.0f;

struct Vec {
    float x, y, z;
};
Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec a) { return std::sqrt(dot(a, a)); }
Vec position(const Observation& o) { return {o.playerX, o.playerY, o.playerZ}; }
// Up: against the gravity field at Mario (the stage's -y when it is unknown).
Vec up(const Observation& o) {
    const Vec g{-o.gravityX, -o.gravityY, -o.gravityZ};
    const float l = length(g);
    return l > 1e-3f ? Vec{g.x / l, g.y / l, g.z / l} : Vec{0.0f, 1.0f, 0.0f};
}
// The part of d across the ground (perpendicular to up).
float acrossGround(Vec d, Vec u) {
    const float along = dot(d, u);
    return length({d.x - along * u.x, d.y - along * u.y, d.z - along * u.z});
}
std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}
std::string text(Vec a) { return "(" + number(a.x) + ", " + number(a.y) + ", " + number(a.z) + ")"; }

struct Walk {
    Button button;
    const char* name;
};
constexpr Walk kWalks[] = {{Button::StickUp, "up"}, {Button::StickDown, "down"},
                           {Button::StickRight, "right"}, {Button::StickLeft, "left"}};
constexpr Walk kCameras[] = {{Button::CameraLeft, "left"}, {Button::CameraRight, "right"}};

unsigned long environmentNumber(const char* name, unsigned long fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end != nullptr && *end == '\0' ? parsed : fallback;
}

}  // namespace

bool stageEnabledFromEnvironment(StageConfig* config) {
    const char* smoke = std::getenv("PETARI_SMOKE");
    if (smoke == nullptr || std::strcmp(smoke, "stage") != 0) return false;
    const char* stage = std::getenv("PETARI_STAGE");
    const char* scenario = std::getenv("PETARI_SCENARIO");
    const long number = scenario != nullptr ? std::strtol(scenario, nullptr, 10) : 0;
    if (stage == nullptr || stage[0] == '\0' || number < 1) {
        std::fputs("PETARI SMOKE: script stage needs PETARI_STAGE and PETARI_SCENARIO; not running\n", stderr);
        return false;
    }
    config->stage = stage;
    config->scenario = static_cast<int>(number);
    config->tailFrames = environmentNumber("PETARI_STAGE_IDLE_FRAMES", 600);
    return true;
}

StageDriver::StageDriver(unsigned long frameLimit, const StageConfig& config)
    : mBoot(frameLimit, Script::Reload), mConfig(config), mFrameLimit(frameLimit) {}

const char* StageDriver::phase() const {
    switch (mPhase) {
    case Phase::Boot: return mBoot.phase();
    case Phase::Load: return "stage: loading";
    case Phase::Ready: return "stage: waiting for gameplay";
    case Phase::Idle: return "stage: idle";
    case Phase::Walk: return "stage: walking";
    case Phase::Jump: return "stage: jumping";
    case Phase::Spin: return "stage: spinning";
    case Phase::Camera: return "stage: rotating the camera";
    case Phase::PauseOpen: return "stage: opening the pause menu";
    case Phase::Paused: return "stage: paused";
    case Phase::PauseClose: return "stage: closing the pause menu";
    case Phase::Tail: return "stage: idling after the checks";
    case Phase::Done: return "done";
    }
    return "?";
}

void StageDriver::next(Phase phase) {
    mPhase = phase;
    mPhaseFrames = 0;
    mStep = 0;
}

void StageDriver::tap(Button button, unsigned long holdFrames, Step& step) {
    step.presses.push_back({button, true});
    step.assertFocus = true;
    mReleases.push_back({mFrame + holdFrames, button});
}

void StageDriver::check(const std::string& name, const char* status, const std::string& detail) {
    mChecks.push_back({name, status, detail});
    note("stage check " + name + ": " + status + " " + detail);
}

void StageDriver::finish(Result result, const std::string& reason, Step& step) {
    for (const Release& release : mReleases) {
        step.presses.push_back({release.button, false});
    }
    mReleases.clear();
    mResult = result;
    mReason = "synthetic entry " + mConfig.stage + " scenario " + std::to_string(mConfig.scenario) + ": " + reason;
    std::string warned;
    for (const Check& c : mChecks) {
        if (std::strcmp(c.status, "ok") != 0 && std::strcmp(c.status, "done") != 0) {
            warned += (warned.empty() ? "" : ", ") + c.name + " " + c.status;
        }
    }
    note("stage summary: stage " + mConfig.stage + " scenario " + std::to_string(mConfig.scenario) + ", load start " +
         std::to_string(mLoadStartFrame) + ", ready " + std::to_string(mReadyFrame) + ", end " +
         std::to_string(mFrame) + ", checks " + std::to_string(mChecks.size()) +
         (warned.empty() ? ", no warnings" : ", not ok: " + warned));
    mPhase = Phase::Done;
    step.requestQuit = true;
    note(std::string(resultName(mResult)) + ": " + mReason);
}

Step StageDriver::step(const Observation& o) {
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
        // The boot script's presses end here: nothing it held stays held.
        step.presses.push_back({Button::A, false});
        step.presses.push_back({Button::B, false});
        mLastScene = o.scene;
        mLastStage = o.stage;
        mPhysicalBase = mPhysicalSeen = o.physical.gameplay;
        for (const std::string& milestone : o.milestones) {
            note("milestone " + milestone);
            mSeen.push_back(milestone);
        }
        mLoadStartFrame = mFrame;
        note("synthetic stage entry: scene Game stage " + o.stage + " (requested " + mConfig.stage + " scenario " +
             std::to_string(mConfig.scenario) + ")");
        if (o.stage != mConfig.stage) {
            finish(Result::Fail, "the stage fixture entry was not applied: the file loaded into " + o.stage, step);
            return step;
        }
        next(Phase::Load);
        return step;
    }

    // What changed.
    if (o.scene != mLastScene || o.stage != mLastStage) {
        note("scene " + (o.scene.empty() ? std::string("(none)") : o.scene) + (o.stage.empty() ? "" : " stage " + o.stage) +
             " scenario " + std::to_string(o.scenario));
        mLastScene = o.scene;
        mLastStage = o.stage;
    }
    for (const std::string& milestone : o.milestones) {
        note("milestone " + milestone);
        mSeen.push_back(milestone);
    }
    for (const Observation::Prompt& prompt : o.prompts) {
        note("prompt " + prompt.messageId + " type " + std::to_string(prompt.type));
        finish(Result::Blocked, "system prompt " + prompt.messageId + " (type " + std::to_string(prompt.type) + ")", step);
        return step;
    }
    if (o.physical.gameplay > mPhysicalSeen) {
        mPhysicalSeen = o.physical.gameplay;
        note("physical input while " + std::string(phase()) + ": " + std::string(o.physical.last, strnlen(o.physical.last, sizeof(o.physical.last))));
    }
    if (mFrame >= mFrameLimit) {
        finish(Result::Fail, "frame limit " + std::to_string(mFrameLimit) + " reached while " + phase(), step);
        return step;
    }
    if (o.scene != "Game" || o.stage != mConfig.stage) {
        finish(Result::Fail, "left the stage while " + std::string(phase()) + ": now scene " + o.scene + " stage " + o.stage,
               step);
        return step;
    }

    if (mPhase == Phase::Load) {
        if (o.sceneReady) {
            if (o.selectedScenario != mConfig.scenario) {
                finish(Result::Fail, "scenario mismatch: selected " + std::to_string(o.selectedScenario) + " (placed " +
                                         std::to_string(o.scenario) + ")",
                       step);
                return step;
            }
            note("stage loaded: " + o.stage + " scenario " + std::to_string(o.selectedScenario) + " (placed " +
                 std::to_string(o.scenario) + ") after " + std::to_string(mPhaseFrames) + " frames");
            next(Phase::Ready);
            mReadyFrames = 0;
        } else if (mPhaseFrames >= kLoadLimit) {
            finish(Result::Fail, "not ready: the stage did not finish loading within " + std::to_string(kLoadLimit) + " frames",
                   step);
        }
        return step;
    }

    if (o.playerValid && o.playerDead) {
        finish(Result::Fail, "died: Mario died while " + std::string(phase()) + " at " + text(position(o)), step);
        return step;
    }
    // Talks: tap A through them, as a player reads; the exercise waits and
    // restarts its current step afterwards.
    bool talkTarget = false;
    for (const Observation::Target& target : o.targets) {
        if (target.id == "Talk.Advance" && (target.flags & kTargetSelectable)) talkTarget = true;
    }
    if (o.talkActive || talkTarget) {
        if (++mTalkFrames == 1) note(std::string("talk open while ") + phase() + "; tapping A through it");
        if (mTalkFrames > kTalkLimit) {
            finish(Result::Fail, "not ready: a talk stayed open for " + std::to_string(kTalkLimit) + " frames", step);
            return step;
        }
        if (mFrame >= mTalkTapAt) {
            tap(Button::A, kTapFrames, step);
            mTalkTapAt = mFrame + kTalkTapInterval;
        }
        mReadyFrames = 0;
        if (mPhase != Phase::Ready) mPhaseFrames = 0;
        return step;
    }
    if (mTalkFrames > 0) {
        note("talk closed after " + std::to_string(mTalkFrames) + " frames");
        mTalkFrames = 0;
    }

    if (mPhase == Phase::Ready) {
        const bool ready = o.sceneReady && o.playerValid && !o.demoActive && o.pausePermitted;
        mReadyFrames = ready ? mReadyFrames + 1 : 0;
        if (mReadyFrames >= kReadyFrames) {
            mReadyFrame = mFrame;
            note("stage ready: frame " + std::to_string(mFrame) + ", Mario at " + text(position(o)) +
                 (o.playerOnGround ? ", on the ground" : ", in the air"));
            next(Phase::Idle);
        } else if (mPhaseFrames >= kReadyLimit) {
            finish(Result::Fail, std::string("not ready: gameplay never became ready (player ") +
                                     (o.playerValid ? "present" : "missing") + ", demo " +
                                     (o.demoActive ? "active" : "idle") + ", pause " +
                                     (o.pausePermitted ? "permitted" : "not permitted") + ")",
                   step);
        }
        return step;
    }
    if (!o.playerValid) {
        finish(Result::Fail, std::string("the player vanished while ") + phase(), step);
        return step;
    }
    exercise(o, step);
    if (mResult == Result::Pass && o.physical.gameplay > mPhysicalBase) {
        mResult = Result::Assisted;
        mReason += "; ASSISTED: " + std::to_string(o.physical.gameplay - mPhysicalBase) + " physical gameplay input(s)";
        note("ASSISTED: physical gameplay input during the stage checks");
    }
    return step;
}

void StageDriver::exercise(const Observation& o, Step& step) {
    const Vec pos = position(o);
    const Vec start{mStartX, mStartY, mStartZ};
    auto setStart = [&] {
        mStartX = pos.x;
        mStartY = pos.y;
        mStartZ = pos.z;
    };
    switch (mPhase) {
    case Phase::Idle:
        if (mPhaseFrames == 1) setStart();
        if (mPhaseFrames >= kIdleFrames) {
            const float drift = length(sub(pos, start));
            check("idle", drift <= kIdleDrift && o.playerOnGround ? "ok" : "warn",
                  "drift " + number(drift) + (o.playerOnGround ? ", on the ground" : ", not on the ground"));
            next(Phase::Walk);
        }
        break;
    case Phase::Walk: {
        const Walk& walk = kWalks[mStep];
        if (mPhaseFrames == kSettle) {
            setStart();
            tap(walk.button, kWalkHold, step);
        } else if (mPhaseFrames == kSettle + kWalkHold + kWalkAfter) {
            const float moved = acrossGround(sub(pos, start), up(o));
            check(std::string("walk_") + walk.name, moved >= kWalkMinimum ? "ok" : "warn",
                  number(moved) + " units across the ground, " + text(start) + " -> " + text(pos));
            if (moved >= kWalkMinimum) ++mWalksOk;
            mPhaseFrames = 0;
            if (++mStep == 4) next(Phase::Jump);
        }
        break;
    }
    case Phase::Jump:
        if (mPhaseFrames == kSettle) {
            setStart();
            mLeftGround = false;
            mMaxRise = 0.0f;
            tap(Button::A, kTapFrames, step);
        } else if (mPhaseFrames > kSettle) {
            const unsigned long since = mPhaseFrames - kSettle;
            mMaxRise = std::max(mMaxRise, dot(sub(pos, start), up(o)));
            if (!o.playerOnGround) mLeftGround = true;
            if (!mLeftGround && since > kJumpLeaveLimit) {
                check("jump", "warn", "did not leave the ground within " + std::to_string(kJumpLeaveLimit) + " frames");
                next(Phase::Spin);
            } else if (mLeftGround && o.playerOnGround) {
                check("jump", mMaxRise >= kJumpMinimumRise ? "ok" : "warn",
                      "rose " + number(mMaxRise) + ", landed after " + std::to_string(since) + " frames");
                next(Phase::Spin);
            } else if (since > kJumpLandLimit) {
                check("jump", "warn", "no landing within " + std::to_string(kJumpLandLimit) + " frames (rose " +
                                          number(mMaxRise) + ")");
                next(Phase::Spin);
            }
        }
        break;
    case Phase::Spin:
        if (mPhaseFrames == kSettle) {
            tap(Button::Spin, kTapFrames, step);
        } else if (mPhaseFrames == kSettle + kSpinWait) {
            check("spin", "done", "shake pressed (no spin state is observed)");
            next(Phase::Camera);
        }
        break;
    case Phase::Camera: {
        const Walk& camera = kCameras[mStep];
        const bool left = camera.button == Button::CameraLeft;
        if (mPhaseFrames > kSettle && !mCamTriggerSeen && (left ? o.padLeftTrigger : o.padRightTrigger)) {
            mCamTriggerSeen = true;
            mCamRoundAllowed = left ? o.camRoundLeft : o.camRoundRight;
        }
        if (mPhaseFrames == kSettle) {
            mCamZx = o.camZx;
            mCamZy = o.camZy;
            mCamZz = o.camZz;
            mCamTriggerSeen = false;
            mCamRoundAllowed = false;
            tap(camera.button, kTapFrames, step);
        } else if (mPhaseFrames == kSettle + kCameraWait) {
            const Vec before{mCamZx, mCamZy, mCamZz};
            const Vec after{o.camZx, o.camZy, o.camZz};
            const float denominator = length(before) * length(after);
            const float cosine = denominator > 1e-6f ? std::clamp(dot(before, after) / denominator, -1.0f, 1.0f) : 1.0f;
            const float degrees = std::acos(cosine) * 180.0f / 3.14159265f;
            // A view that did not turn is a warning either way; the detail
            // says whether the game saw the press and whether the area's
            // camera allows rotation (where it does not, the game answers with
            // its "can't" sound, as on the Wii).
            std::string why;
            if (degrees < kCameraMinimumDegrees) {
                if (!mCamTriggerSeen) {
                    why = "; INPUT: the game never saw the D-pad press";
                } else if (!mCamRoundAllowed) {
                    why = "; fixed camera: the area's camera does not allow rotation";
                } else {
                    why = "; CAMERA: rotation allowed but the view did not turn";
                }
            }
            check(std::string("camera_") + camera.name, degrees >= kCameraMinimumDegrees ? "ok" : "warn",
                  "view turned " + number(degrees) + " degrees" + why);
            mPhaseFrames = 0;
            if (++mStep == 2) next(Phase::PauseOpen);
        }
        break;
    }
    case Phase::PauseOpen:
        if (!mPausePressed) {
            if (mPhaseFrames >= kSettle && o.pausePermitted) {
                mPauseOpenCount = static_cast<unsigned long>(std::count(mSeen.begin(), mSeen.end(), "PauseMenu.Open"));
                tap(Button::Plus, kPauseHold, step);
                mPausePressed = true;
                mPhaseFrames = 0;
            } else if (mPhaseFrames >= kPausePermitLimit) {
                finish(Result::Fail, "pausing was not permitted for " + std::to_string(kPausePermitLimit) + " frames", step);
            }
        } else if (static_cast<unsigned long>(std::count(mSeen.begin(), mSeen.end(), "PauseMenu.Open")) > mPauseOpenCount) {
            check("pause_open", "ok", "PauseMenu.Open " + std::to_string(mPhaseFrames) + " frames after the hold");
            next(Phase::Paused);
        } else if (mPhaseFrames >= kPauseMenuLimit) {
            finish(Result::Fail, "no PauseMenu.Open within " + std::to_string(kPauseMenuLimit) + " frames of holding Plus",
                   step);
        }
        break;
    case Phase::Paused:
        if (mPhaseFrames == kPauseMenuSettle) {
            setStart();
            tap(Button::StickUp, kPausedHold, step);
        } else if (mPhaseFrames == kPauseMenuSettle + kPausedHold + kSettle) {
            const float drift = length(sub(pos, start));
            if (drift > kPausedDrift) {
                finish(Result::Fail, "Mario moved " + number(drift) + " units while the game was paused", step);
                break;
            }
            check("paused_still", "ok", "drift " + number(drift) + " with the stick held while paused");
            mPauseCloseCount = static_cast<unsigned long>(std::count(mSeen.begin(), mSeen.end(), "PauseMenu.Close"));
            tap(Button::Plus, kTapFrames, step);
            next(Phase::PauseClose);
        }
        break;
    case Phase::PauseClose:
        if (static_cast<unsigned long>(std::count(mSeen.begin(), mSeen.end(), "PauseMenu.Close")) > mPauseCloseCount) {
            check("pause_close", "ok", "PauseMenu.Close " + std::to_string(mPhaseFrames) + " frames after Plus");
            next(Phase::Tail);
        } else if (mPhaseFrames >= kPauseMenuLimit) {
            finish(Result::Fail, "no PauseMenu.Close within " + std::to_string(kPauseMenuLimit) + " frames of Plus", step);
        }
        break;
    case Phase::Tail:
        if (mPhaseFrames >= mConfig.tailFrames) {
            finish(Result::Pass,
                   "loaded, ready, pause menu opened and closed, " + std::to_string(mConfig.tailFrames) +
                       " idle frames after the checks; walks responsive " + std::to_string(mWalksOk) + "/4",
                   step);
        }
        break;
    default:
        break;
    }
}

}  // namespace PetariNative::App::Smoke

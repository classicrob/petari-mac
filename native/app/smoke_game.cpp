// Observation for the smoke run (smoke.hpp): read-only queries of game and VI
// state. SDK side: no Aurora or SDL headers here.

#include "Game/System/GameDataFunction.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/GameScene.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"

#include <petari/milestone.hpp>
#include <petari/platform/vi.hpp>

#include "smoke.hpp"
#include "actor_observe_store.hpp"
#include "ui_observe_store.hpp"

namespace PetariNative::App::Smoke {

namespace {
unsigned long gMilestonesRead = 0;
}

Observation observeGame(bool wantPlayer) {
    Observation observation;

    const PetariNative::Platform::VI::DisplayState display = PetariNative::Platform::VI::displayState();
    observation.videoConfigured = display.configured;
    observation.videoBlack = display.black;

    const unsigned long count = petari_milestone_count();
    for (; gMilestonesRead < count; ++gMilestonesRead) {
        const char* name = petari_milestone_at(gMilestonesRead);
        observation.milestones.push_back(name != nullptr ? name : "(milestone history overflowed)");
    }

    std::vector<UiObserve::Target> targets;
    std::vector<UiObserve::Prompt> prompts;
    UiObserve::take(&targets, &prompts);
    for (const UiObserve::Target& target : targets) {
        observation.targets.push_back({target.id, target.index, target.u, target.v, target.flags});
    }
    for (const UiObserve::Prompt& prompt : prompts) {
        observation.prompts.push_back({prompt.messageId, prompt.type});
    }
    // Taken every frame, so the fixed store never fills with stale frames.
    std::vector<ActorObserve::Actor> actors;
    ActorObserve::take(&actors);
    for (const ActorObserve::Actor& actor : actors) {
        observation.actors.push_back({actor.kind, actor.x, actor.y, actor.z, actor.dx, actor.dy, actor.dz, actor.state, actor.flags});
    }

    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    if (pGameSystem == nullptr || pGameSystem->mSceneController == nullptr) {
        return observation;
    }
    // Construction and deletion windows (audited):
    // - The scene is created and initialised on the async executor thread
    //   (GameSystemSceneController::initializeScene). A seam can fall between
    //   createScene and Scene::init, when the scene exists but its nerve does
    //   not. SceneInitializeState_End is set on this thread only after that
    //   initialisation ended, and is reset by requestChangeScene before the
    //   scene is destroyed, so scene objects are queried only in End.
    // - The scene/stage names are values in the controller: always readable.
    // - The save-data sequence is created with its nerve in
    //   GameSequenceDirector's constructor (GameSystem::init, before the first
    //   frame) and never deleted.
    const GameSystemSceneController* pController = pGameSystem->mSceneController;
    observation.scene = pController->mCurrSceneControlInfo.mScene;
    observation.stage = pController->mCurrSceneControlInfo.mStage;
    observation.scenario = pController->mCurrSceneControlInfo.mScenarioNo;
    observation.selectedScenario = pController->mCurrSceneControlInfo.mSelectedScenarioNo;
    observation.sceneReady = pController->isSceneInitializeState(SceneInitializeState_End);
    observation.strap = observation.sceneReady && GameSystemFunction::isDisplayStrapRemineder();
    if (pGameSystem->mSequenceDirector != nullptr) {
        observation.saveSequence = GameSequenceFunction::isActiveSaveDataHandleSequence();
    }
    // Mario: only when asked (after Prologue.GameStart), in a ready Game scene,
    // through the scene's MarioHolder when it and its actor exist.
    if (wantPlayer && observation.sceneReady && observation.scene == "Game" && MR::isExistSceneObj(SceneObj_MarioHolder)) {
        const MarioHolder* pHolder = MR::getSceneObj< MarioHolder >(SceneObj_MarioHolder);
        const MarioActor* pMario = pHolder != nullptr ? pHolder->getMarioActor() : nullptr;
        if (pMario != nullptr) {
            observation.playerValid = true;
            observation.playerX = pMario->mPosition.x;
            observation.playerY = pMario->mPosition.y;
            observation.playerZ = pMario->mPosition.z;
            // The MR:: player queries go through the same MarioHolder actor.
            observation.playerOnGround = MR::isOnGroundPlayer();
            // The gravity FIELD at Mario (Mario::mAirGravityVec, from the gravity
            // system). MR::getPlayerGravity() is contact-relative instead: on
            // the ground it is the negated ground normal, so it tilts on slopes.
            // No Mario core: the gravity stays zero, which the story route
            // reports as "no gravity direction" rather than guessing.
            observation.gravityX = observation.gravityY = observation.gravityZ = 0.0f;
            if (const Mario* pCore = pMario->getMario()) {
                const TVec3f& gravity = pCore->getAirGravityVec();
                observation.gravityX = gravity.x;
                observation.gravityY = gravity.y;
                observation.gravityZ = gravity.z;
            }
            observation.demoActive = MR::isDemoActive();
            observation.padA = MR::testCorePadButtonA(WPAD_CHAN0);
            observation.padB = MR::testCorePadButtonB(WPAD_CHAN0);
            observation.padPlus = MR::testCorePadButtonPlus(WPAD_CHAN0);
            observation.padMinus = MR::testCorePadButtonMinus(WPAD_CHAN0);
            observation.padOperating = MR::isOperatingWPad(WPAD_CHAN0);
            // CameraContext and TalkDirector are Game-scene SceneObjs: safe here.
            const TVec3f camX = MR::getCamXdir();
            const TVec3f camZ = MR::getCamZdir();
            observation.camXx = camX.x;
            observation.camXy = camX.y;
            observation.camXz = camX.z;
            observation.camZx = camZ.x;
            observation.camZy = camZ.y;
            observation.camZz = camZ.z;
            observation.talkActive = MR::isSystemTalking();
            observation.playerDead = MR::isPlayerDead();
            observation.playerLife = static_cast< int >(pMario->getHealth());
            observation.playerInBind = MR::isPlayerInBind();
            observation.playerSwinging = MR::isPlayerSwingAction();
            // The loaded file's game data (GameDataHolder), read-only; a file
            // is loaded whenever Mario is in a Game scene.
            observation.starEggStar1 = GameDataFunction::hasPowerStar("EggStarGalaxy", 1);
            observation.powerStars = GameDataFunction::calcCurrentPowerStarNum();
            observation.stageResult = GameSequenceFunction::hasStageResultSequence();
            // The Game scene is a GameScene (SceneFactory: "Game").
            if (pController->mScene != nullptr) {
                observation.pausePermitted = static_cast< const GameScene* >(pController->mScene)->isPermitToPauseMenu();
            }
        }
    }
    return observation;
}

}  // namespace PetariNative::App::Smoke

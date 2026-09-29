// Observation for the smoke run (smoke.hpp): read-only queries of game and VI
// state. SDK side: no Aurora or SDL headers here.

#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/SingletonHolder.hpp"

#include <petari/milestone.hpp>
#include <petari/platform/vi.hpp>

#include "smoke.hpp"

namespace PetariNative::App::Smoke {

namespace {
unsigned long gMilestonesRead = 0;
}

Observation observeGame() {
    Observation observation;

    const PetariNative::Platform::VI::DisplayState display = PetariNative::Platform::VI::displayState();
    observation.videoConfigured = display.configured;
    observation.videoBlack = display.black;

    const unsigned long count = petari_milestone_count();
    for (; gMilestonesRead < count; ++gMilestonesRead) {
        const char* name = petari_milestone_at(gMilestonesRead);
        observation.milestones.push_back(name != nullptr ? name : "(milestone history overflowed)");
    }

    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    if (pGameSystem == nullptr || pGameSystem->mSceneController == nullptr) {
        return observation;
    }
    const GameSystemSceneController* pController = pGameSystem->mSceneController;
    observation.scene = pController->mCurrSceneControlInfo.mScene;
    observation.stage = pController->mCurrSceneControlInfo.mStage;
    observation.sceneReady = pController->isSceneInitializeState(SceneInitializeState_End);
    observation.strap = GameSystemFunction::isDisplayStrapRemineder();
    observation.saveSequence = GameSequenceFunction::isActiveSaveDataHandleSequence();
    return observation;
}

}  // namespace PetariNative::App::Smoke

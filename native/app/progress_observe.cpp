// Feeds the personal progress record (petari/progress.hpp, native/PROGRESS.md) from the game,
// once per game frame at the frame seam, and receives the Power Star event. Read-only:
// nothing here changes game state. SDK side: no Aurora or SDL headers.

#include "Game/LiveActor/HitSensor.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"

#include <petari/host_allocation.hpp>
#include <petari/milestone.hpp>
#include <petari/progress.hpp>
#include <petari/progress_hook.hpp>

#include <cstring>

#include "progress_observe.hpp"
#include "smoke_observe_safety.hpp"

namespace PetariNative::App {
namespace {
unsigned long gMilestonesRead = 0;
bool gPaused = false;
bool gStarted = false;
}  // namespace

void observeProgressFrame() {
    // The pause menu has no query of its own; its milestones say when it opens and closes.
    const unsigned long count = petari_milestone_count();
    if (!gStarted) {
        gStarted = true;
        gMilestonesRead = count;
    }
    for (; gMilestonesRead < count; ++gMilestonesRead) {
        const char* name = petari_milestone_at(gMilestonesRead);
        if (name == nullptr) continue;
        if (std::strcmp(name, "PauseMenu.Open") == 0) gPaused = true;
        else if (std::strcmp(name, "PauseMenu.Close") == 0) gPaused = false;
    }

    Progress::FrameState state;
    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    const GameSystemSceneController* pController = pGameSystem != nullptr ? pGameSystem->mSceneController : nullptr;
    if (pController != nullptr && pController->mScene != nullptr && pController->isSceneInitializeState(SceneInitializeState_End) &&
        std::strcmp(pController->mCurrSceneControlInfo.mScene, "Game") == 0 && MR::isExistSceneObj(SceneObj_MarioHolder) &&
        MR::isExistSceneObj(SceneObj_DemoDirector) && MR::isExistSceneObj(SceneObj_ScenePlayingResult)) {
        const MarioHolder* pHolder = MR::getSceneObj< MarioHolder >(SceneObj_MarioHolder);
        const MarioActor* pMario = pHolder != nullptr ? pHolder->getMarioActor() : nullptr;
        if (Smoke::canObservePlayer(pMario)) {
            state.inGame = true;
            state.stage = pController->mCurrSceneControlInfo.mStage;
            state.scenario = pController->mCurrSceneControlInfo.mSelectedScenarioNo;
            state.sceneId = static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(pController->mScene));
            state.demo = MR::isDemoActive();
            state.dead = MR::isPlayerDead();
            state.paused = gPaused;
            state.coins = MR::getCoinNum();
            state.starBits = MR::getStarPieceNum();
            state.purpleCoins = MR::getPurpleCoinNum();
        }
    }
    if (!state.inGame) gPaused = false;
    Progress::frame(state);
}

}  // namespace PetariNative::App

extern "C" void petari_progress_star_get(int mission, int grand) {
    PetariNative::HostAllocationScope host;
    PetariNative::Progress::starGet(mission, grand != 0);
}

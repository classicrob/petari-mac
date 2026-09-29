// The game's phase for the frame-time statistics (host.hpp). SDK side: no
// Aurora or SDL headers here. Reads the same scene-controller values as the
// smoke observation (smoke_game.cpp), on the game thread while it holds the CPU.

#include <cstring>

#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/Util/SingletonHolder.hpp"

#include "host.hpp"

namespace PetariNative::App::Host {

FrameStats::Phase framePhase() {
    static bool sceneReadyOnce = false;
    const GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();
    const GameSystemSceneController* pController = pGameSystem != nullptr ? pGameSystem->mSceneController : nullptr;
    if (pController == nullptr || !pController->isSceneInitializeState(SceneInitializeState_End)) {
        return sceneReadyOnce ? FrameStats::Phase::Loading : FrameStats::Phase::Startup;
    }
    sceneReadyOnce = true;
    const SceneControlInfo& info = pController->mCurrSceneControlInfo;
    if (std::strcmp(info.mScene, "Game") == 0 && info.mStage[0] != '\0' && std::strcmp(info.mStage, "FileSelect") != 0) {
        return FrameStats::Phase::Gameplay;
    }
    return FrameStats::Phase::Menu;
}

}  // namespace PetariNative::App::Host

#include "Game/System/GameSystem.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/NameObj/NameObjRegister.hpp"
#include "Game/Screen/HomeButtonLayout.hpp"
#include "Game/Screen/SystemWipeHolder.hpp"
#include "Game/System/AudSystemWrapper.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/FileRipper.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystemDimmingWatcher.hpp"
#include "Game/System/GameSystemErrorWatcher.hpp"
#include "Game/System/GameSystemException.hpp"
#include "Game/System/GameSystemFontHolder.hpp"
#include "Game/System/GameSystemFrameControl.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/System/GameSystemStationedArchiveLoader.hpp"
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/System/HomeButtonStateNotifier.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/MutexHolder.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "Game/Util/SequenceUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/SystemUtil.hpp"
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <nw4r/lyt/init.h>
#include <revolution.h>

#define GX_FIFO_SIZE 0x80000

#ifdef PETARI_NATIVE
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <petari/app.hpp>
#include <cstdlib>

// Boot progress diagnostics, enabled by setting PETARI_TRACE_BOOT. They only report;
// the nerve flow is unchanged. Heartbeats are at most once per second.
namespace {
    bool isTraceBoot() {
        static const bool sEnabled = [] {
            const char* value = std::getenv("PETARI_TRACE_BOOT");
            return value != nullptr && value[0] != '\0' && value[0] != '0';
        }();
        return sEnabled;
    }

    // Counts frames in the current boot phase; returns true once per second.
    bool isTraceBootHeartbeat(const char* pPhase, u32* pFrames, u32* pSeconds) {
        static const char* sPhase = nullptr;
        static OSTime sPhaseStart = 0;
        static OSTime sLastReport = 0;
        static u32 sFrames = 0;
        OSTime now = OSGetTime();

        if (sPhase != pPhase) {
            sPhase = pPhase;
            sPhaseStart = now;
            sLastReport = now;
            sFrames = 0;
        }

        sFrames++;
        if (OSTicksToSeconds(now - sLastReport) < 1) {
            return false;
        }

        sLastReport = now;
        *pFrames = sFrames;
        *pSeconds = static_cast< u32 >(OSTicksToSeconds(now - sPhaseStart));
        return true;
    }

    void traceBootHeap(const char* pName, JKRHeap* pHeap) {
        if (pHeap == nullptr) {
            OSReport("[boot]   %s: none\n", pName);
            return;
        }

        OSReport("[boot]   %s: size %u, free %d, max free %d\n", pName,
                 static_cast< u32 >(static_cast< u8* >(pHeap->getEndAddr()) - static_cast< u8* >(pHeap->getStartAddr())),
                 pHeap->getTotalFreeSize(), pHeap->getFreeSize());
    }

    void traceBootHeaps() {
        HeapMemoryWatcher* pWatcher = SingletonHolder< HeapMemoryWatcher >::get();
        traceBootHeap("stationed NAPA", pWatcher->mStationedHeapNapa);
        traceBootHeap("stationed GDDR", pWatcher->mStationedHeapGDDR);
        traceBootHeap("game NAPA", pWatcher->mGameHeapNapa);
        traceBootHeap("game GDDR", pWatcher->mGameHeapGDDR);
        traceBootHeap("audio system", pWatcher->mAudSystemHeap);
    }
}  // namespace

#define TRACE_BOOT(...)               \
    do {                              \
        if (isTraceBoot()) {          \
            OSReport(__VA_ARGS__);    \
        }                             \
    } while (0)
#else
#define TRACE_BOOT(...)
#endif

#define INIT_AUDIO_KEY "オーディオ初期化"  // "Audio Initialization"

namespace NrvGameSystem {
    NEW_NERVE(GameSystemInitializeAudio, GameSystem, InitializeAudio);
    NEW_NERVE(GameSystemInitializeLogoScene, GameSystem, InitializeLogoScene);
    NEW_NERVE(GameSystemLoadStationedArchive, GameSystem, LoadStationedArchive);
    NEW_NERVE(GameSystemWaitForReboot, GameSystem, WaitForReboot);
    NEW_NERVE(GameSystemNormal, GameSystem, Normal);
};  // namespace NrvGameSystem

#ifdef PETARI_NATIVE
// The native host entry point initializes the platform, then calls this game main.
extern "C" void petari_game_main(void) {
#else
void main(void) {
#endif
    OSInitFastCast();
    DVDInit();
    VIInit();
    HeapMemoryWatcher::createRootHeap();
    OSInitMutex(&MR::MutexHolder< 0 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 1 >::sMutex);
    OSInitMutex(&MR::MutexHolder< 2 >::sMutex);
    nw4r::lyt::LytInit();
    MR::setLayoutDefaultAllocator();
    SingletonHolder< HeapMemoryWatcher >::init();
    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    FileRipper::setup(0x20000, MR::getStationedHeapNapa());
    GameSystemException::init();
    MR::initAcosTable();
    SingletonHolder< GameSystem >::init();
    SingletonHolder< GameSystem >::get()->init();

    GameSystem* pGameSystem = SingletonHolder< GameSystem >::get();

    while (true) {
        pGameSystem->frameLoop();
    }
}

GameSystem::GameSystem()
    : NerveExecutor("GameSystem"), mFifoBase(nullptr), mSequenceDirector(nullptr), mErrorWatcher(nullptr), mFontHolder(nullptr),
      mFrameControl(nullptr), mObjHolder(nullptr), mSceneController(nullptr), mStationedArchiveLoader(nullptr), mHomeButtonLayout(nullptr),
      mSystemWipeHolder(nullptr), mHomeButtonStateNotifier(nullptr), mIsExecuteLoadSystemArchive(false) {
}

void GameSystem::init() {
    JKRAram::create(0xE00000, 0xFFFFFFFF, 8, 7, 3);
    mObjHolder = new GameSystemObjHolder();
    mFontHolder = new GameSystemFontHolder();
    mFontHolder->createFontFromEmbeddedData();
    initNerve(GET_NERVE(GameSystem, GameSystemInitializeAudio));
    mSequenceDirector = new GameSequenceDirector();
    initGX();
    DrawSyncManager::start(0x300, 15);
    mSceneController = new GameSystemSceneController();
    mObjHolder->init();
    mErrorWatcher = new GameSystemErrorWatcher();
    mFrameControl = new GameSystemFrameControl();
    SingletonHolder< GameSystemResetAndPowerProcess >::init();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->initWithoutIter();
    mStationedArchiveLoader = new GameSystemStationedArchiveLoader();
    mHomeButtonLayout = new HomeButtonLayout();
    mHomeButtonStateNotifier = new HomeButtonStateNotifier();
    mDimmingWatcher = new GameSystemDimmingWatcher();
    setNerve(GET_NERVE(GameSystem, GameSystemInitializeAudio));
}

bool GameSystem::isExecuteLoadSystemArchive() const {
    return mIsExecuteLoadSystemArchive;
}

bool GameSystem::isDoneLoadSystemArchive() const {
    return isNerve(GET_NERVE(GameSystem, GameSystemNormal));
}

void GameSystem::startToLoadSystemArchive() {
    mIsExecuteLoadSystemArchive = true;
#ifdef PETARI_NATIVE
    TRACE_BOOT("[boot] stationed archive loading: start\n");
    if (isTraceBoot()) {
        traceBootHeaps();
    }
#endif

    SingletonHolder< HeapMemoryWatcher >::get()->setCurrentHeapToStationedHeap();
    SingletonHolder< NameObjRegister >::get()->setCurrentHolder(mObjHolder->mObjHolder);
    setNerve(GET_NERVE(GameSystem, GameSystemLoadStationedArchive));
}

void GameSystem::exeInitializeAudio() {
    if (MR::isFirstStep(this)) {
        TRACE_BOOT("[boot] InitializeAudio: starting async audio system creation\n");
        MR::startFunctionAsyncExecute(MR::Functor(mObjHolder, &GameSystemObjHolder::createAudioSystem), 14, INIT_AUDIO_KEY);
    }

    updateSceneController();

#ifdef PETARI_NATIVE
    u32 traceFrames, traceSeconds;
    if (isTraceBoot() && isTraceBootHeartbeat("InitializeAudio", &traceFrames, &traceSeconds)) {
        // The wave-data query reads the audio system, so only ask after creation ended.
        bool isCreated = MR::isEndFunctionAsyncExecute(INIT_AUDIO_KEY);
        OSReport("[boot] InitializeAudio: waiting %us (%u frames), audio system created %d, system wave data loaded %s\n",
                 traceSeconds, traceFrames, isCreated, isCreated ? (mObjHolder->mAudioSystem->isLoadDoneWaveDataAtSystemInit() ? "1" : "0") : "-");
    }
#endif

    if (MR::isEndFunctionAsyncExecute(INIT_AUDIO_KEY) && mObjHolder->mAudioSystem->isLoadDoneWaveDataAtSystemInit()) {
        TRACE_BOOT("[boot] InitializeAudio: audio system created and system wave data loaded\n");
        MR::waitForEndFunctionAsyncExecute(INIT_AUDIO_KEY);
        setNerve(GET_NERVE(GameSystem, GameSystemInitializeLogoScene));
    }
}

void GameSystem::exeInitializeLogoScene() {
    if (GameSystemFunction::isResetProcessing()) {
        TRACE_BOOT("[boot] InitializeLogoScene: reset in progress, waiting for reboot\n");
        setNerve(GET_NERVE(GameSystem, GameSystemWaitForReboot));
    } else {
        if (MR::isFirstStep(this)) {
            TRACE_BOOT("[boot] InitializeLogoScene: requesting scene \"Logo\"\n");
            MR::requestChangeScene("Logo");
        }

        updateSceneController();

#ifdef PETARI_NATIVE
        u32 traceFrames, traceSeconds;
        if (isTraceBoot() && isTraceBootHeartbeat("InitializeLogoScene", &traceFrames, &traceSeconds)) {
            OSReport("[boot] InitializeLogoScene: %us (%u frames), stationed loading requested %d\n", traceSeconds, traceFrames,
                     mIsExecuteLoadSystemArchive);
        }
#endif
    }
}

void GameSystem::exeLoadStationedArchive() {
    mStationedArchiveLoader->update();
    updateSceneController();

#ifdef PETARI_NATIVE
    u32 traceFrames, traceSeconds;
    if (isTraceBoot() && isTraceBootHeartbeat("LoadStationedArchive", &traceFrames, &traceSeconds)) {
        OSReport("[boot] LoadStationedArchive: loading %us (%u frames)\n", traceSeconds, traceFrames);
    }
#endif

    if (mStationedArchiveLoader->isDone()) {
#ifdef PETARI_NATIVE
        TRACE_BOOT("[boot] stationed archive loading: done\n");
        if (isTraceBoot()) {
            traceBootHeaps();
        }
#endif
        setNerve(GET_NERVE(GameSystem, GameSystemNormal));
    }
}

void GameSystem::exeWaitForReboot() {
}

void GameSystem::exeNormal() {
    updateSceneController();
    mStationedArchiveLoader->update();
}

void GameSystem::initGX() {
    if (mFifoBase == nullptr) {
        mFifoBase = new (32) u8[GX_FIFO_SIZE];
    }

    GXInit(mFifoBase, GX_FIFO_SIZE);
}

void GameSystem::initAfterStationedResourceLoaded() {
    TRACE_BOOT("[boot] initAfterStationedResourceLoaded: start\n");
    mFontHolder->createFontFromFile();
    mObjHolder->initAfterStationedResourceLoaded();
    mHomeButtonLayout->initWithoutIter();
    mErrorWatcher->initAfterResourceLoaded();
    mSystemWipeHolder = MR::createSystemWipeHolder();
    mSceneController->initAfterStationedResourceLoaded();
    mSequenceDirector->initAfterResourceLoaded();
#ifdef PETARI_NATIVE
    TRACE_BOOT("[boot] initAfterStationedResourceLoaded: done\n");
    if (isTraceBoot()) {
        traceBootHeaps();
    }
#endif
}

void GameSystem::prepareReset() {
    mStationedArchiveLoader->prepareReset();
}

inline bool isSystemWaitForReboot(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(GET_NERVE(GameSystem, GameSystemWaitForReboot));
}

inline bool isSystemNormal(const GameSystem* pGameSystem) {
    return pGameSystem->isNerve(GET_NERVE(GameSystem, GameSystemNormal));
}

bool GameSystem::isPreparedReset() const {
    return isSystemWaitForReboot(this) || isSystemNormal(this) || mStationedArchiveLoader->isPreparedReset();
}

void GameSystem::frameLoop() {
    MainLoopFramework::sManager->beginRender();
    draw();
    MainLoopFramework::sManager->endRender();
    update();
    calcAnim();
    mObjHolder->captureIfAllowForScreenPreserver();
    MainLoopFramework::sManager->endFrame();
#ifdef PETARI_NATIVE
    // Host window, events and Aurora frame boundary (native/app).
    petari_host_frame_seam();
#endif
    MainLoopFramework::sManager->waitForRetrace();
}

void GameSystem::draw() {
    mSceneController->drawScene();
    mSequenceDirector->draw();
    mObjHolder->drawStarPointer();
    mObjHolder->drawBeforeEndRender();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->draw();
    }

    mErrorWatcher->draw();
    mHomeButtonLayout->draw();
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->draw();
}

void GameSystem::update() {
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->movement();
    mSceneController->checkRequestAndChangeScene();
    mObjHolder->update();
    mHomeButtonLayout->movement();

    if (!mHomeButtonLayout->isActive()) {
        mErrorWatcher->movement();
    }

    mDimmingWatcher->_5 = mErrorWatcher->isWarning() || mHomeButtonLayout->isActive() || GameSequenceFunction::isActiveSaveDataHandleSequence();
    mDimmingWatcher->update();
    updateNerve();
}

void GameSystem::updateSceneController() {
    bool isSceneUpdate = true;
    bool isResetProcessing = SingletonHolder< GameSystemResetAndPowerProcess >::get()->isActive();

    mObjHolder->updateAudioSystem();

    if (isResetProcessing) {
        isSceneUpdate = false;
    }

    if (mHomeButtonLayout->isActive()) {
        isSceneUpdate = false;
    }

    if (GameSystemFunction::isOccurredSystemWarning()) {
        isSceneUpdate = false;
    }

    mHomeButtonStateNotifier->update(mHomeButtonLayout->isActive() || GameSystemFunction::isOccurredSystemWarning());

    if (isSceneUpdate || isResetProcessing) {
        mSequenceDirector->update();
    }

    if (isSceneUpdate || mSceneController->isFirstUpdateSceneNerveNormal()) {
        if (mSystemWipeHolder != nullptr) {
            mSystemWipeHolder->movement();
        }

        mSceneController->updateScene();
    }

    if (isResetProcessing) {
        mSceneController->updateSceneDuringResetProcessing();
    }
}

void GameSystem::calcAnim() {
    mSceneController->calcAnimScene();

    if (mSystemWipeHolder != nullptr) {
        mSystemWipeHolder->calcAnim();
    }
}

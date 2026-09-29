#include "Game/System/GameSystemResetAndPowerProcess.hpp"
#include "Game/LiveActor/Nerve.hpp"
#include "Game/System/DrawSyncManager.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/GameSystemFunction.hpp"
#include "Game/System/MainLoopFramework.hpp"
#include "Game/System/NativeBootTrace.hpp"
#include "Game/Util/Color.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/TriggerChecker.hpp"
#include "Game/Util/ValueControl.hpp"
#include <JSystem/JUtility/JUTVideo.hpp>

namespace {
    static const s32 sFadeinoutFrame = 30;
    static const s32 sResetWaitFrame = 15;
};  // namespace

namespace NrvGameSystemResetAndPowerProcess {
    NEW_NERVE(GameSystemResetAndPowerProcessPolling, GameSystemResetAndPowerProcess, Polling);
    NEW_NERVE(GameSystemResetAndPowerProcessWaitResetPermitted, GameSystemResetAndPowerProcess, WaitResetPermitted);
    NEW_NERVE(GameSystemResetAndPowerProcessPrepareReset, GameSystemResetAndPowerProcess, PrepareReset);
    NEW_NERVE(GameSystemResetAndPowerProcessReset, GameSystemResetAndPowerProcess, Reset);
    NEW_NERVE(GameSystemResetAndPowerProcessWaitPrepareFadein, GameSystemResetAndPowerProcess, WaitPrepareFadein);
    NEW_NERVE(GameSystemResetAndPowerProcessFadein, GameSystemResetAndPowerProcess, Fadein);
};  // namespace NrvGameSystemResetAndPowerProcess

#ifdef PETARI_NATIVE
namespace {
    // Check-disk request of the current Reset nerve: when it was issued and answered.
    OSTime sCheckDiskIssued = 0;
    volatile OSTime sCheckDiskAnswered = 0;
    volatile s32 sCheckDiskResult = -1;

    const char* getResetNerveName(const GameSystemResetAndPowerProcess* pProcess) {
        if (pProcess->isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPolling))) {
            return "Polling";
        }
        if (pProcess->isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitResetPermitted))) {
            return "WaitResetPermitted";
        }
        if (pProcess->isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPrepareReset))) {
            return "PrepareReset";
        }
        if (pProcess->isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessReset))) {
            return "Reset";
        }
        if (pProcess->isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitPrepareFadein))) {
            return "WaitPrepareFadein";
        }
        if (pProcess->isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessFadein))) {
            return "Fadein";
        }
        return "?";
    }
}  // namespace
#endif

void GameSystemResetAndPowerProcess::init(const JMapInfoIter& rIter) {
    initNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPolling));
    OSSetPowerCallback(GameSystemResetAndPowerProcess::handleOSPowerCallback);
    appear();
}

void GameSystemResetAndPowerProcess::draw() const {
    if (!isActive()) {
        return;
    }

    J2DOrthoGraphSimple graph;
    graph.setPort();

    u8 alpha = MR::lerp(255, 0, mFadeinoutControl->getValue());
    graph.setColor(static_cast< const GXColor& >(GXColor(Color8(0, 0, 0, alpha))));

    f32 height = static_cast< s32 >(JUTVideo::getManager()->getRenderMode()->efbHeight);
    f32 width = MR::getScreenWidth();
    graph.fillBox(TBox2f(0.0f, 0.0f, width, height));
}

bool GameSystemResetAndPowerProcess::isActive() const NO_INLINE {
    return !isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPolling));
}

void GameSystemResetAndPowerProcess::setResetOperationApplicationReset() {
    mResetOperation = ResetOperation_ApplicationReset;
}

void GameSystemResetAndPowerProcess::setResetOperationReturnToMenu() {
    mResetOperation = ResetOperation_ReturnToMenu;
}

void GameSystemResetAndPowerProcess::requestReset(bool param1) {
    if (tryPermitReset()) {
        _5E = param1;

        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitResetPermitted));
    }
}

void GameSystemResetAndPowerProcess::requestGoWiiMenu(bool param1) {
    if (tryPermitReset()) {
        _5E = param1;
        setResetOperationReturnToMenu();
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitResetPermitted));
    }
}

void GameSystemResetAndPowerProcess::notifyCheckDiskResult(bool param1) {
    _5C = true;

    if (param1) {
        return;
    }

    if (mResetOperation == ResetOperation_ApplicationReset) {
        setResetOperationReturnToMenu();
    }
}

void GameSystemResetAndPowerProcess::exePolling() {
    if (tryAcceptPowerOff()) {
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitResetPermitted));
    } else if (mResetTriggerChecker->getOnTrigger()) {
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitResetPermitted));
    }
}

void GameSystemResetAndPowerProcess::exeWaitResetPermitted() {
    if (GameSystemFunction::isPermitToResetAudioSystem() && GameSystemFunction::isPermitToResetSaveDataHandleSequence()) {
        if (_5E) {
            mFadeinoutControl->setZero();
        } else {
            mFadeinoutControl->setDirToZero();
        }

        _5E = false;

        GameSystemFunction::activateScreenPreserver();
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPrepareReset));
    }
}

void GameSystemResetAndPowerProcess::exePrepareReset() {
    if (MR::isFirstStep(this)) {
        GameSystemFunction::prepareResetAudioSystem();
        GameSystemFunction::prepareResetSystem();
        GameSystemFunction::resetAllControllerRumble();
        GameSystemFunction::forceToDeactivateHomeButtonLayout();
        GameSequenceFunction::requestPrepareResetNWC24();
        GameSystemFunction::prepareResetSaveDataHandleSequence();
    }

    if (MR::isStep(this, 2)) {
        GameSystemFunction::requestResetAudioSystem(mResetOperation != ResetOperation_ApplicationReset);
    }

    bool b = mFadeinoutControl->mFrame == 0;

    if (b) {
        b = GameSystemFunction::isPrepareResetSaveDataHandleSequence();
    }

    if (b) {
        b = isResetAcceptAudio();
    }

    if (b) {
        b = GameSequenceFunction::isEnableToResetNWC24();
    }

    if (b) {
        GameSequenceFunction::resetNWC24();
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessReset));
    }
}

void GameSystemResetAndPowerProcess::exeReset() {
    if (MR::isFirstStep(this)) {
        _5C = false;

#ifdef PETARI_NATIVE
        sCheckDiskIssued = OSGetTime();
        sCheckDiskAnswered = 0;
        sCheckDiskResult = -1;
#endif
        DVDCheckDiskAsync(&mCommandBlock, GameSystemResetAndPowerProcess::handleCheckDiskAsync);
    }

    if (_5C) {
        GameSystemFunction::setPermissionToCheckWiiRemoteConnectAndScreenDimming(false);

        if (mResetOperation != ResetOperation_ApplicationReset) {
            exitApplication();
        }
    }

    MR::setNerveAtStep(this, GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitPrepareFadein), ::sResetWaitFrame);
}

void GameSystemResetAndPowerProcess::exeWaitPrepareFadein() {
    if (MR::isFirstStep(this)) {
        GameSystemFunction::deactivateScreenPreserver();
        GameSystemFunction::resumeResetAudioSystem();
        MR::forceOpenSystemWipeFade();
    }

    if (GameSystemFunction::isPreparedFadeinSystem()) {
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessFadein));
    }
}

void GameSystemResetAndPowerProcess::exeFadein() {
    if (MR::isFirstStep(this)) {
        GameSystemFunction::restoreFromResetSaveDataHandleSequence();
    }

    if (tryAcceptPowerOff()) {
        GameSystemFunction::restartControllerLeaveWatcher();
        GameSystemFunction::restartSceneController();
        setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessWaitResetPermitted));
    } else {
        if (MR::isFirstStep(this)) {
            mFadeinoutControl->setDirToOneResetFrame();
        }

        if (mFadeinoutControl->mFrame == mFadeinoutControl->mMaxFrame) {
            GameSystemFunction::restartControllerLeaveWatcher();
            GameSystemFunction::restartSceneController();
            setNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPolling));
        }
    }
}

void GameSystemResetAndPowerProcess::exitApplication() {
    NATIVE_TRACE_BOOT("[reset] exitApplication: operation %d\n", mResetOperation);
    DrawSyncManager::end();
    MainLoopFramework::setForOSResetSystem();
    VISetBlack(TRUE);
    VIFlush();
    VIWaitForRetrace();

    switch (mResetOperation) {
    case ResetOperation_Restart:
        OSRestart(0);
        break;
    case ResetOperation_ReturnToMenu:
        OSReturnToMenu();
        break;
    case ResetOperation_RebootSystem:
        OSRebootSystem();
        break;
    case ResetOperation_ShutdownSystem:
        OSShutdownSystem();
        break;
    }
}

bool GameSystemResetAndPowerProcess::tryPermitReset() NO_INLINE {
    return !isActive();
}

bool GameSystemResetAndPowerProcess::tryAcceptPowerOff() {
    if (!mIsValidPowerOff) {
        return false;
    }

    mIsValidPowerOff = false;
    mResetOperation = ResetOperation_ShutdownSystem;

    return true;
}

bool GameSystemResetAndPowerProcess::isResetAcceptAudio() const {
    if (mResetOperation != ResetOperation_ApplicationReset && MR::isGreaterStep(this, 2) && GameSystemFunction::isDoneResetAudioSystem()) {
        return true;
    }

    return MR::isGreaterStep(this, 17);
}

void GameSystemResetAndPowerProcess::control() {
    mResetTriggerChecker->update(OSGetResetButtonState() != FALSE);
    mFadeinoutControl->update();

#ifdef PETARI_NATIVE
    // Reset/power state: every nerve change, and once per second outside Polling, with
    // the gates WaitResetPermitted and PrepareReset wait for and the Reset check-disk state.
    if (MR::Native::isTraceBoot()) {
        static const char* sPrevNerve = nullptr;
        static OSTime sLastReport = 0;
        const char* pNerve = getResetNerveName(this);
        bool isChanged = pNerve != sPrevNerve;

        if (isChanged || (isActive() && MR::Native::isTraceHeartbeat(&sLastReport))) {
            s32 answeredMs = sCheckDiskAnswered == 0 ? -1 : static_cast< s32 >(OSTicksToMilliseconds(sCheckDiskAnswered - sCheckDiskIssued));
            // The NWC24 messenger is queried only where PrepareReset itself queries it (-1 elsewhere).
            bool isNWC24Queried = isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessPrepareReset)) ||
                                  isNerve(GET_NERVE(GameSystemResetAndPowerProcess, GameSystemResetAndPowerProcessReset));
            OSReport("[reset] %s %s (step %d): operation %d, power request %d, fade frame %d; permit audio %d, permit save %d, "
                     "save prepared %d, audio accepted %d, NWC24 idle %d; check disk done %d (result %d, %d ms)\n",
                     isChanged ? "nerve" : "still", pNerve, getNerveStep(), mResetOperation, mIsValidPowerOff, mFadeinoutControl->mFrame,
                     GameSystemFunction::isPermitToResetAudioSystem(), GameSystemFunction::isPermitToResetSaveDataHandleSequence(),
                     GameSystemFunction::isPrepareResetSaveDataHandleSequence(), isResetAcceptAudio(),
                     isNWC24Queried ? GameSequenceFunction::isEnableToResetNWC24() : -1, _5C, sCheckDiskResult, answeredMs);
            sPrevNerve = pNerve;
        }
    }
#endif
}

void GameSystemResetAndPowerProcess::handleOSPowerCallback() {
    NATIVE_TRACE_BOOT("[reset] power callback\n");
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->mIsValidPowerOff = true;
}

void GameSystemResetAndPowerProcess::handleCheckDiskAsync(s32 result, DVDCommandBlock* pBlock) {
#ifdef PETARI_NATIVE
    // Drive thread; the game thread's trace reports it.
    sCheckDiskResult = result;
    sCheckDiskAnswered = OSGetTime();
#endif
    SingletonHolder< GameSystemResetAndPowerProcess >::get()->notifyCheckDiskResult(result != 0);
}

GameSystemResetAndPowerProcess::GameSystemResetAndPowerProcess()
    : LayoutActor("リセット・電源", false), mResetTriggerChecker(), mFadeinoutControl(), mResetOperation(ResetOperation_Restart), _5C(true),
      mIsValidPowerOff(), _5E() {
    mResetTriggerChecker = new TriggerChecker();

    mFadeinoutControl = new ValueControl(::sFadeinoutFrame);
    mFadeinoutControl->setOne();
}

GameSystemResetAndPowerProcess::~GameSystemResetAndPowerProcess() {
}

#include "Game/System/NativeUnlockedSave.hpp"

#ifdef PETARI_NATIVE
#include "Game/System/ConfigDataHolder.hpp"
#include "Game/System/FindingLuigiEventScheduler.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/System/GameDataConst.hpp"
#include "Game/System/GameDataHolder.hpp"
#include "Game/System/GameEventFlag.hpp"
#include "Game/System/GameEventFlagTable.hpp"
#include "Game/System/SaveDataHandleSequence.hpp"
#include "Game/System/SaveDataHandler.hpp"
#include "Game/System/ScenarioDataParser.hpp"
#include "Game/System/UserFile.hpp"
#include "Game/Util/StringUtil.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <petari/unlocked_save.hpp>

namespace {
    enum Phase { Phase_Load, Phase_Saving, Phase_Reload, Phase_Done };

    enum Variant { Variant_AllMissions, Variant_CompleteLuigi, Variant_GrandFinale };

    // The last story event (StoryEvent.bcsv): the observatory's restoration story is over.
    const char cLastStoryEvent[] = "クッパＪｒクリーチャープラント発見";
    const u32 cGameDataSize = 0xF80;

    Phase sPhase = Phase_Load;
    // The game data and config binaries as stored, compared after the reload.
    u8 sStoredMario[cGameDataSize];
    u8 sStoredLuigi[cGameDataSize];
    u8 sReloaded[cGameDataSize];
    s32 sFailures = 0;

    Variant getVariant() {
        const std::string& rName = PetariNative::UnlockedSave::variant;

        if (rName == "complete-luigi") {
            return Variant_CompleteLuigi;
        }

        if (rName == "grand-finale") {
            return Variant_GrandFinale;
        }

        return Variant_AllMissions;
    }

    void check(bool condition, const char* pWhat, const char* pDetail = "") {
        if (!condition) {
            std::fprintf(stderr, "PETARI UNLOCKED SAVE: FAIL %s %s\n", pWhat, pDetail);
            sFailures++;
        }
    }

    // The Grand Finale's star needs both endings; every variant leaves it to be won.
    bool isStarUnlocked(const char* pGalaxyName, s32 starId) {
        return !GameEventFlagTable::isPowerStarType(pGalaxyName, starId, "SpecialStarFinalChallenge");
    }

    s32 calcUnlockedStarNum() {
        s32 num = 0;

        for (ScenarioDataIter iter = MR::makeBeginScenarioDataIter(); !iter.isEnd(); iter.goNext()) {
            GalaxyStatusAccessor accessor = iter.makeAccessor();

            for (s32 starId = 1; starId <= accessor.getPowerStarNum(); starId++) {
                if (isStarUnlocked(accessor.getName(), starId)) {
                    num++;
                }
            }
        }

        return num;
    }

    bool isCompleteEndingSeen(bool isMario) {
        Variant variant = getVariant();

        return variant == Variant_GrandFinale || (variant == Variant_CompleteLuigi && isMario);
    }

    // Every change goes through the game's own progression API. Only stored
    // flags are set; flags the game derives (grand stars, domes, comets, library,
    // hungry Luma galaxies) follow from the stars, fed star bits and story.
    void unlockGameData(GameDataHolder* pHolder, bool isMario) {
        pHolder->resetAllData();

        for (ScenarioDataIter iter = MR::makeBeginScenarioDataIter(); !iter.isEnd(); iter.goNext()) {
            GalaxyStatusAccessor accessor = iter.makeAccessor();
            const char* pGalaxyName = accessor.getName();

            for (s32 starId = 1; starId <= accessor.getPowerStarNum(); starId++) {
                if (isStarUnlocked(pGalaxyName, starId)) {
                    pHolder->setPowerStar(pGalaxyName, starId, true);
                    pHolder->onGalaxyScenarioFlagAlreadyVisited(pGalaxyName, starId);
                }
            }
        }

        // Hungry Lumas in the observatory: fed in full, so their galaxies exist.
        for (s32 idx = 0; idx < GameEventFlagTable::calcExclamationGalaxyNum(); idx++) {
            pHolder->addStarPieceGivingToTicoSeed(idx + 8, pHolder->getStarPieceNumMaxGivingToTicoSeed(idx + 8));
        }

        pHolder->followStoryEventByName(cLastStoryEvent);
        // Opened after the Grand Star 3 return and one more star.
        pHolder->setGameEventValue("LibraryOpenNewStarCount", 0);
        pHolder->setGameEventValue("LuigiEventState", FindingLuigiEventScheduler::STATE_END);

        // Stored flags: every galaxy opening demo seen and the one-time talks
        // done. First power-up and tutorial explanations stay unseen.
        for (s32 pass = 0; pass < 2; pass++) {
            for (GameEventFlagIter iter = GameEventFlagTable::getBeginIter(); !iter.isEnd(); iter.goNext()) {
                const GameEventFlag* pFlag = iter.getFlag();

                if ((pFlag->mSaveFlag & 0x1) != 0) {
                    continue;
                }

                if (pFlag->mType != GameEventFlag::Type_Galaxy && pFlag->mType != GameEventFlag::Type_EventFlag) {
                    continue;
                }

                if (MR::isEqualString(pFlag->mName, "ViewCompleteEnding") && !isCompleteEndingSeen(isMario)) {
                    continue;
                }

                pHolder->tryOnGameEventFlag(pFlag->mName);
            }
        }

        pHolder->setPictureBookChapterAlreadyRead(pHolder->getPictureBookChapterCanRead());
    }

    void unlockUserFile(SaveDataHandleSequence* pSequence, UserFile* pFile, SaveDataHandler* pHandler, bool isMario, u8* pStored) {
        const int slot = PetariNative::UnlockedSave::slot;

        pSequence->restoreUserFile(pFile, slot, isMario);
        unlockGameData(pFile->mGameDataHolder, isMario);
        pFile->updateLastModified();
        pHandler->storeUserFile(pFile);

        std::memset(pStored, 0, cGameDataSize);
        pFile->makeGameDataBinary(pStored, cGameDataSize);
    }

    void reportGalaxies(const GameDataHolder* pHolder, const char* pPlayer) {
        const bool isGrandFinale = getVariant() == Variant_GrandFinale;

        for (u32 dome = 0; dome <= 8; dome++) {
            char line[1024];
            s32 length = std::snprintf(line, sizeof(line), "PETARI UNLOCKED SAVE: %s dome %u:", pPlayer, dome);

            for (ScenarioDataIter iter = MR::makeBeginScenarioDataIter(); !iter.isEnd(); iter.goNext()) {
                GalaxyStatusAccessor accessor = iter.makeAccessor();
                const char* pGalaxyName = accessor.getName();

                if (accessor.getPowerStarNum() == 0 || GameDataConst::getIncludedGrandGalaxyId(pGalaxyName) != dome) {
                    continue;
                }

                const bool isOpen = !GameEventFlagTable::isExist(pGalaxyName) || pHolder->isOnGameEventFlag(pGalaxyName);
                const bool isAppear = pHolder->isAppearGalaxy(pGalaxyName);
                const bool isFinal = MR::isEqualString(pGalaxyName, "PeachCastleFinalGalaxy");
                s32 unlocked = 0;

                for (s32 starId = 1; starId <= accessor.getPowerStarNum(); starId++) {
                    if (isStarUnlocked(pGalaxyName, starId)) {
                        unlocked++;
                    }
                }

                const s32 owned = pHolder->getPowerStarNumOwned(pGalaxyName);

                if (length < static_cast< s32 >(sizeof(line))) {
                    length += std::snprintf(line + length, sizeof(line) - length, " %s %d/%d%s", pGalaxyName, owned, accessor.getPowerStarNum(),
                                            isOpen && isAppear ? "" : " LOCKED");
                }

                check(owned == unlocked, "stars owned in", pGalaxyName);

                if (isFinal && !isGrandFinale) {
                    check(!isOpen, "Grand Finale is open without both endings:", pGalaxyName);
                } else {
                    check(isOpen && isAppear, "galaxy not open:", pGalaxyName);
                }
            }

            std::fprintf(stderr, "%s\n", line);
        }
    }

    void checkUserFile(SaveDataHandleSequence* pSequence, UserFile* pFile, bool isMario, const u8* pStored) {
        const char* pPlayer = isMario ? "Mario" : "Luigi";
        GameDataHolder* pHolder = pFile->mGameDataHolder;

        pSequence->restoreUserFile(pFile, PetariNative::UnlockedSave::slot, isMario);
        check(!pFile->mIsGameDataCorrupted, "game data reported corrupt by the loader for", pPlayer);
        check(!pFile->mIsConfigDataCorrupted, "config data reported corrupt by the loader for", pPlayer);
        check(pFile->isCreated(), "file not created for", pPlayer);

        std::memset(sReloaded, 0, sizeof(sReloaded));
        pFile->makeGameDataBinary(sReloaded, cGameDataSize);
        check(std::memcmp(sReloaded, pStored, cGameDataSize) == 0, "reloaded game data differs from the stored binary for", pPlayer);

        const s32 starNum = pHolder->calcCurrentPowerStarNum();
        const s32 expectedStarNum = calcUnlockedStarNum();
        check(starNum == expectedStarNum, "star count for", pPlayer);

        s32 grandStarNum = 0;
        for (s32 idx = 1; idx <= 7; idx++) {
            grandStarNum += pHolder->hasGrandStar(idx) ? 1 : 0;
        }
        check(grandStarNum == 7, "grand stars for", pPlayer);
        check(pHolder->getGalaxyNumCanOpen() == 0, "galaxies left to open (opening demos pending) for", pPlayer);
        check(pHolder->isPassedStoryEvent(cLastStoryEvent), "story progress for", pPlayer);
        check(pHolder->isOnGameEventFlag("LibraryOpenRequirement"), "library closed for", pPlayer);
        check(pHolder->isOnGameEventFlag("SpecialStarGreenAll"), "green stars for", pPlayer);
        check(pHolder->isOnGameEventFlag("EventCoin100CometStarter"), "purple comets for", pPlayer);
        check(pHolder->isOnGameEventFlag("ViewNormalEnding"), "normal ending for", pPlayer);
        check(pHolder->isOnGameEventFlag("ViewCompleteEnding") == isCompleteEndingSeen(isMario), "120-star ending state for", pPlayer);
        check(pHolder->isOnGameEventFlag("RosettaTalkAfterNormalEnding"), "Rosalina's after-ending talk pending for", pPlayer);
        check(pHolder->getPictureBookChapterAlreadyRead() == pHolder->getPictureBookChapterCanRead(), "storybook chapters for", pPlayer);

        for (s32 idx = 0; idx < GameEventFlagTable::calcExclamationGalaxyNum(); idx++) {
            const char* pGalaxyName = GameEventFlagTable::getExclamationGalaxyNameFromIndex(idx);
            char flagName[64];
            std::snprintf(flagName, sizeof(flagName), "StarPiece%s", pGalaxyName);
            check(pHolder->isOnGameEventFlag(flagName), "hungry Luma not fed:", flagName);
        }

        std::fprintf(stderr,
                     "PETARI UNLOCKED SAVE: %s file %d: %d/%d stars, grand stars %d/7, normal ending %d, 120-star ending %d, "
                     "Mario ending %d, Luigi ending %d, storybook %d/9, corrupt game %d config %d\n",
                     pPlayer, PetariNative::UnlockedSave::slot, static_cast< int >(starNum), static_cast< int >(expectedStarNum),
                     static_cast< int >(grandStarNum), pHolder->isOnGameEventFlag("ViewNormalEnding"),
                     pHolder->isOnGameEventFlag("ViewCompleteEnding"), pFile->mConfigDataHolder->isOnCompleteEndingMario(),
                     pFile->mConfigDataHolder->isOnCompleteEndingLuigi(), static_cast< int >(pHolder->getPictureBookChapterAlreadyRead()),
                     pFile->mIsGameDataCorrupted, pFile->mIsConfigDataCorrupted);
        reportGalaxies(pHolder, pPlayer);
    }

    [[noreturn]] void finish() {
        sPhase = Phase_Done;
        std::fprintf(stderr, "PETARI UNLOCKED SAVE: %s (%s, file %d, %d failure(s))\n", sFailures == 0 ? "VERIFIED" : "FAILED",
                     PetariNative::UnlockedSave::variant.c_str(), PetariNative::UnlockedSave::slot, static_cast< int >(sFailures));
        std::fflush(stderr);
        std::_Exit(sFailures == 0 ? 0 : 1);
    }
}  // namespace

namespace NativeUnlockedSave {
    bool isRequested() {
        return !PetariNative::UnlockedSave::variant.empty() && sPhase != Phase_Done;
    }

    bool onLoaded(SaveDataHandleSequence* pSequence, UserFile* pWorkFile, SaveDataHandler* pHandler) {
        const int slot = PetariNative::UnlockedSave::slot;
        const Variant variant = getVariant();

        if (sPhase == Phase_Load) {
            pSequence->restoreUserFile(pWorkFile, slot, true);

            if (!pWorkFile->isCreated() || pWorkFile->mIsConfigDataCorrupted) {
                fail("the seed's file slot has no created (or has a corrupt) file; create one in the game first");
            }

            // Ending records live in the shared config data, before the game data
            // that derives the Grand Finale from them.
            if (variant != Variant_AllMissions) {
                pWorkFile->mConfigDataHolder->onCompleteEndingMario();
            }

            if (variant == Variant_GrandFinale) {
                pWorkFile->mConfigDataHolder->onCompleteEndingLuigi();
            }

            pWorkFile->updateLastModified();
            pHandler->storeUserFile(pWorkFile);

            if (variant == Variant_GrandFinale) {
                unlockUserFile(pSequence, pWorkFile, pHandler, false, sStoredLuigi);
            }

            // Last, so the file opens as Mario.
            unlockUserFile(pSequence, pWorkFile, pHandler, true, sStoredMario);
            std::fprintf(stderr, "PETARI UNLOCKED SAVE: %s applied to file %d; saving through the game's save sequence\n",
                         PetariNative::UnlockedSave::variant.c_str(), slot);
            std::fflush(stderr);
            sPhase = Phase_Saving;
            return true;
        }

        if (sPhase == Phase_Reload) {
            std::fprintf(stderr, "PETARI UNLOCKED SAVE: reloaded GameData.bin passed the game's header, size and checksum checks\n");
            checkUserFile(pSequence, pWorkFile, true, sStoredMario);

            if (variant == Variant_GrandFinale) {
                checkUserFile(pSequence, pWorkFile, false, sStoredLuigi);
            }

            finish();
        }

        return false;
    }

    bool isSaveWritten(bool isSequenceIdle) {
        if (sPhase != Phase_Saving || !isSequenceIdle) {
            return false;
        }

        std::fprintf(stderr, "PETARI UNLOCKED SAVE: save sequence finished; reloading through the game's loader\n");
        std::fflush(stderr);
        sPhase = Phase_Reload;
        return true;
    }

    void fail(const char* pReason) {
        std::fprintf(stderr, "PETARI UNLOCKED SAVE: FAIL %s\n", pReason);
        sFailures++;
        finish();
    }
}  // namespace NativeUnlockedSave
#endif

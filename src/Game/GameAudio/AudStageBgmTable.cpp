#include "Game/GameAudio/AudStageBgmTable.hpp"
#include "Game/AudioLib/AudSoundId.hpp"
#include "Game/GameAudio/AudStageBgmWrap.hpp"
#include "Game/Util/StringUtil.hpp"
#include <JSystem/JAudio2/JAISound.hpp>

// TODO: bgm state enum

namespace {
    struct StageBgm {
        /* 0x00 */ const char* mStageName;
        /* 0x04 */ u32 mBgmIDs[6];
        /* 0x1C */ u32 mBgmStates[8];
    };

    StageBgm sStageBgmSet[] = {
        {
            "OceanRingGalaxy",
            {0xFFFFFFFF, MBGM_GALAXY_03, MBGM_GALAXY_18, MBGM_GALAXY_01_TOMB, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "OceanPhantomCaveGalaxy",
            {MBGM_GALAXY_12, MBGM_GALAXY_05, MBGM_GALAXY_INTER, MBGM_GALAXY_06, MBGM_GALAXY_18, 0xFFFFFFFF},
            {0, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "EggStarGalaxy",
            {MBGM_GALAXY_12, MBGM_GALAXY_01, MBGM_GALAXY_INTER, MBGM_GALAXY_01_TOMB, MBGM_GALAXY_18, 0xFFFFFFFF},
            {0, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "StarDustGalaxy",
            {MBGM_GALAXY_09, MBGM_GALAXY_08, MBGM_GALAXY_01_TOMB, MBGM_GALAXY_12, MBGM_GALAXY_13, MBGM_KINOPIO_TANKEN},
            {0, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "PhantomGalaxy",
            {MBGM_GALAXY_INTER, MBGM_GALAXY_06, MBGM_GALAXY_12, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF},
            {0, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "BattleShipGalaxy",
            {MBGM_GALAXY_11, MBGM_GALAXY_13, MBGM_GALAXY_01_TOMB, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF},
            {0, 1, 7, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "HeavenlyBeachGalaxy",
            {MBGM_GALAXY_03, MBGM_GALAXY_13, MBGM_GALAXY_18, MBGM_GALAXY_17, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "IceVolcanoGalaxy",
            {MBGM_GALAXY_INTER, MBGM_GALAXY_16, MBGM_GALAXY_13, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF},
            {0, 1, 2, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "CannonFleetGalaxy",
            {MBGM_GALAXY_11, MBGM_GALAXY_13, MBGM_GALAXY_01_TOMB, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF},
            {0, 1, 7, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "FactoryGalaxy",
            {MBGM_GALAXY_22, MBGM_GALAXY_18, MBGM_BOSS_MECHA_KOOPA, MBGM_GALAXY_27, MBGM_BOSS_05_A, 0xFFFFFFFF},
            {0, 1, 2, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "SandClockGalaxy",
            {MBGM_GALAXY_19, MBGM_GALAXY_13, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "HeavensDoorGalaxy",
            {MBGM_GALAXY_24, MBGM_GALAXY_25, MBGM_GALAXY_26, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "HoneyBeeKingdomGalaxy",
            {MBGM_GALAXY_10, MBGM_GALAXY_18, MBGM_GALAXY_10_HURRY, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "ReverseKingdomGalaxy",
            {MBGM_GALAXY_10, MBGM_GALAXY_18, MBGM_GALAXY_INTER, MBGM_BOSS_05_A, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "CosmosGardenGalaxy",
            {MBGM_GALAXY_14, MBGM_GALAXY_13, MBGM_GALAXY_12, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "HellProminenceGalaxy",
            {MBGM_GALAXY_02, MBGM_GALAXY_18, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "KoopaBattleVs3Galaxy",
            {MBGM_GALAXY_28, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "SurfingLv1Galaxy",
            {MBGM_GALAXY_03, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
        {
            "SurfingLv2Galaxy",
            {MBGM_GALAXY_03, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
            {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
        },
    };

    const StageBgm* findDataElement(const char* pStageName) {
        for (u32 idx = 0; idx < ARRAY_SIZE(sStageBgmSet); idx++) {
            const StageBgm* bgm = &sStageBgmSet[idx];
            if (MR::isEqualString(pStageName, bgm->mStageName)) {
                return bgm;
            }
        }
        return nullptr;
    }
};  // namespace

u32 AudStageBgmTable::getBgmId(const char* pStageName, s32 bgmNo) {
    JAISoundID cometBgm = AudStageBgmWrap::getCometEventBgm(pStageName);
    if (cometBgm != -1) {
        return cometBgm;
    }

    const StageBgm* bgm = ::findDataElement(pStageName);
    if (bgm != nullptr) {
        return bgm->mBgmIDs[bgmNo];
    }

    return 0;
}

u32 AudStageBgmTable::getBgmState(const char* pStageName, s32 stateNo) {
    const StageBgm* bgm = ::findDataElement(pStageName);
    if (bgm != nullptr) {
        return bgm->mBgmStates[stateNo];
    }

    return 0;
}

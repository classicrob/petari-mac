#include "Game/Effect/ParticleResourceHolder.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/StringUtil.hpp"
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <JSystem/JParticle/JPAResourceManager.hpp>
#include <cstring>
#ifdef PETARI_NATIVE
#include <petari/jpa_resource.hpp>
#include <revolution/os.h>
#endif

ParticleResourceHolder::ParticleResourceHolder(const char* pArchiveName)
    : mResourceMgr(), mAutoEffectList(new JMapInfo), mParticleNames(new JMapInfo), mNumEffectNums() {
    JKRMemArchive* pArchive = MR::mountArchive(pArchiveName, nullptr);
#ifdef PETARI_NATIVE
    // The particle container is big-endian; JPA reads it through structs, so natively the
    // resource manager gets a host-layout image (petari/jpa_resource.hpp) allocated on the
    // same heap as the manager.
    void* pParticles = pArchive->getResource("Particles.jpc");
    const u32 particlesSize = pArchive->getResSize(pParticles);
    u8* pParticleImage = new (MR::getCurrentHeap(), 0x20) u8[particlesSize];
    if (const char* pError = PetariNative::JPA::makeHostImage(pParticles, particlesSize, pParticleImage)) {
        OSPanic(__FILE__, __LINE__, "Particles.jpc in %s: %s", pArchiveName, pError);
    }
    mResourceMgr = new JPAResourceManager(pParticleImage, MR::getCurrentHeap());
#else
    mResourceMgr = new JPAResourceManager(pArchive->getResource("Particles.jpc"), MR::getCurrentHeap());
#endif
    mParticleNames->attach(pArchive->getResource("ParticleNames.bcsv"));
    mAutoEffectList->attach(pArchive->getResource("AutoEffectList.bcsv"));
    countAutoEffectNum();
}

u16 ParticleResourceHolder::getUserIndex(const char* pName) const {
    const JMapInfo* pInfo = mParticleNames;
    int middle;
    int last;
    int first = 0;
    last = pInfo->getNumEntries() - 1;

    const char* pEntryName;

    while (first < last) {
        middle = (first + last) / 2;
        const char* pEntryName = "";

        if (!pInfo->getValue(middle, "name", &pEntryName)) {
            return 0xFFFF;
        }

        const int comparison = strcmp(pEntryName, pName);

        if (comparison == 0) {
            return middle;
        }

        if (comparison < 0) {
            first = middle + 1;
        }

        if (comparison >= 0) {
            last = middle;
        }
    }

    pEntryName = "";

    if (!pInfo->getValue(first, "name", &pEntryName)) {
        return 0xFFFF;
    }

    if (strcmp(pEntryName, pName) == 0) {
        return first;
    }

    return 0xFFFF;
}

void ParticleResourceHolder::countAutoEffectNum() {
    for (JMapInfoIter iter = mAutoEffectList->begin(); iter != mAutoEffectList->end(); iter.mIndex++) {
        const char* pGroupName = nullptr;
        iter.getValue("GroupName", &pGroupName);

        if (pGroupName != nullptr) {
            bool found = false;

            for (EffectNum** pParticle = mEffectNums; pParticle != mEffectNums + mNumEffectNums; pParticle++) {
                if (MR::isEqualStringCase((*pParticle)->mGroupName, pGroupName)) {
                    (*pParticle)->mCount++;
                    found = true;
                    break;
                }
            }

            if (!found) {
                EffectNum* pParticle = new EffectNum(pGroupName);
                const int index = mNumEffectNums++;
                mEffectNums[index] = pParticle;
            }
        }
    }
}

void ParticleResourceHolder::swapTexture(const ResTIMG* pImage, const char* pName) {
    mResourceMgr->swapTexture(pImage, pName);
}

bool ParticleResourceHolder::isExistInResource(const char* pName, u16* pIndex) const {
    const u16 index = getUserIndex(pName);

    if (index == 0xFFFF) {
        return false;
    }

    if (pIndex != nullptr) {
        *pIndex = index;
    }

    return true;
}

JMapInfo* ParticleResourceHolder::getAutoEffectListBinary() const {
    return mAutoEffectList;
}

int ParticleResourceHolder::getAutoEffectNum(const char* pGroupName) const {
    if (pGroupName == nullptr) {
        return 0;
    }

    for (EffectNum* const* pParticle = mEffectNums; pParticle != mEffectNums + mNumEffectNums; pParticle++) {
        if (MR::isEqualStringCase((*pParticle)->mGroupName, pGroupName)) {
            return (*pParticle)->mCount;
        }
    }

    return 0;
}

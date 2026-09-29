#pragma once

#include <JSystem/JGeometry/TVec.hpp>
#ifdef PETARI_NATIVE
#include <cstdint>
#endif

class GravityInfo;
class JMapInfoIter;
class LiveActor;
class NameObj;
class PlanetGravity;

/// @brief Identity of the object requesting gravity, used to skip gravities it hosts.
/// It holds an object address, so it is pointer-sized natively. 0 means the requester itself.
#ifdef PETARI_NATIVE
typedef uintptr_t GravityHostID;
#else
typedef u32 GravityHostID;
#endif

namespace MR {
    void registerGravity(PlanetGravity* pGravity);
    bool calcGravityVector(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcGravityVector(const NameObj* pObj, const TVec3f& rPosition, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcDropShadowVector(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcDropShadowVector(const NameObj* pObj, const TVec3f& rPosition, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcGravityAndDropShadowVector(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcGravityAndMagnetVector(const NameObj* pObj, const TVec3f& rPosition, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcGravityVectorOrZero(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcGravityVectorOrZero(const NameObj* pObj, const TVec3f& rPosition, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcDropShadowVectorOrZero(const NameObj* pObj, const TVec3f& rPosition, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcGravityAndDropShadowVectorOrZero(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool calcAttractMarioLauncherOrZero(const LiveActor* pActor, TVec3f* pDest, GravityInfo* pInfo, GravityHostID host);
    bool isZeroGravity(const LiveActor* pActor);
    bool isLightGravity(const GravityInfo& rInfo);
    void settingGravityParamFromJMap(PlanetGravity* pGravity, const JMapInfoIter& rIter);
    void getJMapInfoGravityType(const JMapInfoIter& rIter, PlanetGravity* pGravity);
    void getJMapInfoGravityPower(const JMapInfoIter& rIter, PlanetGravity* pGravity);
};  // namespace MR

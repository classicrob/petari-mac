#pragma once

#include "Game/MapObj/MapPartsFunction.hpp"
#include <JSystem/JGeometry/TVec.hpp>

class FloaterFloatingForce : public MapPartsFunction {
public:
    FloaterFloatingForce(LiveActor*, const char*);

    virtual ~FloaterFloatingForce();
    virtual void init(const JMapInfoIter&);
    virtual void start();

    virtual void updateHostTrans(TVec3f*) const NO_INLINE {
    }

    virtual void updateHostVelocity(TVec3f*) const {
    }

    virtual const TVec3f& getCurrentVelocity() const {
#ifdef PETARI_NATIVE
        // The Wii build returns a reference to a temporary zero vector.
        static const TVec3f sZero(0.0f, 0.0f, 0.0f);
        return sZero;
#else
        return TVec3f(0.0f, 0.0f, 0.0f);
#endif
    }

    /* 0x18 */ const char* _18;
    /* 0x1C */ TVec3f _1C;
    /* 0x28 */ TVec3f _28;
    /* 0x34 */ f32 mMoveConditionType;
};

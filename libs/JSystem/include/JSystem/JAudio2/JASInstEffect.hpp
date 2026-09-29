#pragma once

#include "JSystem/JAudio2/JASBasicInst.hpp"

class JASInstParam;

struct JASInstEffect {
    enum EffectType {
        VOLUME = 0,
        PITCH = 1,
        PAN = 2,
        FXMIX = 3,
        DOLBY = 4,
    };

    JASInstEffect() {};

#ifdef PETARI_NATIVE
    // Only JASInstRand and JASInstSense are instantiated, and the base effect has
    // no definition, so natively it is pure virtual to emit the base vtable.
    virtual void effect(int, int, JASInstParam*) const = 0;
#else
    virtual void effect(int, int, JASInstParam*) const;
#endif
};

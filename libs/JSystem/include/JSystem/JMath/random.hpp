#pragma once

#ifdef PETARI_NATIVE
#include <revolution/types.h>
#include <cstring>
#else
#include <revolution.h>
#endif

namespace JMath {
    class TRandom_fast_ {
    public:
        TRandom_fast_() {};

        TRandom_fast_(u32);

        inline u32 rand() {
            return mSeed = 0x19660d * mSeed + 0x3c6ef35f;
            ;
        }

        inline float getRandF() {
            #ifdef PETARI_NATIVE
            const u32 bits = (rand() >> 9) | 0x3f800000;
            f32 result;
            std::memcpy(&result, &bits, sizeof(result));
            return result - 1.0f;
            #else
            // !@bug UB: in C++ it's not legal to read from an union member other
            // than the last one that was written to.
            union {
                f32 f;
                u32 s;
            } out;
            out.s = (rand() >> 9) | 0x3f800000;
            return out.f - 1;
            #endif
        }

        inline u32 getRand(u32 range) {
            return (rand() >> 9) % range;
        }

        u32 mSeed;
    };

}  // namespace JMath

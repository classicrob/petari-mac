// Bitfield layout regression tests for game structs whose bitfields are also used through
// numeric masks. MWCC (the Wii compiler) allocates bitfields from the most significant bit
// of each unit; the native headers declare those fields in reverse so every numeric view
// matches the Wii. Each check sets one field and compares the word with the Wii mask.
// Links src/Game/Player/RushEndInfo.cpp; the other game headers are used header-only.
#include "Game/Player/J3DModelX.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/RushEndInfo.hpp"
#include <cstdio>
#include <cstring>

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

template < typename T >
static u32 word(const T& value, int index = 0) {
    u32 result;
    std::memcpy(&result, reinterpret_cast< const u8* >(&value) + index * 4, sizeof(result));
    return result;
}

static void testMovementStates() {
    static_assert(sizeof(Mario::MovementStates) == 8, "MovementStates is two words");
    Mario::MovementStates states;
    std::memset(&states, 0, sizeof(states));
    check(word(states, 0) == 0 && word(states, 1) == 0, "MovementStates zero-initializes");

    states.jumping = true;  // field _0: bit 31 of word 0 (srwi rX, rX, 31)
    check(word(states, 0) == 0x80000000 && word(states, 1) == 0, "jumping is bit 31");
    states.jumping = false;
    states._A = true;  // extrwi rX, rX, 1, 10
    check(word(states, 0) == (1u << (31 - 10)), "_A is bit 21");
    states._A = false;
    states._1F = true;
    check(word(states, 0) == 1, "_1F is bit 0");
    states._1F = false;
    states._20 = true;
    check(word(states, 0) == 0 && word(states, 1) == 0x80000000, "_20 is bit 31 of word 1");
    states._20 = false;
    states._23 = true;  // extrwi rX, rX, 1, 3
    check(word(states, 1) == (1u << (31 - 3)), "_23 is bit 28 of word 1");
    states._23 = false;
    states._3E = 3;  // clrrwi rX, rX, 2
    check(word(states, 1) == 3, "_3E is the low two bits of word 1");
}

static void testDrawStates() {
    static_assert(sizeof(Mario::DrawStates) == 4, "DrawStates is one word");
    Mario::DrawStates states;
    std::memset(&states, 0, sizeof(states));

    // Mario sets _1C_WORD |= 0x00100000 / 0x2000 and reads the fields back.
    u32 value = 0x00100000;
    std::memcpy(&states, &value, sizeof(value));
    check(states._B && !states._A && !states._C, "0x00100000 is field _B");
    value = 0x2000;
    std::memcpy(&states, &value, sizeof(value));
    check(states.mIsUnderwater && !states._11 && !states._13, "0x2000 is field 0x12 (mIsUnderwater)");
    std::memset(&states, 0, sizeof(states));
    states._0 = true;
    check(word(states) == 0x80000000, "_0 is bit 31");
    states._0 = false;
    states._1F = true;
    check(word(states) == 1, "_1F is bit 0");
}

static void testJ3DModelXFlags() {
    static_assert(sizeof(J3DModelX::Flags) == 4, "J3DModelX::Flags is one word");
    J3DModelX::Flags flags;
    std::memset(&flags, 0xFF, sizeof(flags));
    flags.clear();
    check(word(flags) == 0, "clear() zeroes every flag");

    // J3DModelX::drawShapePacket calls display list i when (word & (1 << i)).
    flags._1C = true;
    check(word(flags) == (1u << 3), "_1C selects display list 3");
    flags._1C = false;
    flags._10 = true;
    check(word(flags) == (1u << 15), "_10 selects display list 15");
    flags._10 = false;
    flags._0 = true;
    check(word(flags) == 0x80000000, "_0 is bit 31");
}

static void testRushEndInfo() {
    RushEndInfo info(nullptr, 3, TVec3f(0.0f, 0.0f, 0.0f), true, 0);
    check(info._20 == 0, "RushEndInfo flags start at zero");

    // PlayerUtil sets `_20 |= 0xC0000000` and `mFlags.mDamageType = N`; MarioActorRush reads
    // (_20 >> 30) & 1, _20 >> 31 and (_20 >> 24) & 15.
    info._20 |= 0xC0000000;
    info.mFlags.mDamageType = 6;
    check(info._20 == 0xC6000000, "mDamageType occupies bits 27-24");
    check(((info._20 >> 24) & 15) == 6 && ((info._20 >> 30) & 1) == 1 && (info._20 >> 31) == 1, "numeric reads of the flag word");
    check(info.mFlags._0 == 0xC, "_0 occupies bits 31-28");

    info._20 = 0x00800000;  // PlayerUtil: info._20 |= 0x800000
    check(((info._20 >> 23) & 1) == 1 && info.mFlags._8 == 0x800000 && info.mFlags.mDamageType == 0, "_8 occupies bits 23-0");
}

static void testMarioEffectFlags() {
    typedef decltype(static_cast< MarioActor* >(nullptr)->mEffectFlags) EffectFlags;
    static_assert(sizeof(EffectFlags) == 4, "effect flags are one word");
    EffectFlags flags;
    std::memset(&flags, 0, sizeof(flags));
    flags.mSmoke = true;
    check(word(flags) == 0x80000000, "mSmoke is bit 31");
    flags.mSmoke = false;
    flags.mBeeWind = true;
    check(word(flags) == (1u << 26), "mBeeWind is bit 26");
}

int main() {
    testMovementStates();
    testDrawStates();
    testJ3DModelXFlags();
    testRushEndInfo();
    testMarioEffectFlags();

    if (sFailures != 0) {
        std::fprintf(stderr, "%d bitfield layout check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("Bitfield layout tests passed");
    return 0;
}

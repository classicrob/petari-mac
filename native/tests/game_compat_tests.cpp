// Tests for native game compatibility code: MSL adaptor replacements
// (petari/game_compat.hpp), Wii `long` overload forwarding (Game/Util/NativeOverload.hpp),
// Wii-long forwarding in MR::getRandom/clamp, and HashSortTable pointer payloads.
//
// Links: src/Game/Util/HashUtil.cpp only. HashUtil.cpp calls MR::sortSmall from
// MathUtil.cpp, which pulls in most of the game; this file carries an identical copy.
#include <petari/game_compat.hpp>
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/MathUtil.hpp"
#include "Game/Util/NativeOverload.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace MR {
    void sortSmall(s32 length, u32* pSortArray, s32* pIndexArray) {
        for (int i = 0; i < length; i++) {
            pIndexArray[i] = i;
        }

        for (int index = 0; index < length; index++) {
            u32 element = pSortArray[index];
            int indexOfSmallestElement = index;
            for (int i = index + 1; i < length; i++) {
                if (element > pSortArray[i]) {
                    element = pSortArray[i];
                    indexOfSmallestElement = i;
                }
            }

            s32 temp = pIndexArray[index];
            u32 temp2 = pSortArray[index];
            pIndexArray[index] = pIndexArray[indexOfSmallestElement];
            pSortArray[index] = element;
            pIndexArray[indexOfSmallestElement] = temp;
            pSortArray[indexOfSmallestElement] = temp2;
        }
    }
}  // namespace MR

static int sFailures = 0;
static void check(bool condition, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++sFailures;
    }
}

namespace {
    struct Obj {
        int mValue;
        int mHits;
        void inc() { mHits++; }
        void add(int n) { mHits += n; }
        bool isOdd() const { return (mValue & 1) != 0; }
        bool isAbove(int n) const { return mValue > n; }
    };

    bool isNegative(int value) {
        return value < 0;
    }

    int subtract(int a, int b) {
        return a - b;
    }

    // Mirrors the header overload sets: returns which overload was selected.
    int which(s32, s32) {
        return 1;
    }
    int which(f32, f32) {
        return 2;
    }
    template < typename A, typename B, PETARI_WII_LONG_ARGS(A, B) >
    int which(A a, B b) {
        return which(static_cast< s32 >(a), static_cast< s32 >(b)) + 10;
    }
    int one(u32) {
        return 1;
    }
    int one(const char*) {
        return 2;
    }
    template < typename T, PETARI_WII_LONG_ARGS(T) >
    int one(T value) {
        return one(static_cast< u32 >(value)) + 10;
    }
}  // namespace

static void testAdaptors() {
    Obj objs[4] = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
    std::vector< Obj* > ptrs = {&objs[0], &objs[1], &objs[2], &objs[3]};

    std::for_each(ptrs.begin(), ptrs.end(), std::mem_func(&Obj::inc));
    std::for_each(ptrs.begin(), ptrs.end(), std::mem_fun(&Obj::inc));
    std::for_each(ptrs.begin(), ptrs.end(), std::binder2nd< std::mem_fun1_t< void, Obj, int >, int >(std::mem_func(&Obj::add), 2));
    std::for_each_array(objs, objs + 4, std::mem_func(&Obj::inc));
    for (const Obj& obj : objs) {
        check(obj.mHits == 5, "mem_func/mem_fun/binder2nd/for_each_array call counts");
    }

    check(std::find_if_array(objs, objs + 4, std::not1(std::mem_func(&Obj::isOdd))) == &objs[1], "find_if_array + not1");
    check(std::find_if_array(objs, objs + 4, std::mem_func(&Obj::isOdd)) == &objs[0], "find_if_array const mem_func");
    check(std::find_if_array(objs, objs + 4, std::bind2nd(std::mem_func(&Obj::isAbove), 2)) == &objs[2],
          "bind2nd const_mem_fun1_t");
    check(std::find_if(ptrs.begin(), ptrs.end(), std::not1(std::mem_func(&Obj::isOdd))) == ptrs.begin() + 1, "find_if + not1");

    int values[3] = {1, -2, 3};
    check(std::find_if(values, values + 3, std::ptr_fun(isNegative)) == values + 1, "ptr_fun unary");
    check(std::bind2nd(std::ptr_fun(subtract), 5)(7) == 2, "bind2nd ptr_fun binary");
    check(std::bind1st(std::ptr_fun(subtract), 5)(7) == -2, "bind1st ptr_fun binary");

    int mixed[4] = {-4, -3, 2, 1};
    check(std::rfind_if(mixed + 3, mixed, std::ptr_fun(isNegative)) == mixed + 1, "rfind_if searches backwards");
    check(std::rfind_if(mixed + 3, mixed + 1, std::ptr_fun(isNegative)) == mixed + 1, "rfind_if stops at last");
}

static void testOverloads() {
    check(which(0L, 3L) == 11, "(long, long) forwards to s32");
    check(which(0L, 3) == 11, "(long, int) forwards to s32");
    check(which(1, 2) == 1, "(int, int) stays on s32 without forwarding");
    check(which(1.0f, 2.0f) == 2, "(f32, f32) unaffected");
    check(one(0UL) == 11, "0UL forwards to u32 instead of const char*");
    check(one(3) == 1, "int literal stays on u32");
    check(one(static_cast< const char* >(nullptr)) == 2, "pointer overload unaffected");

    check(MR::clamp(25u + 3, 0l, 20l) == 20, "MR::clamp(unsigned, long, long)");
    check(MR::clamp(-4, 0l, 20l) == 0, "MR::clamp(int, long, long)");
    check(MR::clamp(0.5f, 0.0f, 1.0f) == 0.5f, "MR::clamp f32");
}

static void testHashSortTable() {
    static const char* const cNames[] = {"Mario", "Luigi", "Kinopio", "Peach", "Rosetta"};
    Obj objs[5] = {};
    HashSortTable table(8);
    for (int i = 0; i < 5; i++) {
        check(table.addPtr(cNames[i], &objs[i], false), "addPtr");
    }
    check(!table.addPtr("Mario", &objs[1], true), "addPtr skip of duplicate name");
    table.sort();

    for (int i = 0; i < 5; i++) {
        Obj* pFound = nullptr;
        check(table.searchPtr(cNames[i], &pFound) && pFound == &objs[i], "searchPtr returns the full 64-bit pointer");
    }
    Obj* pMissing = &objs[0];
    check(!table.searchPtr("Koopa", &pMissing) && pMissing == nullptr, "searchPtr miss clears the output");

    HashSortTable::Value raw = 0;
    check(table.searchValue(MR::getHashCode("Peach"), &raw) && raw == reinterpret_cast< HashSortTable::Value >(&objs[3]),
          "searchValue by hash");

    HashSortTable indices(4);
    indices.add("b", 2, false);
    indices.add("a", 1, false);
    indices.add("c", 3, false);
    indices.sort();
    u32 index = 0;
    check(indices.search("a", &index) && index == 1, "index payload search");
    check(indices.search("c", &index) && index == 3, "index payload search after sort");
    check(!indices.search("zz", &index) && index == 0, "index search miss clears the output");
    check(indices.search("a", nullptr), "search with null output");
    indices.swap("a", "d");
    indices.sort();
    check(indices.search("d", &index) && index == 1 && !indices.search("a", nullptr), "swap renames the hash");

    check(MR::getHashCode("x") == 'x' && MR::getHashCode("ab") == 'a' * 31 + 'b', "u32 hash values unchanged");
    check(MR::getHashCodeLower("MaRiO") == MR::getHashCode("mario"), "getHashCodeLower ASCII");
    check(MR::getHashCodeLower("\x82\xA0" "A") == MR::getHashCode("\x82\xA0" "a"), "getHashCodeLower leaves non-ASCII bytes");
}

int main() {
    testAdaptors();
    testOverloads();
    testHashSortTable();

    if (sFailures != 0) {
        std::fprintf(stderr, "%d game compat check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("Game compat tests passed");
    return 0;
}

#pragma once

// Overload-resolution helpers for the native (PETARI_NATIVE) build.
//
// On the Wii, `s32` and `u32` are `long` and `unsigned long`, so game code passes
// literals such as `0L` and `1UL` that select the s32/u32 overloads exactly. Natively
// they are `int` and `unsigned int`, which makes those calls ambiguous with f32 or
// pointer overloads. Headers add constrained forwarding templates so that such calls
// resolve to the same overload as on the Wii, without affecting other calls.

#ifdef PETARI_NATIVE
#include <type_traits>

namespace MR {
    namespace Native {
        template < typename T >
        struct IsWiiInt32 : std::integral_constant< bool, std::is_same< T, int >::value || std::is_same< T, long >::value ||
                                                              std::is_same< T, unsigned int >::value ||
                                                              std::is_same< T, unsigned long >::value > {};

        template < typename T >
        struct IsLong : std::integral_constant< bool, std::is_same< T, long >::value || std::is_same< T, unsigned long >::value > {};

        /// True when every argument is a 32-bit Wii integer type and at least one is `long`/`unsigned long`.
        template < typename... T >
        struct IsWiiLongArgs : std::integral_constant< bool, (IsWiiInt32< T >::value && ...) && (IsLong< T >::value || ...) > {};
    }  // namespace Native
}  // namespace MR

/// Enables a forwarding overload only for Wii-style `long` integer arguments.
#define PETARI_WII_LONG_ARGS(...) typename = typename std::enable_if< MR::Native::IsWiiLongArgs< __VA_ARGS__ >::value >::type
#endif

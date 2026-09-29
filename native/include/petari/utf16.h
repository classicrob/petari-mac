#pragma once
// UTF-16 string and formatting functions for legacy wide-text code. Wii builds
// use a 16-bit wchar_t; native builds rewrite those uses to char16_t and call
// these functions instead of the host's 32-bit wide-character library.
//
// Behavior follows MSL's wstring.c and wprintf.c, including the return value of
// vswprintf (-1 when the output does not fit) and MSL's handling of invalid
// conversions (the rest of the format is copied literally). Code units are not
// validated or combined; surrogate pairs pass through unchanged. Data stays in
// host byte order; big-endian resource decoding belongs to resource loaders.

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
typedef char16_t PetariChar16;
extern "C" {
#else
typedef uint_least16_t PetariChar16;
#endif

size_t petari_utf16_wcslen(const PetariChar16* str);
PetariChar16* petari_utf16_wcscpy(PetariChar16* dst, const PetariChar16* src);
PetariChar16* petari_utf16_wcsncpy(PetariChar16* dst, const PetariChar16* src, size_t num);
int petari_utf16_wcscmp(const PetariChar16* str1, const PetariChar16* str2);
PetariChar16* petari_utf16_wcschr(const PetariChar16* str, PetariChar16 chr);

// Supported conversions match MSL: d i u o x X c s n p % and the floating-point
// conversions f F e E g G a A, with flags - + space # 0, width and precision
// (including *), and length modifiers hh h l ll L j z t. Wii `long` is 32 bits,
// so `l` integer and `%p` reads use the Wii widths: `l` reads 32-bit values and
// `%p` prints the full host pointer. %s reads a char string whose bytes widen
// to code units as in MSL's "C" locale; %ls reads a UTF-16 string. Floating-point
// digits come from the host C library, so they can differ from MSL's float2str
// in the last rounded digit.
int petari_utf16_swprintf(PetariChar16* dst, size_t count, const PetariChar16* format, ...);
int petari_utf16_vswprintf(PetariChar16* dst, size_t count, const PetariChar16* format, va_list args);

#ifdef __cplusplus
}
#endif

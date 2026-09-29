#pragma once
// Native replacement for MSL's wstring.h. This maps to the host wide-string
// functions, which use the host wchar_t width. Wii code treats UTF-16 data as
// wchar_t, so this is a declaration shim only, not a UTF-16 implementation.
#include <stddef.h>
#include <wchar.h>

#pragma once
// Native replacement for MSL's extras.h. stricmp is ported from MSL's extras.c
// with the "C" locale: only 'A'-'Z' are lowered, other values are unchanged, and
// the result is -1, 0 or 1.

#ifdef __cplusplus
extern "C" {
#endif

static inline int stricmp(const char* s1, const char* s2) {
    char c1, c2;

    while (1) {
        c1 = *s1++;
        c2 = *s2++;

        if (c1 >= 'A' && c1 <= 'Z') {
            c1 += 'a' - 'A';
        }

        if (c2 >= 'A' && c2 <= 'Z') {
            c2 += 'a' - 'A';
        }

        if (c1 < c2) {
            return -1;
        }

        if (c1 > c2) {
            return 1;
        }

        if (c1 == 0) {
            return 0;
        }
    }
}

#ifdef __cplusplus
}
#endif

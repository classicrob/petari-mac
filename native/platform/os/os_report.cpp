// OSReport / OSVReport / OSPanic on the host: the Wii debug console becomes
// stdout, and a panic prints its location and aborts (PPCHalt equivalent).

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <revolution/os.h>

#include "os_internal.hpp"

extern "C" {

void OSVReport(const char* msg, va_list list) {
    va_list copy;
    va_copy(copy, list);
    std::vprintf(msg, list);
    std::fflush(stdout);
    // A (possibly truncated) copy is kept for crash reports.
    char text[1024];
    std::vsnprintf(text, sizeof(text), msg, copy);
    va_end(copy);
    PetariNative::Platform::OS::recordReport(text);
}

void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    OSVReport(msg, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    char text[512];
    va_list args;
    va_start(args, msg);
    const int n = std::vsnprintf(text, sizeof(text), msg, args);
    va_end(args);
    if (n >= 0 && static_cast<size_t>(n) < sizeof(text)) {
        std::snprintf(text + n, sizeof(text) - n, " in \"%s\" on line %d.", file, line);
    }
    std::fprintf(stderr, "%s\n", text);
    PetariNative::Platform::OS::recordPanic(text);
    std::fflush(nullptr);
    std::abort();
}

}  // extern "C"

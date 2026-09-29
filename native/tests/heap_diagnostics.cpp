// Test executable diagnostics without the game's framebuffer exception viewer.
#include <JSystem/JUtility/JUTException.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

void JUTException::panic_f(const char* file, int line, const char* format, ...) {
    std::fprintf(stderr, "%s:%d: ", file, line);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::abort();
}
extern "C" void JUTWarningConsole_f(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}
extern "C" void JUTReportConsole_f(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
}
extern "C" void JUTReportConsole(const char* text) { std::fputs(text, stdout); }
extern "C" void JUTWarningConsole(const char* text) { std::fputs(text, stderr); }

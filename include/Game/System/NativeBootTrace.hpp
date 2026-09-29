#pragma once

// Native start-up diagnostics, enabled by PETARI_TRACE_BOOT (unset, empty or a leading
// '0' disables, as in GameSystem.cpp and the app). They only report; game flow is
// unchanged. The Wii build compiles NATIVE_TRACE_BOOT to nothing.
#ifdef PETARI_NATIVE
#include <revolution/os.h>
#include <cstdlib>

namespace MR {
    namespace Native {
        inline bool isTraceBoot() {
            static const bool sEnabled = [] {
                const char* pValue = std::getenv("PETARI_TRACE_BOOT");
                return pValue != nullptr && pValue[0] != '\0' && pValue[0] != '0';
            }();
            return sEnabled;
        }

        // True at most once per second per caller-owned timestamp.
        inline bool isTraceHeartbeat(OSTime* pLastReport) {
            OSTime now = OSGetTime();
            if (*pLastReport != 0 && OSTicksToSeconds(now - *pLastReport) < 1) {
                return false;
            }

            *pLastReport = now;
            return true;
        }
    }  // namespace Native
}  // namespace MR

#define NATIVE_TRACE_BOOT(...)                \
    do {                                      \
        if (MR::Native::isTraceBoot()) {      \
            OSReport(__VA_ARGS__);            \
        }                                     \
    } while (0)
#else
#define NATIVE_TRACE_BOOT(...)
#endif

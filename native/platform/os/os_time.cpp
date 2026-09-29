// OS time base on the host.
//
// OSGetTime() counts timer ticks (bus clock / 4 = 60.75 MHz) since
// 2000-01-01 00:00 local time, as the Wii time base does after the IPL sets it
// from the RTC. It advances with the host's monotonic clock. System time
// (__OSGetSystemTime, used by alarms) counts ticks since this process started.

#include <chrono>
#include <ctime>

#include <revolution/os.h>

namespace {

// OS_TIMER_CLOCK: the 243 MHz bus clock / 4.
constexpr s64 kTicksPerSecond = 60750000;
constexpr s64 kUnixTo2000 = 946684800;

struct TimeBase {
    std::chrono::steady_clock::time_point origin;
    OSTime bootTime;  // OSGetTime() value at origin

    TimeBase() {
        origin = std::chrono::steady_clock::now();
        const auto wall = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(wall);
        std::tm local{};
        localtime_r(&seconds, &local);
        const s64 localSeconds = static_cast<s64>(seconds) + local.tm_gmtoff - kUnixTo2000;
        const auto sub = std::chrono::duration_cast<std::chrono::nanoseconds>(wall - std::chrono::system_clock::from_time_t(seconds)).count();
        bootTime = localSeconds * kTicksPerSecond + static_cast<s64>(sub) * 243 / 4000;
    }
};

const TimeBase& timeBase() {
    static const TimeBase instance;
    return instance;
}

s64 ticksSinceOrigin() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - timeBase().origin).count();
    // 60.75 MHz = 243 ticks per 4000 ns.
    return static_cast<s64>(static_cast<__int128>(ns) * 243 / 4000);
}

int YearDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
int LeapYearDays[] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

BOOL IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int GetLeapDays(int year) {
    if (year < 1) {
        return 0;
    }
    return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
}

void GetDates(s32 days, OSCalendarTime* pTime) {
    int year;
    int dayCount;
    int month;
    int* monthArr;

    pTime->wday = (days + 6) % 7;
    for (year = days / 365; days < (dayCount = GetLeapDays(year) + 365 * year); --year) {
    }
    days -= dayCount;
    pTime->year = year;
    pTime->yday = days;
    monthArr = IsLeapYear(year) ? LeapYearDays : YearDays;
    for (month = 12; days < monthArr[--month];) {
    }
    pTime->mon = month;
    pTime->mday = days - monthArr[month] + 1;
}

}  // namespace

namespace PetariNative::Platform::OS {

// Host deadline for a system-time tick value (for the alarm timer).
std::chrono::steady_clock::time_point systemTimeToHost(OSTime systemTime) {
    const s64 ns = static_cast<s64>(static_cast<__int128>(systemTime) * 4000 / 243);
    return timeBase().origin + std::chrono::nanoseconds(ns);
}

}  // namespace PetariNative::Platform::OS

extern "C" {

OSTime OSGetTime(void) {
    return timeBase().bootTime + ticksSinceOrigin();
}

OSTick OSGetTick(void) {
    return static_cast<OSTick>(OSGetTime());
}

OSTime __OSGetSystemTime(void) {
    return ticksSinceOrigin();
}

OSTime __OSTimeToSystemTime(OSTime time) {
    return time - timeBase().bootTime;
}

// Unchanged from src/RVL_SDK/os/OSTime.c.
void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* pTime) {
    int numDays;
    int numSecs;
    OSTime ticksAfter;

    ticksAfter = ticks % OSSecondsToTicks(1);
    if (ticksAfter < 0) {
        ticksAfter += OSSecondsToTicks(1);
    }
    pTime->usec = (int)(OSTicksToMicroseconds(ticksAfter) % 1000);
    pTime->msec = (int)(OSTicksToMilliseconds(ticksAfter) % 1000);
    ticks -= ticksAfter;

    numDays = (int)(OSTicksToSeconds(ticks) / 86400 + 0xB2575);
    numSecs = (int)(OSTicksToSeconds(ticks) % 86400);
    if (numSecs < 0) {
        numDays -= 1;
        numSecs += 86400;
    }
    GetDates(numDays, pTime);
    pTime->hour = numSecs / 60 / 60;
    pTime->min = (numSecs / 60) % 60;
    pTime->sec = numSecs % 60;
}

}  // extern "C"

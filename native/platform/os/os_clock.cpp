// Values of the Wii low-memory clock globals that <revolution/os.h> and
// <revolution/os/OSTime.h> declare extern under PETARI_NATIVE.
//
// OSTime tick conversions keep the console's timebase (bus clock / 4), so game
// code that converts ticks to seconds keeps its Wii meaning. __MEM2End is not
// defined yet: it describes the MEM2 arena, which belongs to the native OS
// arena implementation, and no ported code reads it.

#include <revolution/os.h>

extern "C" {
u32 __OSBusClock = 243000000;
vu32 OS_BUS_CLOCK_SPEED = 243000000;
}

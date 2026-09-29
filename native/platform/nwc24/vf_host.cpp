// Virtual FAT file system (VF) on the host. Replaces src/RVL_SDK/vf/*.c.
//
// On the console VF stores WiiConnect24 mail in a FAT image on NAND. The game
// only calls VFInitEx, handing VF its work memory (NWC24System), and every
// later VF use happens inside the WiiConnect24 library, which is unavailable
// natively (nwc24_host.cpp). VFInitEx therefore does what it does on the
// console before any drive is mounted: it takes the work area, which later
// calls would allocate from. No other VF function is provided, so a use that
// would need a real FAT image fails to link instead of pretending.

#include <revolution/os.h>

namespace {
void* gVfWork;
u32 gVfWorkSize;
}  // namespace

extern "C" {

void VFInitEx(void* heapStart, u32 size) {
    if (heapStart == nullptr || size == 0) {
        OSPanic(__FILE__, __LINE__, "VFInitEx(): no work memory (%p, %u)", heapStart, size);
    }
    gVfWork = heapStart;
    gVfWorkSize = size;
}

}  // extern "C"

namespace PetariNative::Platform::NWC24 {

// VF work area given to VFInitEx (for tests and diagnostics).
void* vfWorkArea(u32* size) {
    if (size) {
        *size = gVfWorkSize;
    }
    return gVfWork;
}

}  // namespace PetariNative::Platform::NWC24

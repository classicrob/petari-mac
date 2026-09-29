#include <revolution/gx.h>
#include <cstring>

void GXCmd1f32(f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    GXCmd1u32(bits);
}

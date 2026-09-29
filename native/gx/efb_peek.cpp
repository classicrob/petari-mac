#include <revolution/gx.h>
#include <revolution/os.h>
#include <atomic>
#include "sync_backend.h"

extern "C" bool petari_gx_read_snapshot(uint64_t ticket, uint16_t x, uint16_t y,
                                       uint32_t* depth, uint32_t* argb);
namespace {
std::atomic<GXAlphaReadMode> alphaRead{GX_READ_FF};
void pixel(u16 x, u16 y, u32& depth, u32& color) {
    const auto ticket = petari_gx_sync_delivering_ticket();
    if (!ticket || !petari_gx_read_snapshot(ticket, x, y, &depth, &color))
        OSPanic(__FILE__, __LINE__, "GX EFB peek at (%u,%u) has no token-time capture (ticket %llu)",
                x, y, static_cast<unsigned long long>(ticket));
}
}
void GXPeekZ(u16 x, u16 y, u32* value) {
    u32 color;
    if (!value) OSPanic(__FILE__, __LINE__, "GXPeekZ: null output");
    pixel(x, y, *value, color);
}
void GXPeekARGB(u16 x, u16 y, u32* value) {
    u32 depth;
    if (!value) OSPanic(__FILE__, __LINE__, "GXPeekARGB: null output");
    pixel(x, y, depth, *value);
    switch (alphaRead.load(std::memory_order_relaxed)) {
    case GX_READ_00: *value &= 0x00ffffff; break;
    case GX_READ_FF: *value |= 0xff000000; break;
    case GX_READ_NONE: break;
    }
}
void GXPokeAlphaRead(GXAlphaReadMode mode) {
    if (mode != GX_READ_00 && mode != GX_READ_FF && mode != GX_READ_NONE)
        OSPanic(__FILE__, __LINE__, "GXPokeAlphaRead: invalid mode");
    alphaRead.store(mode, std::memory_order_relaxed);
}

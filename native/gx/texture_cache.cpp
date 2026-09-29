#include <revolution/gx.h>
#include <revolution/gx/GXTypes.h>
#include <revolution/os.h>
#include <cstring>
#include <cstdlib>

namespace {
u32 cacheExponent(GXTexCacheSize size, bool allowNone) {
    switch (size) {
    case GX_TEXCACHE_32K: return 3;
    case GX_TEXCACHE_128K: return 4;
    case GX_TEXCACHE_512K: return 5;
    case GX_TEXCACHE_NONE: if (allowNone) return 0; break;
    }
    OSPanic(__FILE__, __LINE__, "Invalid texture cache size %u", static_cast<unsigned>(size));
    std::abort();
}
}

// Retain the SDK descriptor for J3D. Metal textures do not use the Wii TMEM cache.
void GXInitTexCacheRegion(GXTexRegion* region, GXBool mipmap32,
                          u32 even, GXTexCacheSize evenSize, u32 odd, GXTexCacheSize oddSize) {
    GXTexRegionInt descriptor{};
    const u32 evenExp = cacheExponent(evenSize, false);
    const u32 oddExp = cacheExponent(oddSize, true);
    descriptor.image1 = ((even >> 5) & 0x7FFF) | (evenExp << 15) | (evenExp << 18);
    descriptor.image2 = ((odd >> 5) & 0x7FFF) | (oddExp << 15) | (oddExp << 18);
    descriptor.is32bMipmap = mipmap32;
    descriptor.isCached = GX_TRUE;
    static_assert(sizeof(descriptor) == sizeof(*region));
    std::memcpy(region, &descriptor, sizeof(descriptor));
}

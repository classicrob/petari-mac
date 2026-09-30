// CPU-written textures: the host store log (native/platform/os/os_cache.cpp) and the
// patched GXLoadTexObj revalidation (native/gx/patch_aurora_texture.py).
//
// On the Wii a texture the CPU rewrites in place and stores (DCStoreRange) shows the
// new texels at its next draw. Aurora uploads once per texture object and data
// version, so without the store log SnowFloor, fur density, normal-map and Mario's
// dissolve-mask textures kept their first upload.
//
// Both run on game threads (GXLoadTexObj on the render path, DCStoreRange anywhere), so
// their bookkeeping must stay off the game's JKR heaps: an unscoped std container there
// took the logo's tiny system heap and aborted every run. The tests run on a registered
// game thread with a 64 KiB current heap and the allocation-site routing off, so only
// the explicit HostAllocationScopes keep the heap untouched.
//
// Usage: petari_texture_store_tests
#include "JSystem/JKernel/JKRExpHeap.hpp"
#include <petari/boot.hpp>
#include <petari/host_allocation.hpp>
#include <revolution/gx.h>
#include <revolution/os.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" std::uint64_t petari_dc_store_generation(void);
extern "C" int petari_dc_stored_since(const void* addr, std::size_t nBytes, std::uint64_t since);
extern "C" bool petari_gx_revalidate_texobj(GXTexObj* obj);

static int sFailures = 0;
static void check(bool condition, const char* pText) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", pText);
        ++sFailures;
    }
}

// Separate 4 KiB pages, so stores to one never touch another.
alignas(4096) static u8 sTexels[4096];
alignas(4096) static u8 sOther[4096];
alignas(4096) static u8 sSecond[4096];

static void testStoreLog() {
    const std::uint64_t before = petari_dc_store_generation();
    check(!petari_dc_stored_since(sTexels, 64, before), "nothing stored yet since the current generation");
    DCStoreRange(sTexels + 32, 16);
    check(petari_dc_store_generation() > before, "a store advances the generation");
    check(petari_dc_stored_since(sTexels, 64, before) != 0, "a store inside the range is seen");
    check(petari_dc_stored_since(sTexels + 4000, 8, before) != 0, "stores are tracked by page (same 4 KiB page)");
    check(!petari_dc_stored_since(sOther, 64, before), "a store on another page is not seen");
    check(!petari_dc_stored_since(sTexels, 64, petari_dc_store_generation()), "nothing is newer than the current generation");
    check(!petari_dc_stored_since(nullptr, 64, 0) && !petari_dc_stored_since(sTexels, 0, 0), "empty ranges are never stored");
}

static void testTextureRevalidation() {
    GXTexObj obj;
    GXInitTexObj(&obj, sTexels, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    check(!petari_gx_revalidate_texobj(&obj), "a new texture object is not stale");
    check(!petari_gx_revalidate_texobj(&obj), "no store since the last load: not stale");

    DCStoreRange(sOther, 64);
    check(!petari_gx_revalidate_texobj(&obj), "a store elsewhere does not touch the texture");

    std::memset(sTexels, 0xF0, 64);
    DCStoreRange(sTexels, 64);
    check(petari_gx_revalidate_texobj(&obj), "texels stored since the last load: stale (SnowFloor::createTexture)");
    check(!petari_gx_revalidate_texobj(&obj), "stale only once per store");

    DCFlushRangeNoSync(sTexels + 8, 8);
    check(petari_gx_revalidate_texobj(&obj), "DCFlushRangeNoSync counts as a store");
    DCZeroRange(sTexels, 32);
    check(petari_gx_revalidate_texobj(&obj), "DCZeroRange counts as a store");

    // A second object initialised later over the same memory starts fresh.
    GXTexObj second;
    GXInitTexObj(&second, sTexels, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    check(!petari_gx_revalidate_texobj(&second), "a newer texture object over stored memory is not stale");

    // Objects created together and first loaded out of creation order (layout text does
    // this every few frames) have no earlier upload, so none of them is stale.
    GXTexObj batch[3];
    for (GXTexObj& rObj : batch) {
        GXInitTexObj(&rObj, sTexels, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    }
    DCStoreRange(sTexels, 64);
    check(!petari_gx_revalidate_texobj(&batch[2]) && !petari_gx_revalidate_texobj(&batch[0]) && !petari_gx_revalidate_texobj(&batch[1]),
          "first loads in any order are not stale");

    // Double buffering (SnowFloor keeps two textures and alternates): each sees its own stores.
    GXTexObj other;
    GXInitTexObj(&other, sSecond, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    petari_gx_revalidate_texobj(&other);
    petari_gx_revalidate_texobj(&obj);
    DCStoreRange(sSecond, 64);
    check(!petari_gx_revalidate_texobj(&obj), "the other buffer's store does not mark this one");
    check(petari_gx_revalidate_texobj(&other), "the rewritten buffer is stale");
}

// Thousands of texture objects and stored pages grow both bookkeeping maps well past
// the 64 KiB current heap; with a scope missing that aborts ("Native heap allocation
// failed") or at least takes game-heap memory.
static void testGameThreadBookkeeping(JKRHeap* pTiny) {
    const s32 before = pTiny->getTotalFreeSize();
    static u8 sPages[4096 * 256];
    static GXTexObj sObjects[4096];
    for (int i = 0; i < 4096; i++) {
        GXInitTexObj(&sObjects[i], sPages + (i % 256) * 4096, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        petari_gx_revalidate_texobj(&sObjects[i]);
        DCStoreRange(sPages + (i % 256) * 4096, 64);
    }
    for (int i = 0; i < 4096; i++) {
        petari_gx_revalidate_texobj(&sObjects[i]);
    }
    check(pTiny->getTotalFreeSize() == before, "texture and store bookkeeping on a game thread leave the current JKR heap untouched");
}

int main() {
    OSInit();
    PetariNative::setGameAllocationThread(true);
    JKRExpHeap* pRoot = JKRExpHeap::createRoot(1, false);
    pRoot->becomeCurrentHeap();
    pRoot->becomeSystemHeap();
    JKRExpHeap* pTiny = JKRExpHeap::create(64 * 1024, pRoot, false);
    pTiny->becomeCurrentHeap();
    PetariNative::setAllocationSiteCheck(false);

    const s32 tinyFree = pTiny->getTotalFreeSize();
    testGameThreadBookkeeping(pTiny);  // first: the maps are created under the tiny heap
    testStoreLog();
    testTextureRevalidation();
    check(pTiny->getTotalFreeSize() == tinyFree, "no test allocated from the current JKR heap");
    PetariNative::setAllocationSiteCheck(true);
    if (sFailures != 0) {
        std::fprintf(stderr, "%d texture store check(s) failed\n", sFailures);
        return 1;
    }
    std::puts("texture store tests passed");
    return 0;
}

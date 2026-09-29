#include <revolution/gx.h>
#include <revolution/os.h>
#include <atomic>
#include <cstdio>

namespace {
std::atomic<unsigned> callbacks{0};
std::atomic<bool> valid{true};
void onToken(u16 token) {
    const auto index = callbacks.load(std::memory_order_relaxed);
    u32 depth = 0, color = 0;
    GXPeekZ(320, 240, &depth);
    GXPokeAlphaRead(GX_READ_NONE);
    GXPeekARGB(320, 240, &color);
    if (token != 0x1234 + index || depth > 0xffffff)
        valid.store(false, std::memory_order_relaxed);
    callbacks.fetch_add(1, std::memory_order_release);
}
}
extern "C" bool petari_probe_check_sync() {
    callbacks.store(0);
    valid.store(true);
    const auto previous = GXSetDrawSyncCallback(onToken);
    GXSetDrawSync(0x1234);
    GXSetDrawSync(0x1235);
    GXDrawDone();
    GXSetDrawSyncCallback(previous);
    const bool passed = callbacks.load(std::memory_order_acquire) == 2 && valid.load();
    std::fprintf(stderr, "Native token-time EFB callbacks before end_frame: %s (%u callbacks)\n",
                 passed ? "PASS" : "FAIL", callbacks.load());
    return passed;
}

#include <petari/boot.hpp>
#include <petari/host_allocation.hpp>
#include <petari/platform/vi.hpp>
#include <revolution/vi.h>

extern "C" void petari_gx_install_snapshots();
extern "C" void petari_backend_configure_vi(const void* mode);

namespace {
void configure(const GXRenderModeObj* mode, void*) {
    PetariNative::HostAllocationScope host;
    petari_backend_configure_vi(mode);
}
}

extern "C" void petari_attach_vi_renderer() {
    petari_gx_install_snapshots();
    PetariNative::Platform::VI::setHooks({configure, nullptr, nullptr});
}

extern "C" void petari_probe_init_vi() {
    OSInit();
    petari_attach_vi_renderer();
    VIInit();
    GXRenderModeObj mode{};
    mode.viTVmode = VI_TVMODE_NTSC_PROG;
    mode.fbWidth = mode.viWidth = 640;
    mode.efbHeight = mode.xfbHeight = mode.viHeight = 480;
    mode.xFBmode = VI_XFBMODE_SF;
    VIConfigure(&mode);
    VISetBlack(FALSE);
    VIFlush();
}

extern "C" unsigned petari_probe_wait_vi() {
    VIWaitForRetrace();
    return VIGetRetraceCount();
}

extern "C" void petari_shutdown_vi_renderer() {
    PetariNative::Platform::VI::shutdown();
    PetariNative::Platform::VI::setHooks({});
}

#pragma once
// The native application: launch and the per-frame host seam.
//
// The game runs on the macOS main thread, which is also Aurora's and SDL's
// thread (see native/app/INTEGRATION_PLAN.md). GameSystem::frameLoop calls
// petari_host_frame_seam() once per frame, after endFrame() (the frame's
// GXCopyDisp was issued and its GXDrawDone returned) and before
// waitForRetrace(). The seam draws the host overlays, ends the Aurora frame,
// handles window and input events, and opens the next Aurora frame, so an
// Aurora frame is open whenever game code runs.
//
// This header includes neither SDK nor Aurora headers.

extern "C" void petari_host_frame_seam(void);

namespace PetariNative::App {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Presentation, implemented by the renderer (root, native/gx). Both hooks run
// on the main thread under a HostAllocationScope, inside the open Aurora
// frame, before the overlays and aurora_end_frame().
struct PresentHooks {
    // Makes the window show this frame as the console would: the image the
    // game copied with GXCopyDisp, blanked while VI output is black, and
    // darkened while VI dimming is on. Without it the window shows whatever
    // Aurora presents, which the app reports once at start-up.
    void (*composeFrame)(void* user) = nullptr;
    // The game image in window points (the units of SDL mouse events and of
    // ImGui's display). Returns false while no image is shown. Without it the
    // app maps the pointer and overlays to the whole window and says so once.
    bool (*imageRect)(Rect* rect, void* user) = nullptr;
    void* user = nullptr;
};

// Install before the first frame opens, on the main thread. The app calls the
// renderer's petari_attach_vi_renderer() during start-up, just before, which
// is the renderer's place to install these.
void setPresentHooks(const PresentHooks& hooks);

// Releasing the OS CPU baton around the seam's host work (Aurora frame end,
// present, event waits), so audio, loaders and DrawSyncManager keep running.
// The OS layer provides the pair; the app binds them. Without them the seam
// holds the CPU and the app reports that once.
struct CpuRelease {
    void (*begin)() = nullptr;  // the calling OS thread gives up the CPU
    void (*end)() = nullptr;    // it waits for the CPU again
};
// Install before the first frame opens, on the main thread.
void setCpuRelease(const CpuRelease& release);

}  // namespace PetariNative::App

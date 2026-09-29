#pragma once
// Native VI (video interface): retrace timing, retrace callbacks, and the
// display state a renderer presents.
//
// Game code uses the SDK VI API (<revolution/vi.h>). As on the Wii:
// - VISetNextFrameBuffer, VISetBlack, VIConfigure, VIConfigurePan, and
//   VISetTrapFilter change pending state;
// - VIFlush arms it;
// - the next retrace latches it, between the pre- and post-retrace
//   callbacks.
// A retrace is an interrupt: it runs on a host thread with interrupts
// disabled, increments VIGetRetraceCount, calls the pre-retrace callback,
// latches flushed state, calls the post-retrace callback, and wakes threads
// sleeping in VIWaitForRetrace.
//
// Renderer contract (the root bridge in native/gx implements the hooks):
// - onConfigure runs on the game thread inside VIConfigure, after the new
//   render mode is recorded, with the caller's interrupt state (normally
//   enabled). It may request a window or framebuffer resize. It must not
//   block on OS primitives or the retrace.
// - onLatched runs on the retrace interrupt thread, with interrupts
//   disabled, each time a retrace latches flushed state. It must only record
//   or signal (for example, store the state for the render thread). It must
//   not call WebGPU/Aurora, block, or call blocking OS functions.
// - The render thread presents whatever displayState() reports: the latched
//   frameBuffer, or black. It never runs on the retrace thread.
// The platform library has no Aurora dependency.

#include <cstdint>

#include <revolution/gx/GXStruct.h>

namespace PetariNative::Platform::VI {

struct DisplayState {
    void* frameBuffer = nullptr;   // XFB latched for display (VIGetCurrentFrameBuffer)
    bool black = true;             // VI output blanked; VIInit starts black, as on the Wii
    bool configured = false;       // a render mode has been latched
    GXRenderModeObj renderMode{};  // latched render mode (valid when configured)
    std::uint32_t tvFormat = 0;    // VI_NTSC, VI_PAL, VI_MPAL, VI_EURGB60 (VIGetTvFormat)
    std::uint32_t scanMode = 0;    // VI_INTERLACE, VI_NON_INTERLACE, VI_PROGRESSIVE
    std::uint16_t panX = 0, panY = 0, panWidth = 0, panHeight = 0;
    bool trapFilter = false;
    bool dimmed = false;           // screen-saver dimming active (renderer may darken)
    std::uint32_t retraceCount = 0;
    double fieldRate = 0.0;        // retraces per second for the latched mode
};

struct Hooks {
    void (*onConfigure)(const GXRenderModeObj* renderMode, void* user) = nullptr;
    void (*onLatched)(const DisplayState& state, void* user) = nullptr;
    void* user = nullptr;
};

// Installs renderer hooks. May be called at any time; pass {} to remove.
void setHooks(const Hooks& hooks);

// Latched state as of the most recent retrace. Safe from any thread.
DisplayState displayState();

// Where retraces come from. Choose before VIInit().
enum class Clock {
    // A VI host thread raises retraces at the latched mode's field rate:
    // 60000/1001 Hz for NTSC, MPAL, EuRGB60 and progressive modes; 50 Hz for PAL.
    Internal,
    // No VI thread. signalRetrace() raises each retrace, for a display link
    // or a deterministic test clock.
    External,
};
void setClock(Clock clock);

// Raises one retrace interrupt on the calling host thread. The caller must
// not be an OS thread (OS threads cannot be interrupt handlers). Aborts
// unless the clock is External and VIInit() has run.
void signalRetrace();

// TV mode (VITVMode) the console "booted" in, latched by VIInit(). Defaults to
// VI_TVMODE_NTSC_INT; PAL discs need VI_TVMODE_PAL_INT or EuRGB60.
void setBootTvMode(std::uint32_t viTvMode);

// Field rate in Hz for a VITVMode.
double fieldRateFor(std::uint32_t viTvMode);

// Stops the VI thread and returns VI to its pre-VIInit state. For tests and
// shutdown. Must not be called from the retrace thread.
void shutdown();

}  // namespace PetariNative::Platform::VI

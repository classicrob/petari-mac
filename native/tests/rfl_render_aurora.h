#pragma once
// Boundary between the RFL render test's two halves, which cannot share
// headers: rfl_render_tests.cpp (the SDK/game side: RVL GX, RFL, JKR heaps)
// and rfl_render_aurora.cpp (Aurora, WebGPU, and the EFB snapshot reader).
// Plain C types only.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The test body (SDK side). Aurora's entry point calls it after main.
int rfl_render_main(int argc, char** argv);

// Aurora side.
bool rfl_render_initialize(int argc, char** argv);  // Aurora with a Metal window
bool rfl_render_begin_frame(void);                   // false when the window closes
void rfl_render_end_frame(void);
void rfl_render_shutdown(void);

typedef struct RflRenderStats {
    uint32_t differentColorPixels;  // EFB pixels whose color differs from pixel (0, 0)
    uint32_t writtenDepthPixels;    // EFB pixels whose depth is not the clear value
} RflRenderStats;

// Captures the EFB mid-frame (everything drawn so far this frame) and waits
// for the GPU readback. Call between begin and end frame.
bool rfl_render_capture(uint64_t ticket, RflRenderStats* stats);
// ARGB of an EFB pixel in a completed capture.
bool rfl_render_pixel(uint64_t ticket, uint16_t x, uint16_t y, uint32_t* argb);
void rfl_render_retire(uint64_t ticket);

#ifdef __cplusplus
}
#endif

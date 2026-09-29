// Public GX synchronisation entry points with the SDK's ABI (revolution/gx
// headers): draw sync tokens, draw done, FIFO breakpoints, abort, FIFO
// objects and pointers, GP status. They write the SDK's BP commands into
// Aurora's FIFO and use petari/platform/gx_sync.hpp for interrupts, waits and
// state. Aurora is reached only through the C ABI in sync_backend.h; the
// patched Aurora sources no longer define these functions
// (patch_aurora_sync.py).

#include <cstring>

#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXManage.h>
#include <revolution/os.h>

#include "petari/platform/gx_sync.hpp"
#include "sync_backend.h"

namespace GXSync = PetariNative::Platform::GXSync;

extern "C" {
// Aurora's raw command writers (GXVert.cpp), declared here because the SDK
// headers do not.
void GXCmd1u8(const u8 x);
void GXCmd1u32(const u32 x);
u16 GXReadDrawSync(void);
void GXWaitDrawDone(void);
}

namespace {

constexpr u32 kDrawDoneBp = 0x45000002;  // SDK GXSetDrawDone
constexpr u32 kTokenInterruptBp = 0x48000000;
constexpr u32 kTokenBp = 0x47000000;

// Last draw done issued by this game, for GXWaitDrawDone.
std::uint64_t gLastDrawDone = 0;

void writeBp(u32 value) {
    GXCmd1u8(0x61);
    GXCmd1u32(value);
}

GXBool copyFifo(const void* source, GXFifoObj* out) {
    if (source == nullptr) {
        return GX_FALSE;
    }
    std::memcpy(out, source, sizeof(GXFifoObj));  // both definitions are u8 pad[128]
    return GX_TRUE;
}

}  // namespace

extern "C" {

// ---- Draw sync (PE token) ----

void GXSetDrawSync(u16 token) {
    // SDK order: token interrupt BP, then the token register BP.
    BOOL enabled = OSDisableInterrupts();
    writeBp(kTokenInterruptBp | token);
    writeBp(kTokenBp | token);
    GXFlush();
    OSRestoreInterrupts(enabled);
    petari_aurora_fifo_publish();
}

u16 GXReadDrawSync(void) {
    return GXSync::lastToken();
}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback callback) {
    return GXSync::setDrawSyncCallback(callback);
}

// ---- Draw done (PE finish) ----

void GXSetDrawDone(void) {
    BOOL enabled = OSDisableInterrupts();
    writeBp(kDrawDoneBp);
    GXFlush();
    gLastDrawDone = GXSync::noteDrawDoneIssued();
    OSRestoreInterrupts(enabled);
    // Aurora processes commands only inside a frame, except that GXDrawDone
    // publishes everything (as its fifo::drain did). A draw done written
    // while a GXDrawDone waits (handleGXAbortAlarm's recovery, from interrupt
    // context) must be processable too, or that wait never ends.
    if (GXSync::drawDoneWaitPending()) {
        petari_aurora_fifo_publish_all();
    } else {
        petari_aurora_fifo_publish();
    }
}

void GXWaitDrawDone(void) {
    GXSync::waitDrawDone(gLastDrawDone);
}

void GXDrawDone(void) {
    BOOL enabled = OSDisableInterrupts();
    writeBp(kDrawDoneBp);
    GXFlush();
    const std::uint64_t count = gLastDrawDone = GXSync::noteDrawDoneIssued();
    OSRestoreInterrupts(enabled);
    petari_aurora_fifo_publish_all();
    GXSync::waitDrawDone(count);  // yields the OS CPU baton
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback callback) {
    return GXSync::setDrawDoneCallback(callback);
}

// ---- FIFO breakpoints ----

void GXEnableBreakPt(void* breakPoint) {
    GXSync::setBreakpoint(GXSync::pointerToPosition(breakPoint));
    petari_aurora_fifo_wake();
}

void GXDisableBreakPt(void) {
    GXSync::clearBreakpoint();
    petari_aurora_fifo_wake();
}

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback callback) {
    return GXSync::setBreakpointCallback(callback);
}

// ---- Abort ----

// Discards every command written and not yet processed, clears the
// breakpoint, and keeps the game's draw-done waits consistent (see
// GXSync::abortFrame). Callable from interrupt context. Deviation: the SDK
// also drops pending dirty shadow state (gx->dirtyState = 0); Aurora's shadow
// state is kept and sent with the next draw.
void GXAbortFrame(void) {
    GXSync::abortFrame();
    petari_aurora_fifo_abort();
}

// ---- FIFO objects, pointers, status ----

GXBool GXGetCPUFifo(GXFifoObj* fifo) {
    return copyFifo(petari_aurora_cpu_fifo(), fifo);
}

GXBool GXGetGPFifo(GXFifoObj* fifo) {
    return copyFifo(petari_aurora_gp_fifo(), fifo);
}

// There is one stream, so the object is not consulted. The pointers are
// opaque stream positions (GXSync::positionToPointer), never dereferenced:
// read = processed, write = everything written so far.
void GXGetFifoPtrs(const GXFifoObj*, void** readPtr, void** writePtr) {
    *readPtr = GXSync::positionToPointer(petari_aurora_fifo_processed_position());
    *writePtr = GXSync::positionToPointer(petari_aurora_fifo_write_position());
}

// Watermark flags (overhi, underlow) are not modelled: the stream grows.
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    const GXSync::GPStatus status = GXSync::gpStatus();
    *overhi = GX_FALSE;
    *underlow = GX_FALSE;
    *readIdle = status.readIdle ? GX_TRUE : GX_FALSE;
    *cmdIdle = status.commandIdle ? GX_TRUE : GX_FALSE;
    *brkpt = status.breakpoint ? GX_TRUE : GX_FALSE;
}

}  // extern "C"

// Processor-side GX synchronisation for the patched Aurora FIFO, and the
// renderer's snapshot contract, on top of petari/platform/gx_sync.hpp. See
// sync_backend.h. No Aurora headers here.

#include "sync_backend.h"

#include <revolution/os.h>

#include "petari/platform/gx_sync.hpp"

namespace GXSync = PetariNative::Platform::GXSync;

namespace {

// BP registers (SDK GXMisc.c: GXSetDrawDone, GXSetDrawSync).
constexpr std::uint32_t kBpDrawDone = 0x45;
constexpr std::uint32_t kBpToken = 0x47;
constexpr std::uint32_t kBpTokenInterrupt = 0x48;

}  // namespace

extern "C" {

uint64_t petari_gx_sync_process_limit(uint64_t processed, uint64_t target) {
    return GXSync::processLimit(processed, target);
}

void petari_gx_sync_processed(uint64_t processed, uint64_t published, uint32_t sync_bp) {
    // Events are reported before progress, so a draw-done waiter that sees
    // the progress never misses its event.
    switch (sync_bp >> 24) {
    case 0:
        break;
    case kBpDrawDone:
        GXSync::reportDrawDone(processed);
        break;
    case kBpToken:
        GXSync::setTokenRegister(static_cast<std::uint16_t>(sync_bp & 0xFFFF));
        break;
    case kBpTokenInterrupt:
        GXSync::reportToken(static_cast<std::uint16_t>(sync_bp & 0xFFFF), processed);
        break;
    default:
        OSPanic(__FILE__, __LINE__, "GX sync: BP 0x%08x is not a synchronisation register", sync_bp);
    }
    GXSync::reportProgress(processed, published);
}

void petari_gx_sync_abort_applied(uint64_t processed, uint64_t published) {
    GXSync::abortApplied(processed, published);
}

void petari_gx_sync_wait_processed(uint64_t position) {
    GXSync::waitProcessed(position);
}

void petari_gx_sync_set_snapshot_hook(PetariGXSnapshotHook hook, void* user) {
    GXSync::setSnapshotHook(hook, user);
}

void petari_gx_sync_snapshot_ready(uint64_t ticket) {
    GXSync::snapshotReady(ticket);
}

void petari_gx_sync_set_flush_hook(PetariGXFlushHook hook, void* user) {
    GXSync::setFlushHook(hook, user);
}

uint64_t petari_gx_sync_delivering_ticket(void) {
    return GXSync::deliveringTicket();
}

uint64_t petari_gx_sync_held_ticket(void) {
    return GXSync::heldTicket();
}

uint64_t petari_gx_sync_delivered_ticket(void) {
    return GXSync::deliveredTicket();
}

}  // extern "C"

// Tests for the public GX synchronisation wrappers (native/gx/sync_bridge.cpp)
// and the processor-side C ABI (native/gx/sync_backend.cpp) against a fake
// Aurora FIFO. The fake implements the petari_aurora_* ABI and runs a
// processor thread with the same loop patch_aurora_sync.py gives Aurora's
// fifo.cpp: breakpoint limit, stop at synchronisation BPs, report outside the
// buffer lock, abort discard, drain through the platform wait.

#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXManage.h>
#include <revolution/gx/GXPerf.h>
#include <revolution/os.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../gx/sync_backend.h"
#include "petari/platform/gx_sync.hpp"

extern "C" {
void __OSThreadInit(void);
void GXCmd1u8(const u8 x);
void GXCmd1u32(const u32 x);
u16 GXReadDrawSync(void);
void GXWaitDrawDone(void);
}

namespace GXS = PetariNative::Platform::GXSync;

namespace {

int checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::exit(1);
    }
}

template <class Predicate>
bool eventually(Predicate predicate, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// ---- Fake Aurora FIFO ----
// Stream commands: 0x61 + u32 BP write; 0x90 + u32 n + n payload bytes (a
// "draw" that takes gDrawDelay to process).

constexpr u8 kDraw = 0x90;

std::mutex gBufferMutex;
std::vector<u8> gBuffer;  // whole stream, never compacted
std::atomic<std::uint64_t> gPublished{0};
std::atomic<std::uint64_t> gProcessed{0};
std::atomic<std::uint64_t> gAbortTo{0};
std::atomic<bool> gAbortRequested{false};
std::atomic<std::uint32_t> gWake{0};
std::atomic<bool> gStop{false};
std::atomic<bool> gFrameActive{true};
std::atomic<int> gDrawDelayUs{0};
std::thread gWorker;
GXFifoObj gCpuFifo;
bool gCpuFifoSet = false;

// Processed commands (processor thread) and GP interrupt callbacks (GP
// interrupt thread), each in its own order.
std::mutex gTraceLock;
std::vector<std::string> gTrace;
std::vector<std::string> gCallbacks;
void trace(const std::string& s) {
    std::lock_guard<std::mutex> g(gTraceLock);
    gTrace.push_back(s);
}
void traceCallback(const std::string& s) {
    std::lock_guard<std::mutex> g(gTraceLock);
    gCallbacks.push_back(s);
}
std::vector<std::string> takeTrace() {
    std::lock_guard<std::mutex> g(gTraceLock);
    std::vector<std::string> out;
    out.swap(gTrace);
    return out;
}
std::vector<std::string> takeCallbacks() {
    std::lock_guard<std::mutex> g(gTraceLock);
    std::vector<std::string> out;
    out.swap(gCallbacks);
    return out;
}

void storeMax(std::atomic<std::uint64_t>& value, std::uint64_t candidate) {
    std::uint64_t current = value.load();
    while (current < candidate && !value.compare_exchange_weak(current, candidate)) {
    }
}

std::mutex gWakeLock;
std::condition_variable gWakeChanged;
void wakeWorker() {
    std::lock_guard<std::mutex> g(gWakeLock);
    gWake.fetch_add(1);
    gWakeChanged.notify_all();
}

std::uint32_t readU32(const u8* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// Aurora's process(): up to and including the next synchronisation BP.
std::uint32_t process(const u8* data, std::uint32_t size, std::uint32_t& syncBp) {
    std::uint32_t offset = 0;
    while (offset < size) {
        if (offset != 0 && gAbortRequested.load()) {
            return offset;
        }
        const u8 cmd = data[offset++];
        const std::uint32_t value = readU32(data + offset);
        offset += 4;
        if (cmd == 0x61) {
            const std::uint32_t reg = value >> 24;
            trace("bp" + std::to_string(reg) + ":" + std::to_string(value & 0xFFFF));
            if (reg == 0x45 || reg == 0x47 || reg == 0x48) {
                syncBp = value;
                return offset;
            }
        } else if (cmd == kDraw) {
            offset += value;
            trace("draw" + std::to_string(value));
            if (int us = gDrawDelayUs.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(us));
            }
        } else {
            std::fprintf(stderr, "bad fake command %02x\n", cmd);
            std::abort();
        }
    }
    return offset;
}

// The patched fifo.cpp process_to (with command_processor.cpp's early stop
// on abort).
void processTo(std::uint64_t target) {
    std::uint64_t processed = gProcessed.load();
    while (processed < target) {
        if (const std::uint64_t abortTo = gAbortTo.load(); abortTo > processed) {
            gAbortRequested = false;
            processed = abortTo;
            gProcessed.store(processed);
            petari_gx_sync_abort_applied(processed, gPublished.load());
            continue;
        }
        const std::uint64_t limit = petari_gx_sync_process_limit(processed, target);
        if (limit == processed) {
            return;
        }
        std::uint32_t syncBp = 0;
        std::uint32_t bytes;
        std::vector<u8> copy;
        {
            // Aurora decodes under sBufferMutex, which its writers take only
            // to grow the buffer; the fake copies so its slow "draws" never
            // block writers.
            std::lock_guard<std::mutex> lock(gBufferMutex);
            copy.assign(gBuffer.begin() + processed, gBuffer.begin() + limit);
        }
        bytes = process(copy.data(), static_cast<std::uint32_t>(copy.size()), syncBp);
        processed += bytes;
        gProcessed.store(processed);
        petari_gx_sync_processed(processed, gPublished.load(), syncBp);
    }
}

// The patched fifo.cpp worker_main.
void workerMain() {
    while (true) {
        const std::uint32_t event = gWake.load();
        const std::uint64_t processed = gProcessed.load();
        const std::uint64_t published = gPublished.load();
        if (published != processed) {
            processTo(published);
            if (gProcessed.load() != processed) {
                continue;
            }
        }
        if (gStop.load()) {
            return;
        }
        std::unique_lock<std::mutex> lock(gWakeLock);
        gWakeChanged.wait(lock, [&] { return gWake.load() != event; });
    }
}

void startFifo() {
    gStop = false;
    gWorker = std::thread(workerMain);
    std::memset(&gCpuFifo, 0x5A, sizeof(gCpuFifo));
    gCpuFifoSet = true;
}

void stopFifo() {
    gStop = true;
    wakeWorker();
    gWorker.join();
}

// The patched fifo::drain (aurora_end_frame).
void drain() {
    const std::uint64_t target = petari_aurora_fifo_write_position();
    storeMax(gPublished, target);
    wakeWorker();
    petari_gx_sync_wait_processed(target);
}

void writeDraw(std::uint32_t bytes = 16) {
    GXCmd1u8(kDraw);
    GXCmd1u32(bytes);
    std::lock_guard<std::mutex> lock(gBufferMutex);
    gBuffer.insert(gBuffer.end(), bytes, 0);
}

}  // namespace

// ---- Aurora ABI provided by the fake ----
extern "C" {
void GXCmd1u8(const u8 x) {
    std::lock_guard<std::mutex> lock(gBufferMutex);
    gBuffer.push_back(x);
}
void GXCmd1u32(const u32 x) {
    std::lock_guard<std::mutex> lock(gBufferMutex);
    for (int shift = 24; shift >= 0; shift -= 8) {
        gBuffer.push_back(static_cast<u8>(x >> shift));
    }
}
void GXFlush(void) {}

uint64_t petari_aurora_fifo_write_position(void) {
    std::lock_guard<std::mutex> lock(gBufferMutex);
    return gBuffer.size();
}
uint64_t petari_aurora_fifo_processed_position(void) {
    return gProcessed.load();
}
void petari_aurora_fifo_publish(void) {
    if (gFrameActive) {
        storeMax(gPublished, petari_aurora_fifo_write_position());
        wakeWorker();
    }
}
void petari_aurora_fifo_publish_all(void) {
    storeMax(gPublished, petari_aurora_fifo_write_position());
    wakeWorker();
}
void petari_aurora_fifo_abort(void) {
    const std::uint64_t written = petari_aurora_fifo_write_position();
    storeMax(gPublished, written);
    storeMax(gAbortTo, written);
    gAbortRequested = true;
    wakeWorker();
}
void petari_aurora_fifo_wake(void) {
    wakeWorker();
}
const void* petari_aurora_cpu_fifo(void) {
    return gCpuFifoSet ? &gCpuFifo : nullptr;
}
const void* petari_aurora_gp_fifo(void) {
    return nullptr;
}
}

namespace {

// ---- Tests ----

void tokenCallback(u16 token) {
    traceCallback("token" + std::to_string(token) + (OSDisableInterrupts() == FALSE ? "" : "!interrupts-enabled"));
}
void drawDoneCallback() {
    traceCallback("done");
}

void testFifoObjectsAndPointers() {
    GXFifoObj fifo;
    std::memset(&fifo, 0, sizeof(fifo));
    check(GXGetCPUFifo(&fifo) == GX_TRUE && fifo.pad[0] == 0x5A && fifo.pad[127] == 0x5A,
          "GXGetCPUFifo(GXFifoObj*) copies the current CPU FIFO and returns TRUE");
    check(GXGetGPFifo(&fifo) == GX_FALSE, "GXGetGPFifo returns FALSE without a GP FIFO");
    void* read;
    void* write;
    GXGetFifoPtrs(&fifo, &read, &write);
    check(reinterpret_cast<uintptr_t>(write) >= 0x80000000u && reinterpret_cast<uintptr_t>(read) >= 0x80000000u,
          "FIFO pointers are above DrawSyncManager's token range");
    writeDraw();
    void* write2;
    GXGetFifoPtrs(&fifo, &read, &write2);
    check(GXS::pointerToPosition(write2) == GXS::pointerToPosition(write) + 5 + 16, "write pointer advances by bytes written");
}

void testTokensAndDrawDone() {
    GXSetDrawSyncCallback(tokenCallback);
    GXSetDrawDoneCallback(drawDoneCallback);
    takeCallbacks();
    writeDraw();
    GXSetDrawSync(7);
    writeDraw();
    GXDrawDone();
    check((takeTrace() == std::vector<std::string>{"draw16", "draw16", "bp72:7", "bp71:7", "draw16", "bp69:2"}),
          "SDK command order: token interrupt BP, token register BP");
    check((takeCallbacks() == std::vector<std::string>{"token7", "done"}),
          "token interrupt, then draw done, in interrupt context, before GXDrawDone returns");
    check(GXReadDrawSync() == 7, "GXReadDrawSync reads the token register");
}

std::atomic<int> gSpins{0};
std::atomic<bool> gStopSpin{false};
void* spinner(void*) {
    while (!gStopSpin) {
        gSpins++;
        OSYieldThread();
    }
    return nullptr;
}

void testWaitsYieldBaton() {
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    gStopSpin = false;
    OSCreateThread(&thread, spinner, nullptr, stack + sizeof(stack), sizeof(stack), 24, 0);
    OSResumeThread(&thread);
    gDrawDelayUs = 2000;
    gSpins = 0;
    for (int i = 0; i < 8; ++i) {
        writeDraw();
    }
    GXDrawDone();
    check(gSpins.load() > 0, "GXDrawDone yields the CPU baton");
    const int afterDrawDone = gSpins.load();
    for (int i = 0; i < 8; ++i) {
        writeDraw();
    }
    drain();
    check(gSpins.load() > afterDrawDone, "fifo::drain's wait yields the CPU baton");
    gDrawDelayUs = 0;
    gStopSpin = true;
    OSJoinThread(&thread, nullptr);
    takeTrace();
}

void testDrawDoneOutsideFrame() {
    gFrameActive = false;
    writeDraw();
    GXDrawDone();  // publishes everything, as Aurora's drain did
    check(gProcessed.load() == petari_aurora_fifo_write_position(), "GXDrawDone outside a frame processes everything");
    GXSetDrawDone();  // Aurora processes nothing outside a frame on its own
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(gProcessed.load() < petari_aurora_fifo_write_position(), "GXSetDrawDone outside a frame keeps Aurora's gating");
    gFrameActive = true;
    drain();  // the next frame's end
    GXWaitDrawDone();
    takeTrace();
    takeCallbacks();
}

std::atomic<int> gBreakpoints{0};
void breakpointCallback() {
    gBreakpoints++;
}

void testBreakpointAndStatus() {
    GXSetBreakPtCallback(breakpointCallback);
    GXFifoObj fifo;
    void* read;
    void* frame2;
    writeDraw();
    GXSetDrawSync(1);
    GXGetCPUFifo(&fifo);
    GXGetFifoPtrs(&fifo, &read, &frame2);
    GXEnableBreakPt(frame2);
    writeDraw();
    GXSetDrawSync(2);
    GXSetDrawDone();
    check(eventually([] { return gBreakpoints.load() == 1; }), "breakpoint callback when the processor reaches it");
    GXBool overhi, underlow, readIdle, cmdIdle, brkpt;
    GXGetGPStatus(&overhi, &underlow, &readIdle, &cmdIdle, &brkpt);
    check(brkpt && !readIdle && !cmdIdle && !overhi && !underlow, "GXGetGPStatus: halted at the breakpoint with commands pending");
    check(petari_aurora_fifo_processed_position() == GXS::pointerToPosition(frame2), "nothing past the breakpoint processed");
    u32 a, b, c, clocks1, clocks2;
    GXReadXfRasMetric(&a, &b, &c, &clocks1);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    GXReadXfRasMetric(&a, &b, &c, &clocks2);
    check(clocks1 == clocks2, "the diagnostic byte counter does not move while halted");
    GXDisableBreakPt();
    GXWaitDrawDone();
    GXGetGPStatus(&overhi, &underlow, &readIdle, &cmdIdle, &brkpt);
    check(!brkpt && readIdle && cmdIdle, "processing resumes after GXDisableBreakPt; GP idle");
    takeTrace();
    const auto t = takeCallbacks();
    check(t.back() == "done" && std::count(t.begin(), t.end(), "token2") == 1, "commands past the breakpoint processed after release");
    GXSetBreakPtCallback(nullptr);
}

// ---- Token-time snapshots ----

struct Capture {
    std::uint64_t ticket;
    std::uint16_t token;
    std::uint64_t position;
    std::uint64_t processedAtHook;
    bool onProcessorThread;
};
std::mutex gCaptureLock;
std::vector<Capture> gCaptures;
std::thread::id gProcessorThread;
std::atomic<std::uint64_t> gFlushTicket{0};
std::atomic<int> gFlushCalls{0};
std::atomic<bool> gAutoComplete{false};
std::vector<std::uint64_t> gDeliveringTickets;

void snapshotHook(std::uint64_t ticket, std::uint16_t token, std::uint64_t position, void*) {
    {
        std::lock_guard<std::mutex> g(gCaptureLock);
        gCaptures.push_back({ticket, token, position, gProcessed.load(), std::this_thread::get_id() == gWorker.get_id()});
    }
    if (gAutoComplete) {
        // Root's plan: submit the segment now, complete from the render worker.
        std::thread([ticket] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            petari_gx_sync_snapshot_ready(ticket);
        }).detach();
    }
}
void flushHook(std::uint64_t ticket, void*) {
    gFlushTicket = ticket;
    gFlushCalls++;
}
void peekingTokenCallback(u16 token) {
    gDeliveringTickets.push_back(petari_gx_sync_delivering_ticket());
    traceCallback("token" + std::to_string(token));
}

void testSnapshotTickets() {
    petari_gx_sync_set_snapshot_hook(snapshotHook, nullptr);
    petari_gx_sync_set_flush_hook(flushHook, nullptr);
    GXSetDrawSyncCallback(peekingTokenCallback);
    gCaptures.clear();
    takeTrace();
    takeCallbacks();
    const std::uint64_t deliveredBefore = petari_gx_sync_delivered_ticket();

    writeDraw();
    GXSetDrawSync(21);
    writeDraw();
    GXSetDrawSync(22);
    writeDraw();
    GXSetDrawDone();
    check(eventually([] { return gProcessed.load() == petari_aurora_fifo_write_position(); }),
          "the processor runs ahead of incomplete tickets");
    std::vector<Capture> caps;
    {
        std::lock_guard<std::mutex> g(gCaptureLock);
        caps = gCaptures;
    }
    check(caps.size() == 2 && caps[1].ticket == caps[0].ticket + 1 && caps[0].token == 21 && caps[1].token == 22,
          "one ticket per token, increasing in stream order");
    check(caps[0].onProcessorThread && caps[0].processedAtHook == caps[0].position,
          "the hook runs on the processor thread at the token's command boundary");
    check(gFlushCalls.load() == 1 && gFlushTicket.load() == caps[1].ticket,
          "a draw done queued behind incomplete tickets asks for a flush covering them");
    check(eventually([] {
              std::lock_guard<std::mutex> g(gTraceLock);
              return gCallbacks.size() == 1;
          }),
          "the draw done is delivered while the token captures are still in flight");
    check(takeTrace().size() == 8 && (takeCallbacks() == std::vector<std::string>{"done"}) && gDeliveringTickets.empty(),
          "no token delivered before its snapshot; the draw done does not wait for it");
    GXWaitDrawDone();  // returns without the captures

    check(petari_gx_sync_held_ticket() == caps[0].ticket, "the oldest incomplete ticket is reported as held");
    petari_gx_sync_snapshot_ready(caps[1].ticket);  // out of order
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(gDeliveringTickets.empty(), "a later ticket cannot overtake an earlier one");
    petari_gx_sync_snapshot_ready(caps[0].ticket);
    PetariNative::Platform::GXSync::waitTokensDelivered();  // the frame boundary
    check((gDeliveringTickets == std::vector<std::uint64_t>{caps[0].ticket, caps[1].ticket}),
          "callbacks see their own ticket (GXPeekZ reads that capture)");
    check((takeCallbacks() == std::vector<std::string>{"token21", "token22"}),
          "tokens in stream order once their captures complete, before the frame boundary returns");
    check(petari_gx_sync_delivered_ticket() == caps[1].ticket && deliveredBefore < caps[0].ticket,
          "delivered ticket advances after the callbacks return");
    check(petari_gx_sync_delivering_ticket() == 0, "no delivering ticket outside callbacks");
    check(petari_gx_sync_held_ticket() == 0, "nothing held once all captures completed");
    gDeliveringTickets.clear();
}

// DrawSyncManager (Game/System/DrawSyncManager.cpp) with snapshots completed
// by the renderer on its own: GXDrawDone mid-frame and the end-of-frame drain
// never wait for aurora_end_frame, and the breakpoint keeps the processor at
// most one peek point ahead.
OSMessageQueue gDsmQueue;
OSMessage gDsmSlots[20];
std::deque<void*> gDsmFifo;
std::atomic<int> gDsmTokens{0};

void* drawSyncManagerThread(void*) {
    while (true) {
        OSMessage msg;
        OSReceiveMessage(&gDsmQueue, &msg, OS_MESSAGE_BLOCK);
        const uintptr_t value = reinterpret_cast<uintptr_t>(msg);
        if (value >= 0x80000000) {
            gDsmFifo.push_back(msg);
            if (gDsmFifo.size() == 2) {
                GXEnableBreakPt(msg);
            }
        } else if (value < 0x10000) {
            gDsmFifo.pop_front();
            if (gDsmFifo.size() == 1) {
                GXDisableBreakPt();
            } else if (gDsmFifo.size() >= 2) {
                GXEnableBreakPt(gDsmFifo[1]);
            }
        } else {
            return nullptr;
        }
    }
}

void dsmCallback(u16 token) {
    gDsmTokens++;
    OSSendMessage(&gDsmQueue, reinterpret_cast<OSMessage>(static_cast<uintptr_t>(token)), OS_MESSAGE_BLOCK);
}

void testDrawSyncManagerWithSnapshots() {
    gAutoComplete = true;
    gDsmTokens = 0;
    OSInitMessageQueue(&gDsmQueue, gDsmSlots, 20);
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, drawSyncManagerThread, nullptr, stack + sizeof(stack), sizeof(stack), 15, 0);
    OSResumeThread(&thread);
    GXSetDrawSyncCallback(dsmCallback);
    gDrawDelayUs = 200;
    for (u16 frame = 1; frame <= 20; ++frame) {
        writeDraw();
        // pushBreakPoint + GXSetDrawSync (StarPointer / TalkPeekZ / LensFlare).
        GXFifoObj fifo;
        void* read;
        void* write;
        GXFlush();
        GXGetCPUFifo(&fifo);
        GXGetFifoPtrs(&fifo, &read, &write);
        OSSendMessage(&gDsmQueue, write, OS_MESSAGE_BLOCK);
        GXSetDrawSync(frame);
        writeDraw();
        if (frame % 4 == 0) {
            GXDrawDone();  // MarioActorSpecialDraw / OdhConverter, mid-frame
        }
        const std::uint64_t bp = GXS::breakpointPosition();
        if (bp != GXS::kNoBreakpoint) {
            check(petari_aurora_fifo_processed_position() <= bp, "processor stays behind the DrawSyncManager breakpoint");
        }
        drain();  // aurora_end_frame
    }
    gDrawDelayUs = 0;
    check(eventually([] { return gDsmTokens.load() == 20; }), "every frame's token reached DrawSyncManager");
    OSSendMessage(&gDsmQueue, reinterpret_cast<OSMessage>(0x10000), OS_MESSAGE_BLOCK);
    OSJoinThread(&thread, nullptr);
    GXDisableBreakPt();
    gAutoComplete = false;
    takeTrace();
    takeCallbacks();
}

// handleGXAbortAlarm (MainLoopFramework.cpp): the GP hangs (here, a draw
// that takes 150 ms) while the main thread waits in GXDrawDone; an alarm in
// interrupt context reads the metric and status, disables the breakpoint,
// aborts, writes a raw BP and GXSetDrawDone. The main thread must wake, and
// later draw-done waits must still count correctly.
std::atomic<bool> gMainDone{false};
void* stuckMain(void*) {
    GXDrawDone();
    gMainDone = true;
    return nullptr;
}

void testAbortRecovery() {
    GXSetDrawSyncCallback(tokenCallback);
    petari_gx_sync_set_snapshot_hook(nullptr, nullptr);
    takeTrace();
    takeCallbacks();
    gDrawDelayUs = 150000;
    writeDraw();
    GXSetDrawSync(99);
    gMainDone = false;
    static OSThread thread;
    alignas(32) static u8 stack[0x4000];
    OSCreateThread(&thread, stuckMain, nullptr, stack + sizeof(stack), sizeof(stack), 16, 0);
    OSResumeThread(&thread);
    OSSleepTicks(OSMillisecondsToTicks(20));
    check(!gMainDone.load(), "GXDrawDone blocks while the GP is stuck");
    gDrawDelayUs = 0;
    std::thread alarm([] {
        BOOL enabled = OSDisableInterrupts();  // interrupt context
        u32 a, b, c, clocks;
        GXReadXfRasMetric(&a, &b, &c, &clocks);
        GXBool overhi, underlow, readIdle, cmdIdle, brkpt;
        GXGetGPStatus(&overhi, &underlow, &readIdle, &cmdIdle, &brkpt);
        GXDisableBreakPt();
        GXAbortFrame();
        GXCmd1u8(0x61);
        GXCmd1u32(0x5800000f);
        GXSetDrawDone();
        OSRestoreInterrupts(enabled);
    });
    alarm.join();
    OSJoinThread(&thread, nullptr);
    check(gMainDone.load(), "the recovery draw done wakes the stuck GXDrawDone");
    const auto abortTrace = takeTrace();
    check((abortTrace == std::vector<std::string>{"draw16", "bp88:15", "bp69:2"}),
          "the stream written before the abort is discarded; the recovery BPs are processed");
    check((takeCallbacks() == std::vector<std::string>{"done"}), "the discarded token never interrupts");
    writeDraw();
    GXDrawDone();
    check(gProcessed.load() == petari_aurora_fifo_write_position(), "a later GXDrawDone waits for its own draw done");
    check((takeCallbacks() == std::vector<std::string>{"done"}), "and gets exactly one draw-done interrupt");
    takeTrace();
}

// Bounded stress of ticket lifetime and aborts: 400 frames of tokens whose
// captures complete out of order on a separate "render worker", mid-frame
// GXDrawDone, and every 40th frame a GP hang recovered by the game's abort
// alarm (a real OSAlarm handler in interrupt context). Checks: callbacks in
// stream order with no duplicates, every processed token gets exactly one
// ticket and one callback, tokens discarded by an abort never get either,
// and every GXDrawDone returns.
std::mutex gPoolLock;
std::condition_variable gPoolChanged;
std::vector<std::uint64_t> gPoolTickets;
bool gPoolStop = false;
std::atomic<std::uint64_t> gStressTicketsIssued{0};
std::atomic<std::uint64_t> gStressLastTicket{0};
std::vector<u16> gStressDelivered;  // GP interrupt thread only
std::atomic<int> gStressOrderErrors{0};

void stressHook(std::uint64_t ticket, std::uint16_t, std::uint64_t, void*) {
    if (ticket != gStressLastTicket.load() + 1 && gStressLastTicket.load() != 0) {
        gStressOrderErrors++;
    }
    gStressLastTicket = ticket;
    gStressTicketsIssued++;
    if (ticket % 7 == 0) {
        petari_gx_sync_snapshot_ready(ticket);  // completed inside the hook
        return;
    }
    std::lock_guard<std::mutex> g(gPoolLock);
    gPoolTickets.push_back(ticket);
    gPoolChanged.notify_one();
}

void stressPool() {
    std::mt19937 rng(1234);
    std::unique_lock<std::mutex> lock(gPoolLock);
    while (true) {
        gPoolChanged.wait(lock, [] { return gPoolStop || !gPoolTickets.empty(); });
        if (gPoolTickets.empty()) {
            return;
        }
        // Complete a random pending ticket: out of order.
        const std::size_t i = rng() % gPoolTickets.size();
        const std::uint64_t ticket = gPoolTickets[i];
        gPoolTickets.erase(gPoolTickets.begin() + i);
        const int delay = static_cast<int>(rng() % 300);
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(delay));
        petari_gx_sync_snapshot_ready(ticket);
        lock.lock();
    }
}

void stressTokenCallback(u16 token) {
    if (!gStressDelivered.empty() && token <= gStressDelivered.back()) {
        gStressOrderErrors++;
    }
    if (petari_gx_sync_delivering_ticket() == 0) {
        gStressOrderErrors++;
    }
    gStressDelivered.push_back(token);
}

std::atomic<int> gStressAborts{0};
void stressAbortAlarm(OSAlarm*, OSContext*) {
    // handleGXAbortAlarm, MainLoopFramework.cpp.
    u32 a, b, c, clocks;
    GXReadXfRasMetric(&a, &b, &c, &clocks);
    GXBool overhi, underlow, readIdle, cmdIdle, brkpt;
    GXGetGPStatus(&overhi, &underlow, &readIdle, &cmdIdle, &brkpt);
    GXDisableBreakPt();
    GXAbortFrame();
    GXCmd1u8(0x61);
    GXCmd1u32(0x5800000f);
    GXSetDrawDone();
    gStressAborts++;
}

void testStressTicketsAndAborts() {
    takeTrace();
    takeCallbacks();
    gStressDelivered.clear();
    gStressLastTicket = 0;
    gPoolStop = false;
    std::thread pool(stressPool);
    petari_gx_sync_set_flush_hook(nullptr, nullptr);
    petari_gx_sync_set_snapshot_hook(stressHook, nullptr);
    GXSetDrawSyncCallback(stressTokenCallback);
    std::mt19937 rng(99);
    u16 token = 1;
    for (int frame = 1; frame <= 400; ++frame) {
        const int tokens = 1 + static_cast<int>(rng() % 4);
        for (int t = 0; t < tokens; ++t) {
            writeDraw(8);
            GXSetDrawSync(token++);
        }
        if (frame % 40 == 0) {
            // The GP hangs on a draw; the game's 0.5 s alarm (here 5 ms)
            // aborts. Tokens written behind the hung draw are discarded.
            gDrawDelayUs = 30000;
            writeDraw(8);
            gDrawDelayUs.store(30000);
            GXSetDrawSync(token++);
            GXSetDrawSync(token++);
            OSAlarm alarm;
            OSCreateAlarm(&alarm);
            OSSetAlarm(&alarm, OSMillisecondsToTicks(5), stressAbortAlarm);
            GXDrawDone();
            OSCancelAlarm(&alarm);
            gDrawDelayUs = 0;
        } else if (rng() % 3 == 0) {
            GXDrawDone();  // mid-frame, behind incomplete tickets
        }
        drain();  // aurora_end_frame
    }
    GXDrawDone();
    {
        std::lock_guard<std::mutex> g(gPoolLock);
        gPoolStop = true;
    }
    gPoolChanged.notify_all();
    pool.join();
    check(gStressAborts.load() == 10, "every hang was recovered by the abort alarm");
    check(gStressOrderErrors.load() == 0, "tickets issued in order; callbacks in stream order, each with its ticket");
    check(gStressDelivered.size() == gStressTicketsIssued.load(), "every processed token got exactly one ticket and one callback");
    check(petari_gx_sync_delivered_ticket() == gStressLastTicket.load(), "all tickets delivered and retirable");
    check(gStressDelivered.size() < static_cast<std::size_t>(token - 1), "tokens discarded by aborts never interrupt");
    petari_gx_sync_set_snapshot_hook(nullptr, nullptr);
    GXSetDrawSyncCallback(nullptr);
    takeTrace();
    takeCallbacks();
}

}  // namespace

int main() {
    __OSThreadInit();
    startFifo();
    testFifoObjectsAndPointers();
    testTokensAndDrawDone();
    testWaitsYieldBaton();
    testDrawDoneOutsideFrame();
    testBreakpointAndStatus();
    testSnapshotTickets();
    testDrawSyncManagerWithSnapshots();
    testAbortRecovery();
    testStressTicketsAndAborts();
    stopFifo();
    GXS::shutdown();
    OSReport("GX sync backend tests passed (%d checks)\n", checks);
    return 0;
}

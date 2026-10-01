#pragma once
// Input-to-present latency probe (PETARI_LATENCY_PROBE=1). Header-only shared
// state, so the input layer, Aurora's present path and the app record into it
// without new link dependencies. Off: every hook is one relaxed atomic load.
//
// For each press of A (or Start) it records, on the host monotonic clock:
//   press   - the input event (Input::keyEvent/mouse/pad, or a test injection)
//   report  - the first virtual WPAD report carrying it (200 Hz)
//   read    - the first WPADRead (the game's KPADRead, once per game frame) that
//             returns it; the Aurora frame index then is the press's game frame
//   copy    - the first display copy (GXCopyDisp) in a later frame that can hold
//             the response: SMG draws before it updates, and copies the EFB at the
//             next frame's start, so update F is drawn in F+1 and copied in F+2
//   shown   - the first frame whose presented XFB holds that copy (seam time)
//   present - the render worker's Present() return for that frame
// From present, macOS composites the drawable at the next display refresh.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <time.h>

namespace PetariNative::LatencyProbe {

inline bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("PETARI_LATENCY_PROBE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return on;
}

inline std::uint64_t nowNs() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// One press in flight at a time; a new press while one is in flight is ignored.
enum Stage : int { Idle, Pressed, Reported, Read, Copied, Shown, Presented };
inline std::atomic<int> stage{Idle};
inline std::atomic<std::uint64_t> pressNs{0}, reportNs{0}, readNs{0}, copyNs{0}, shownNs{0}, presentNs{0};
inline std::atomic<std::uint32_t> readFrame{0}, copyFrame{0}, shownFrame{0};
inline std::atomic<const void*> copyXfb{nullptr};
// Aurora frames, counted at each present choice (petari_present::take).
inline std::atomic<std::uint32_t> frame{0};

inline void notePress() {
    if (!enabled()) return;
    int idle = Idle;
    if (stage.compare_exchange_strong(idle, Pressed)) pressNs.store(nowNs());
}
// WPAD report built with button A down.
inline void noteReport() {
    if (stage.load(std::memory_order_relaxed) != Pressed) return;
    reportNs.store(nowNs());
    stage.store(Reported);
}
// The game read a status with A down; frame: Aurora's current frame index.
inline void noteRead(std::uint32_t frame) {
    if (stage.load(std::memory_order_relaxed) != Reported) return;
    readNs.store(nowNs());
    readFrame.store(frame);
    stage.store(Read);
}
// GXCopyDisp into xfb during Aurora frame `frame`.
inline void noteCopy(const void* xfb, std::uint32_t frame) {
    if (stage.load(std::memory_order_relaxed) != Read || frame < readFrame.load() + 2) return;
    copyNs.store(nowNs());
    copyFrame.store(frame);
    copyXfb.store(xfb);
    stage.store(Copied);
}
// Aurora frame `frame` presents XFB `xfb` (aurora_end_frame, main thread).
inline bool noteShown(const void* xfb, std::uint32_t frame) {
    if (stage.load(std::memory_order_relaxed) != Copied || xfb != copyXfb.load() || frame < copyFrame.load()) return false;
    shownNs.store(nowNs());
    shownFrame.store(frame);
    stage.store(Shown);
    return true;
}
// Render worker: Present() returned for Aurora frame `frame`.
inline void notePresent(std::uint32_t frame) {
    if (stage.load(std::memory_order_relaxed) != Shown || frame != shownFrame.load()) return;
    presentNs.store(nowNs());
    stage.store(Presented);
}

}  // namespace PetariNative::LatencyProbe

namespace PetariNative::PresentTiming {
// Which display copy the seam presents. Latest (the default): the newest
// GXCopyDisp, shown as soon as the frame's FIFO is drained, two frames before
// VI would latch it (measured: press to present p50 81 -> 48 ms, same pacing).
// PETARI_PRESENT_XFB=latched: the XFB VI latched at the last retrace, the Wii's
// own scan-out timing. VI's black and dimming state apply either way.
inline bool latest() {
    static const bool on = [] {
        const char* value = std::getenv("PETARI_PRESENT_XFB");
        return value == nullptr || std::strcmp(value, "latched") != 0;
    }();
    return on;
}
// The destination of the newest GXCopyDisp (game thread).
inline std::atomic<const void*> lastDisplayCopy{nullptr};
}  // namespace PetariNative::PresentTiming

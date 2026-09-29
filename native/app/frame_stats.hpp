#pragma once
// Frame-time statistics for the frame seam (frame_seam.cpp): one record per
// game frame, kept in bounded storage (fixed histograms, a fixed window, the
// worst frames, and an optional preallocated ring for a CSV dump). Recording a
// frame does not allocate, lock or log. Standard headers only.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace PetariNative::App::FrameStats {

// What the game was doing when a frame ended (Host::framePhase), or Unfocused
// when the window lacked focus or could not present at any time in the frame.
enum class Phase : std::uint8_t {
    Startup,    // no game scene has finished initializing yet
    Loading,    // the current scene is initializing (scene change, stage load)
    Menu,       // a ready scene other than a game stage: logo, file select, ...
    Gameplay,   // a ready Game scene on a stage other than file select
    Unfocused,  // window unfocused or not presentable during the frame
};
constexpr unsigned kPhaseCount = 5;
const char* phaseName(Phase phase);

// Parts of a frame, in microseconds. The frame is the interval between two
// seam entries. The game-thread parts are consecutive and, with Unattributed,
// add up to the interval. The other-thread parts overlap them and each other.
enum Part : unsigned {
    // Game thread, in frame order.
    SeamCompose,     // seam: present compose, image rectangle, overlays
    SeamEndFrame,    // seam: aurora_end_frame (submission side; no GPU wait)
    SeamBeginFrame,  // seam: events, aurora_begin_frame (frame slot / staging memory wait)
    SeamReacquire,   // seam: waiting to get the CPU baton back
    RetraceWait,     // frame loop: waiting for the next retrace tick after the seam
    GameWork,        // frame loop: render, update, animation, capture (and waits inside them)
    DrawDoneWait,    // frame loop: endFrame's GXDrawDone wait and flush
    Unattributed,    // the rest (smoke driver, game-thread marks missing)
    // Game thread, overlapping RetraceWait.
    RetraceWake,     // latest VI retrace to the frame loop resuming after its wait
    // Other threads.
    PipelineWait,    // GX processor: blocking pipeline resolves (sum)
    EfbStagingWait,  // GX processor: EFB capture staging memory (sum)
    EfbSubmitWait,   // GX processor: EFB capture segment submit (sum)
    EfbCaptureMax,   // longest EFB capture request-to-mapped latency (max)
    DrawableAcquire, // render worker: drawable acquisition (sum)
    FrameSubmit,     // render worker: frame command-buffer submit (sum)
    PresentCall,     // render worker: present call, submission side (sum)
    ViTimerLateMax,  // VI thread: largest retrace-timer overshoot (max)
    PartCount
};
const char* partName(unsigned part);
constexpr unsigned kGameThreadParts = Unattributed + 1;

struct Frame {
    std::uint64_t index = 0;  // 1 for the interval ending at the second seam
    double intervalMs = 0.0;
    Phase phase = Phase::Startup;
    std::uint32_t efbCaptures = 0;
    std::uint32_t pipelineWaits = 0;
    std::uint32_t us[PartCount] = {};
};

// VI field rate (NTSC 60000/1001 Hz). A frame is "late" past 1.25 periods:
// under timer-paced VI it missed its retrace and waits for the next.
constexpr double kFieldMs = 1000.0 * 1001.0 / 60000.0;
constexpr double kLateMs = 1.25 * kFieldMs;

class Recorder {
public:
    // Keeps the last csvFrames frames for writeCsv (0: none). Allocates.
    explicit Recorder(std::size_t csvFrames = 0);

    void add(const Frame& frame);

    // The frames since the last window report, then starts a new window.
    void writeWindow(std::FILE* out);
    void writeSummary(std::FILE* out) const;
    // Frames kept for the CSV, oldest first. Returns false if it cannot write.
    bool writeCsv(const char* path) const;

    std::uint64_t frames() const { return mFrames; }

    // Nearest-rank percentile of a phase's intervals (kPhaseCount: all),
    // accurate to the histogram bin (10 us below 50 ms, 1 ms to 5 s).
    double percentile(unsigned phase, double p) const;

    struct Totals {
        std::uint64_t frames = 0;
        double sumMs = 0.0;
        double maxMs = 0.0;
        std::uint64_t over16 = 0;  // > 16.7 ms
        std::uint64_t over33 = 0;  // > 33.3 ms
        std::uint64_t late = 0;    // > kLateMs
        std::uint64_t longestLateRun = 0;
        std::uint64_t lateSum[PartCount] = {};  // us, over late frames
        std::uint64_t allSum[PartCount] = {};   // us, over all frames
        std::uint64_t efbCaptures = 0;
        std::uint64_t pipelineWaits = 0;
    };
    const Totals& totals(unsigned phase) const { return mTotals[phase]; }

private:
    static constexpr unsigned kFineBins = 5000;    // 10 us to 50 ms
    static constexpr unsigned kCoarseBins = 4950;  // 1 ms from 50 ms to 5 s
    static constexpr unsigned kBins = kFineBins + kCoarseBins + 1;
    static constexpr unsigned kWindow = 120;
    static constexpr unsigned kWorst = 8;
    static unsigned bin(double ms);
    static double binUpperMs(unsigned bin);

    std::uint64_t mFrames = 0;
    Totals mTotals[kPhaseCount + 1];
    std::vector<std::uint32_t> mHistogram;  // (kPhaseCount + 1) * kBins
    std::uint64_t mRun = 0;                 // current late run, any phase
    std::uint64_t mPhaseRun = 0;            // current late run within mRunPhase
    Phase mRunPhase = Phase::Startup;
    Frame mWindow[kWindow];
    unsigned mWindowCount = 0;
    Frame mWorst[kWorst];
    unsigned mWorstCount = 0;
    std::vector<Frame> mRing;
    std::size_t mRingNext = 0;
    std::uint64_t mRingDropped = 0;
};

}  // namespace PetariNative::App::FrameStats

// Frame-time statistics (frame_stats.hpp).

#include "frame_stats.hpp"

#include <algorithm>
#include <cmath>

namespace PetariNative::App::FrameStats {

namespace {

constexpr const char* kPhaseNames[kPhaseCount + 1] = {"startup", "loading", "menu", "gameplay", "total"};
constexpr const char* kPartNames[PartCount] = {
    "seam_compose", "seam_end_frame", "seam_begin_frame", "seam_reacquire", "retrace_wait", "game_work",
    "draw_done_wait", "unattributed", "retrace_wake", "pipeline_wait", "efb_staging_wait", "efb_submit_wait",
    "efb_capture_max", "drawable_acquire", "frame_submit", "present_call", "vi_timer_late_max", "vi_lock_wait_max",
    "texture_hash", "texture_upload", "token_barrier_wait", "stage_prep_wait",
};
constexpr unsigned kTotal = kPhaseCount;

double ms(std::uint64_t us) {
    return static_cast<double>(us) / 1000.0;
}

// Mean per frame of summed parts, largest value of the max parts.
void writeParts(std::FILE* out, const std::uint64_t* sums, std::uint64_t frames, const std::uint32_t* maxima) {
    if (frames == 0) {
        return;
    }
    std::fprintf(out,
                 "      game thread: retrace wait %.2f, game work %.2f, draw-done wait %.2f, seam compose %.2f, "
                 "end frame %.2f, begin frame %.2f, CPU reacquire %.2f, unattributed %.2f (retrace-to-resume %.2f, "
                 "token barrier %.2f and stage shader prep %.2f in game work)\n",
                 ms(sums[RetraceWait]) / frames, ms(sums[GameWork]) / frames, ms(sums[DrawDoneWait]) / frames,
                 ms(sums[SeamCompose]) / frames, ms(sums[SeamEndFrame]) / frames, ms(sums[SeamBeginFrame]) / frames,
                 ms(sums[SeamReacquire]) / frames, ms(sums[Unattributed]) / frames, ms(sums[RetraceWake]) / frames,
                 ms(sums[TokenBarrierWait]) / frames, ms(sums[StagePrepWait]) / frames);
    std::fprintf(out,
                 "      other threads (overlapping): pipeline wait %.2f, EFB staging %.2f, EFB submit %.2f, "
                 "texture hash %.2f, texture upload %.2f, drawable acquire %.2f, frame submit %.2f, present call %.2f",
                 ms(sums[PipelineWait]) / frames, ms(sums[EfbStagingWait]) / frames, ms(sums[EfbSubmitWait]) / frames,
                 ms(sums[TextureHash]) / frames, ms(sums[TextureUpload]) / frames, ms(sums[DrawableAcquire]) / frames,
                 ms(sums[FrameSubmit]) / frames, ms(sums[PresentCall]) / frames);
    if (maxima != nullptr) {
        std::fprintf(out, "; max EFB capture latency %.2f, max VI timer overshoot %.2f, max VI lock wait %.2f",
                     ms(maxima[EfbCaptureMax]), ms(maxima[ViTimerLateMax]), ms(maxima[ViLockWaitMax]));
    }
    std::fputc('\n', out);
}

}  // namespace

const char* phaseName(Phase phase) {
    return kPhaseNames[static_cast<unsigned>(phase)];
}

const char* partName(unsigned part) {
    return part < PartCount ? kPartNames[part] : "?";
}

Recorder::Recorder(std::size_t csvFrames)
    : mHistogram(static_cast<std::size_t>(kPhaseCount + 1) * kBins, 0), mRing(csvFrames) {}

unsigned Recorder::bin(double ms) {
    if (!(ms >= 0.0)) {
        return 0;
    }
    if (ms < 50.0) {
        return static_cast<unsigned>(ms * 100.0);
    }
    const double coarse = ms - 50.0;
    if (coarse < kCoarseBins) {
        return kFineBins + static_cast<unsigned>(coarse);
    }
    return kBins - 1;
}

double Recorder::binUpperMs(unsigned index) {
    if (index < kFineBins) {
        return (index + 1) / 100.0;
    }
    return 50.0 + (index - kFineBins + 1);
}

double Recorder::percentile(unsigned phase, double p) const {
    const Totals& totals = mTotals[phase];
    if (totals.frames == 0) {
        return 0.0;
    }
    const std::uint64_t rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(p / 100.0 * totals.frames)));
    const std::uint32_t* histogram = &mHistogram[static_cast<std::size_t>(phase) * kBins];
    std::uint64_t seen = 0;
    for (unsigned i = 0; i < kBins; i++) {
        seen += histogram[i];
        if (seen >= rank) {
            // The overflow bin and the top bin report the largest interval.
            return i >= kBins - 1 ? totals.maxMs : std::min(binUpperMs(i), totals.maxMs);
        }
    }
    return totals.maxMs;
}

void Recorder::add(const Frame& frame) {
    ++mFrames;
    const bool late = frame.intervalMs > kLateMs;
    const unsigned phase = static_cast<unsigned>(frame.phase);
    const unsigned binIndex = bin(frame.intervalMs);
    for (unsigned index : {phase, kTotal}) {
        Totals& t = mTotals[index];
        ++t.frames;
        t.sumMs += frame.intervalMs;
        t.maxMs = std::max(t.maxMs, frame.intervalMs);
        t.over16 += frame.intervalMs > 16.7;
        t.over33 += frame.intervalMs > 1000.0 / 30.0;
        t.late += late;
        t.efbCaptures += frame.efbCaptures;
        t.pipelineWaits += frame.pipelineWaits;
        t.textureUploads += frame.textureUploads;
        t.unfocused += frame.unfocused;
        for (unsigned part = 0; part < PartCount; part++) {
            t.allSum[part] += frame.us[part];
            if (late) {
                t.lateSum[part] += frame.us[part];
            }
        }
        ++mHistogram[static_cast<std::size_t>(index) * kBins + binIndex];
    }

    // Late runs: across phases for the total, within one phase for the phase.
    if (late) {
        mPhaseRun = mRun > 0 && mRunPhase == frame.phase ? mPhaseRun + 1 : 1;
        mRunPhase = frame.phase;
        ++mRun;
        mTotals[kTotal].longestLateRun = std::max(mTotals[kTotal].longestLateRun, mRun);
        mTotals[phase].longestLateRun = std::max(mTotals[phase].longestLateRun, mPhaseRun);
    } else {
        mRun = 0;
        mPhaseRun = 0;
    }

    if (mWindowCount < kWindow) {
        mWindow[mWindowCount++] = frame;
    }

    // The worst frames, longest first.
    if (mWorstCount < kWorst || frame.intervalMs > mWorst[mWorstCount - 1].intervalMs) {
        unsigned i = mWorstCount < kWorst ? mWorstCount++ : kWorst - 1;
        for (; i > 0 && mWorst[i - 1].intervalMs < frame.intervalMs; --i) {
            mWorst[i] = mWorst[i - 1];
        }
        mWorst[i] = frame;
    }

    if (!mRing.empty()) {
        if (mFrames > mRing.size()) {
            ++mRingDropped;
        }
        mRing[mRingNext] = frame;
        mRingNext = (mRingNext + 1) % mRing.size();
    }
}

void Recorder::writeWindow(std::FILE* out) {
    if (mWindowCount == 0) {
        return;
    }
    double sorted[kWindow];
    std::uint64_t sums[PartCount] = {};
    std::uint32_t maxima[PartCount] = {};
    unsigned over16 = 0, over33 = 0, late = 0, efb = 0, pipelines = 0, textures = 0, unfocused = 0;
    unsigned phases[kPhaseCount] = {};
    for (unsigned i = 0; i < mWindowCount; i++) {
        const Frame& frame = mWindow[i];
        sorted[i] = frame.intervalMs;
        over16 += frame.intervalMs > 16.7;
        over33 += frame.intervalMs > 1000.0 / 30.0;
        late += frame.intervalMs > kLateMs;
        efb += frame.efbCaptures;
        pipelines += frame.pipelineWaits;
        textures += frame.textureUploads;
        unfocused += frame.unfocused;
        ++phases[static_cast<unsigned>(frame.phase)];
        for (unsigned part = 0; part < PartCount; part++) {
            sums[part] += frame.us[part];
            maxima[part] = std::max(maxima[part], frame.us[part]);
        }
    }
    std::sort(sorted, sorted + mWindowCount);
    const auto rank = [&](double p) {
        const unsigned r = static_cast<unsigned>(std::ceil(p / 100.0 * mWindowCount));
        return sorted[std::max(1u, r) - 1];
    };
    std::fprintf(out,
                 "Petari frames %llu-%llu: p50 %.2f p95 %.2f p99 %.2f max %.2f ms, >16.7 ms %u, >33.3 ms %u, "
                 "late %u; phases",
                 static_cast<unsigned long long>(mWindow[0].index),
                 static_cast<unsigned long long>(mWindow[mWindowCount - 1].index), rank(50), rank(95), rank(99),
                 sorted[mWindowCount - 1], over16, over33, late);
    for (unsigned phase = 0; phase < kPhaseCount; phase++) {
        if (phases[phase] != 0) {
            std::fprintf(out, " %s %u", kPhaseNames[phase], phases[phase]);
        }
    }
    std::fprintf(out, "; unfocused %u; EFB captures %u, blocking pipeline resolves %u, texture uploads %u\n", unfocused, efb,
                 pipelines, textures);
    std::fputs("    mean ms per frame:\n", out);
    writeParts(out, sums, mWindowCount, maxima);
    mWindowCount = 0;
}

void Recorder::writeSummary(std::FILE* out) const {
    const Totals& all = mTotals[kTotal];
    std::fprintf(out,
                 "Petari frame times: %llu intervals between frame seams (game-thread wall time; presentation "
                 "and scan-out are not observed). late = over %.2f ms (1.25 VI fields)\n",
                 static_cast<unsigned long long>(all.frames), kLateMs);
    std::fprintf(out, "  %-9s %8s %9s %7s %7s %7s %9s %8s %8s %8s %9s %9s\n", "phase", "frames", "seconds", "p50", "p95",
                 "p99", "max", ">16.7ms", ">33.3ms", "late", "late run", "unfocused");
    for (unsigned index : {0u, 1u, 2u, 3u, kTotal}) {
        const Totals& t = mTotals[index];
        if (t.frames == 0 && index != kTotal) {
            continue;
        }
        std::fprintf(out, "  %-9s %8llu %9.2f %7.2f %7.2f %7.2f %9.2f %8llu %8llu %8llu %9llu %9llu\n", kPhaseNames[index],
                     static_cast<unsigned long long>(t.frames), t.sumMs / 1000.0, percentile(index, 50),
                     percentile(index, 95), percentile(index, 99), t.maxMs, static_cast<unsigned long long>(t.over16),
                     static_cast<unsigned long long>(t.over33), static_cast<unsigned long long>(t.late),
                     static_cast<unsigned long long>(t.longestLateRun), static_cast<unsigned long long>(t.unfocused));
    }
    for (unsigned index : {0u, 1u, 2u, 3u, kTotal}) {
        const Totals& t = mTotals[index];
        if (t.frames == 0) {
            continue;
        }
        std::fprintf(out, "  %s: %llu EFB captures, %llu blocking pipeline resolves, %llu texture uploads; mean ms per frame:\n",
                     kPhaseNames[index], static_cast<unsigned long long>(t.efbCaptures),
                     static_cast<unsigned long long>(t.pipelineWaits), static_cast<unsigned long long>(t.textureUploads));
        writeParts(out, t.allSum, t.frames, nullptr);
        if (t.late != 0) {
            std::fprintf(out, "    late frames only (%llu):\n", static_cast<unsigned long long>(t.late));
            writeParts(out, t.lateSum, t.late, nullptr);
        }
    }
    std::fputs("  worst frames (ms):\n", out);
    for (unsigned i = 0; i < mWorstCount; i++) {
        const Frame& f = mWorst[i];
        std::fprintf(out, "    frame %llu %s%s %.2f: EFB captures %u, pipeline resolves %u, texture uploads %u\n",
                     static_cast<unsigned long long>(f.index), phaseName(f.phase), f.unfocused ? " (unfocused)" : "",
                     f.intervalMs, f.efbCaptures, f.pipelineWaits, f.textureUploads);
        std::uint64_t sums[PartCount];
        std::copy(f.us, f.us + PartCount, sums);
        writeParts(out, sums, 1, f.us);
    }
    if (!mRing.empty() && mRingDropped != 0) {
        std::fprintf(out, "  frame CSV keeps the last %zu frames; %llu earlier frames are only in the totals\n",
                     mRing.size(), static_cast<unsigned long long>(mRingDropped));
    }
}

bool Recorder::writeCsv(const char* path) const {
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    std::fputs("frame,phase,unfocused,interval_ms,efb_captures,pipeline_resolves,texture_uploads", file);
    for (unsigned part = 0; part < PartCount; part++) {
        std::fprintf(file, ",%s_ms", kPartNames[part]);
    }
    std::fputc('\n', file);
    const std::size_t kept = std::min<std::uint64_t>(mFrames, mRing.size());
    const std::size_t first = mFrames > mRing.size() ? mRingNext : 0;
    for (std::size_t i = 0; i < kept; i++) {
        const Frame& f = mRing[(first + i) % mRing.size()];
        std::fprintf(file, "%llu,%s,%d,%.3f,%u,%u,%u", static_cast<unsigned long long>(f.index), phaseName(f.phase),
                     f.unfocused ? 1 : 0, f.intervalMs, f.efbCaptures, f.pipelineWaits, f.textureUploads);
        for (unsigned part = 0; part < PartCount; part++) {
            std::fprintf(file, ",%.3f", ms(f.us[part]));
        }
        std::fputc('\n', file);
    }
    return std::fclose(file) == 0;
}

}  // namespace PetariNative::App::FrameStats

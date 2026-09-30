static void petari_note_stage_config(HashType hash, PipelineRef runtimeKey);
static void petari_stage_reset();
static void petari_stage_flush_logs();
static void petari_seed_global();
static PetariPipeline::Clock::time_point petariGlobalStarted{}, petariGlobalLastLog{};
static size_t petariGlobalInitialPending = 0;
static bool petariGlobalActive = false;
static void petari_global_progress() {
  std::lock_guard lock{g_pipelineMutex};
  if (!petariGlobalActive) return;
  const auto now = PetariPipeline::Clock::now();
  const auto remaining = g_pendingPipelines.size();
  if (remaining && now - petariGlobalLastLog < std::chrono::seconds(5)) return;
  petariGlobalLastLog = now;
  const auto completed = petariGlobalInitialPending - std::min(petariGlobalInitialPending, remaining);
  std::fprintf(stderr, "[gx global prep] completed=%zu total=%zu remaining=%zu elapsed_ms=%.3f state=%s\n",
               completed, petariGlobalInitialPending, remaining,
               PetariPipeline::milliseconds(now - petariGlobalStarted), remaining ? "preparing" : "ready");
  if (!remaining) petariGlobalActive = false;
}
struct PetariPipelineSample {
  ShaderType type = ShaderType::GX;
  HashType configKey = 0;
  PetariPipeline::Clock::time_point queued{}, started{}, finished{}, requested{}, firstSkip{}, lastSkip{};
  PetariPipeline::Stages stages{};
  uint64_t skipped = 0;
  bool failed = false;
  bool drawRequested = false;
  bool drawBlocking = false; // a draw or clear waited for (or skipped) this variant
};
static absl::flat_hash_map<PipelineRef, PetariPipelineSample> petariPipelineSamples;
static std::atomic<unsigned> petariPipelineWaiters{0};
static std::atomic<uint64_t> petariSkippedDraws{0};
static uint32_t petariFailedPipelines = 0; // guarded by g_pipelineMutex
static unsigned petariActiveWorkers = PetariPipeline::workerCount(); // guarded by g_pipelineMutex
// Guarded by g_pipelineMutex: global-backlog jobs compiling now and their cap
// outside gameplay; stage jobs no draw requested yet compiling now; and
// whether the last frame was active gameplay (set from the frame seam).
static unsigned petariBackgroundInFlight = 0;
static unsigned petariStageSpeculativeInFlight = 0;
static bool petariGameplay = false;
// Bounded GX draw waits: remaining wait this frame, and draws whose pipeline was
// still compiling when the budget ran out (encoded as skips until it lands).
static PetariPipeline::Clock::duration petariDrawBudgetLeft = PetariPipeline::drawWaitBudget();
static uint64_t petariDeferredDraws = 0;
static unsigned petariBackgroundCap = ~0u;

extern "C" void petari_gx_pipeline_startup_finished() {
  unsigned previous, current;
  {
    std::lock_guard lock{g_pipelineMutex};
    previous = petariActiveWorkers;
    current = petariActiveWorkers = PetariPipeline::workerCount();
  }
  g_pipelineQueueCv.notify_all();
  std::fprintf(stderr, "[gx pipeline pool] startup_finished=1 previous_workers=%u background_workers=%u "
               "gameplay_stage_cap=%u gameplay_backlog_cap=%u extra_inflight=drain\n",
               previous, current, PetariPipeline::gameplayStageCap(), PetariPipeline::gameplayBacklogCap());
}


extern "C" void petari_gx_pipeline_preparation_status(uint32_t* total, uint32_t* pending, uint32_t* failed) {
  std::lock_guard lock{g_pipelineMutex};
  *pending = static_cast<uint32_t>(g_pendingPipelines.size());
  *total = static_cast<uint32_t>(g_pipelines.size()) + *pending;
  *failed = petariFailedPipelines;
}

extern "C" bool petari_gx_pipeline_full_preparation() {
  return PetariPipeline::globalPrecompile();
}

extern "C" void petari_gx_pipeline_set_gameplay(bool gameplay) {
  {
    std::lock_guard lock{g_pipelineMutex};
    if (petariGameplay == gameplay) return;
    petariGameplay = gameplay;
  }
  // Leaving gameplay lifts the speculative throttle for parked workers.
  if (!gameplay) g_pipelineQueueCv.notify_all();
}

static void petari_note_variant(PipelineRef runtimeKey, HashType configKey, ShaderType type,
                                PipelinePriority priority) {
  std::lock_guard lock{g_pipelineMutex};
  auto& sample = petariPipelineSamples[runtimeKey];
  sample.drawBlocking |= priority == PipelinePriority::Blocking;
  sample.type = type;
  sample.configKey = configKey;
  if (priority != PipelinePriority::Background && sample.requested == PetariPipeline::Clock::time_point{}) {
    sample.requested = PetariPipeline::Clock::now();
  }
}

struct PetariPipelineWait {
  bool active;
  PipelineRef key;
  PetariPipeline::Clock::time_point start;
  PetariPipelineWait(bool blocking, PipelineRef runtimeKey) : active(blocking), key(runtimeKey) {
    if (active) {
      start = PetariPipeline::Clock::now();
      petariPipelineWaiters.fetch_add(1, std::memory_order_relaxed);
    }
  }
  ~PetariPipelineWait() {
    if (!active) return;
    petariPipelineWaiters.fetch_sub(1, std::memory_order_relaxed);
    const auto waited = PetariPipeline::Clock::now() - start;
    // Frame telemetry: real waits only; a ready pipeline returns in microseconds.
    const auto waitedNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
    if (waitedNs >= 50000) {
      PetariNative::FrameTelemetry::add(PetariNative::FrameTelemetry::PipelineWait, waitedNs);
    }
    const auto ms = PetariPipeline::milliseconds(waited);
    if (PetariPipeline::diagnostics() && ms >= 10.0) {
      std::fprintf(stderr, "[gx pipeline] blocking resolve %.2f ms key=%016llx\n", ms,
                   static_cast<unsigned long long>(key));
    }
  }
};

// Next queue a worker may take from, or null (guarded by g_pipelineMutex).
// Draw-requested jobs sit at the front of the requested queue and are never
// throttled; stage work precedes the global backlog.
static std::deque<PendingPipeline>* petari_runnable_queue() {
  if (!g_pipelineQueue.empty()) {
    const auto sample = petariPipelineSamples.find(g_pipelineQueue.front().hash);
    if (!petariGameplay || (sample != petariPipelineSamples.end() && sample->second.drawBlocking) ||
        petariStageSpeculativeInFlight < PetariPipeline::gameplayStageCap())
      return &g_pipelineQueue;
    return nullptr;
  }
  const unsigned backlogCap = petariGameplay ? PetariPipeline::gameplayBacklogCap() : petariBackgroundCap;
  if (!g_backgroundPipelineQueue.empty() && petariBackgroundInFlight < backlogCap) return &g_backgroundPipelineQueue;
  return nullptr;
}

template <typename Ready>
static void petari_bounded_draw_wait(std::unique_lock<std::mutex>& lock, Ready ready) {
  if (petariDrawBudgetLeft > PetariPipeline::Clock::duration::zero()) {
    const auto start = PetariPipeline::Clock::now();
    g_pipelineReadyCv.wait_until(lock, start + petariDrawBudgetLeft, ready);
    petariDrawBudgetLeft -= std::min(petariDrawBudgetLeft, PetariPipeline::Clock::now() - start);
  }
  if (!ready()) ++petariDeferredDraws;
}

static void petari_pipeline_frame_budget() {
  std::lock_guard lock{g_pipelineMutex};
  petariDrawBudgetLeft = PetariPipeline::drawWaitBudget();
}

static void petari_pipeline_summary() {
  uint64_t missedConfigs = 0, skipped = 0, unfinished = 0, failed = 0;
  double maxPendingMs = 0;
  const auto now = PetariPipeline::Clock::now();
  for (const auto& [key, sample] : petariPipelineSamples) {
    failed += sample.failed;
    if (!sample.skipped) continue;
    ++missedConfigs;
    skipped += sample.skipped;
    const bool pending = sample.finished == PetariPipeline::Clock::time_point{};
    unfinished += pending;
    const double pendingMs = PetariPipeline::milliseconds((pending || sample.failed ? now : sample.finished) - sample.firstSkip);
    maxPendingMs = std::max(maxPendingMs, pendingMs);
    std::fprintf(stderr,
        "[gx pipeline miss] key=%016llx config=%016llx skipped=%llu first_skip_to_terminal_or_exit_ms=%.3f "
        "visible_skip_span_ms=%.3f state=%s\n",
        static_cast<unsigned long long>(key), static_cast<unsigned long long>(sample.configKey),
        static_cast<unsigned long long>(sample.skipped), pendingMs,
        PetariPipeline::milliseconds(sample.lastSkip - sample.firstSkip),
        pending ? "pending" : sample.failed ? "failed" : "ready");
  }
  std::fprintf(stderr,
      "[gx pipeline summary] policy=%s skipped_draws=%llu deferred_draws=%llu missed_configs=%llu unfinished=%llu "
      "failed_compiles=%llu pending_compiles=%zu terminal_compiles=%zu max_first_skip_to_terminal_or_exit_ms=%.3f\n",
      PetariPipeline::asynchronous() ? "async" : PetariPipeline::unboundedBlocking() ? "blocking" : "bounded",
      static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(petariDeferredDraws),
      static_cast<unsigned long long>(missedConfigs), static_cast<unsigned long long>(unfinished),
      static_cast<unsigned long long>(failed), g_pendingPipelines.size(), g_pipelines.size(), maxPendingMs);
}

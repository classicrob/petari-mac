static void pipeline_worker(unsigned workerIndex = 0) {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif
  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    bool background = false;
    {
      std::unique_lock lock{g_pipelineMutex};
      if (g_hasPipelineThread) {
        g_pipelineQueueCv.wait(lock, [workerIndex] {
          return workerIndex >= petariActiveWorkers || !g_pipelineQueue.empty() ||
                 !g_backgroundPipelineQueue.empty() || g_pipelineThreadEnd;
        });
      } else if (g_pipelineQueue.empty() && g_backgroundPipelineQueue.empty()) {
        return;
      }
      if (g_pipelineThreadEnd || (g_hasPipelineThread && workerIndex >= petariActiveWorkers)) break;
      background = g_pipelineQueue.empty();
      auto& source = background ? g_backgroundPipelineQueue : g_pipelineQueue;
      pending = std::move(source.front());
      source.pop_front();
      petariPipelineSamples[pending.hash].started = PetariPipeline::Clock::now();
    }
    if (g_hasPipelineThread) PetariPipeline::compilationQoS(background);
    PetariPipeline::stages = {.runtimeKey = pending.hash};
    auto result = pending.create();
    const auto finished = PetariPipeline::Clock::now();
    const auto pipelineCount = result.pipeline_count();
    PetariPipelineSample sample;
    {
      std::lock_guard lock{g_pipelineMutex};
      auto& stored = petariPipelineSamples[pending.hash];
      stored.finished = finished;
      stored.stages = PetariPipeline::stages;
      stored.failed = !result.main;
      petariFailedPipelines += stored.failed;
      sample = stored;
      g_pipelines.try_emplace(pending.hash, std::move(result));
      g_pendingPipelines.erase(pending.hash);
    }
    if (!g_hasPipelineThread) g_pipelinesPerFrame += std::max(1u, pipelineCount);
    notify_pipeline_ready(true, pipelineCount);
    petari_global_progress();
    if (PetariPipeline::diagnostics()) {
      const auto requestWait = sample.requested == PetariPipeline::Clock::time_point{} ? 0.0 :
          std::max(0.0, PetariPipeline::milliseconds(sample.started - sample.requested));
      std::fprintf(stderr,
          "[gx pipeline compile] key=%016llx config=%016llx type=%u queue_ms=%.3f requested_queue_ms=%.3f "
          "build_ms=%.3f wgsl_ms=%.3f module_ms=%.3f pipeline_api_ms=%.3f dump_ms=%.3f "
          "wgsl_bytes=%llu wgsl_lines=%llu shaders=%u tev_max=%u indirect_max=%u status=%s\n",
          static_cast<unsigned long long>(pending.hash), static_cast<unsigned long long>(sample.configKey),
          static_cast<unsigned>(sample.type), PetariPipeline::milliseconds(sample.started - sample.queued),
          requestWait, PetariPipeline::milliseconds(finished - sample.started),
          sample.stages.sourceMs, sample.stages.moduleMs, sample.stages.pipelineMs, sample.stages.dumpMs,
          static_cast<unsigned long long>(sample.stages.sourceBytes),
          static_cast<unsigned long long>(sample.stages.sourceLines), sample.stages.shaderCount,
          sample.stages.maxTevStages, sample.stages.maxIndStages, sample.failed ? "failed" : "ready");
    }
  }
}

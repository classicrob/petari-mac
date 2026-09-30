#include "pipeline_owner.hpp"
#include <petari/host_allocation.hpp>
#include <atomic>
namespace aurora::gfx {
static std::atomic<uint64_t> petariManifestReadFailures{0};

extern "C" uint64_t petari_gx_pipeline_manifest_failure_count() {
  return petariManifestReadFailures.load(std::memory_order_relaxed);
}

struct PetariStageRecord {
  bool initialized = false;
  bool prepared = false;
  uint64_t layoutKey = 0;
  absl::flat_hash_set<HashType> configs;
  absl::flat_hash_set<HashType> seen;
  std::vector<PipelineRef> targets;
};
static absl::flat_hash_map<std::string, PetariStageRecord> petariStages;
static std::string petariActiveStage;
static absl::flat_hash_set<std::string> petariOverlayStages;
static std::vector<std::string> petariStageLogs;

static void petari_stage_flush_logs() {
  std::vector<std::string> logs;
  {
    std::lock_guard lock{g_pipelineMutex};
    logs.swap(petariStageLogs);
  }
  for (const auto& line : logs) std::fprintf(stderr, "%s\n", line.c_str());
}

static void petari_stage_reset() {
  std::lock_guard lock{g_pipelineMutex};
  petariStages.clear();
  petariActiveStage.clear();
  petariOverlayStages.clear();
}

static void petari_note_stage_config(HashType hash, PipelineRef runtimeKey) {
  std::lock_guard lock{g_pipelineMutex};
  petariPipelineSamples[runtimeKey].drawRequested = true;
  const auto record = [&](const std::string& name, bool overlay) {
    auto& stage = petariStages[name];
    if (!stage.seen.insert(hash).second) return;
    bool overlayCovered = false;
    for (const auto& tag : petariOverlayStages) overlayCovered |= petariStages[tag].configs.contains(hash);
    petariStageLogs.push_back(fmt::format(
        "[gx stage config] stage={} config={:016x} phase={} covered={} stage_covered={} overlay_tag={} overlay_covered={} owner={}",
        name, hash, stage.prepared ? "after_prep" : "loading",
        stage.configs.contains(hash) || overlayCovered ? 1 : 0, stage.configs.contains(hash) ? 1 : 0,
        overlay ? 1 : 0, overlayCovered ? 1 : 0, PetariPipelineOwner::current));
  };
  if (!petariActiveStage.empty()) record(petariActiveStage, false);
  for (const auto& name : petariOverlayStages) record(name, true);
}

static std::string petari_stage_name(const char* value) {
  if (!value || !*value) return "Unknown";
  std::string result;
  for (; *value && result.size() < 96; ++value) {
    const char c = *value;
    result += ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
  }
  return result;
}

extern "C" void petari_gx_pipeline_stage_begin(const char* name, const char* seedPath) {
  // Called from game threads; keep renderer allocations off the current JKR heap.
  PetariNative::HostAllocationScope petariHostAllocations;
  const auto started = PetariPipeline::Clock::now();
  const std::string stageName = petari_stage_name(name);
  const bool globalBackground = stageName == "__global__";
  const bool overlay = globalBackground || stageName == "ScenarioSelect" || stageName == "GalaxyMap";
  const auto priority = globalBackground ? PipelinePriority::Background : PipelinePriority::Normal;
  const auto scene = scene_render_target_layout();
  auto offscreen = scene;
  offscreen.colorAttachmentCount = 1;
  offscreen.sampleCount = 1;
  detail::finalize_render_target_layout(offscreen);
  {
    std::lock_guard lock{g_pipelineMutex};
    if (!overlay && petariActiveStage != stageName && !petariActiveStage.empty()) {
      auto previousTargets = petariStages[petariActiveStage].targets;
      for (const auto& tag : petariOverlayStages) {
        const auto& extra = petariStages[tag].targets;
        previousTargets.insert(previousTargets.end(), extra.begin(), extra.end());
      }
      // Move old speculative work behind newly requested work. Work already
      // compiling is not interrupted; real draw requests retain priority.
      for (const auto key : previousTargets) {
        const auto stat = petariPipelineSamples.find(key);
        if (stat != petariPipelineSamples.end() && stat->second.drawRequested) continue;
        auto it = find_pending_pipeline(g_pipelineQueue, key);
        if (it != g_pipelineQueue.end()) {
          g_backgroundPipelineQueue.emplace_back(std::move(*it));
          g_pipelineQueue.erase(it);
        }
      }
    }
    if (globalBackground) {
      // Global preparation must not affect stage attribution, coverage, or gates.
    } else if (overlay) {
      petariOverlayStages.insert(stageName);
    } else {
      if (petariActiveStage != stageName) petariOverlayStages.clear();
      petariActiveStage = stageName;
    }
    auto& stage = petariStages[stageName];
    if (stage.initialized && stage.layoutKey == scene.key) {
      if (!overlay) stage.prepared = false;
      for (const auto key : stage.targets) promote_pending_pipeline(key, priority);
      g_pipelineQueueCv.notify_all();
      return;
    }
    stage = {};
    stage.initialized = true;
    stage.layoutKey = scene.key;
  }

  std::vector<KnownPipeline> manifest;
  std::string provenance = "shared-observed-memory";
  std::string path;
  if (seedPath && *seedPath) {
    path = seedPath;
  } else {
    const char* directory = std::getenv("PETARI_PIPELINE_SEED_DIR");
    auto root = directory && *directory ? std::filesystem::path(directory) :
        std::filesystem::path(g_config.resourcesPath ? g_config.resourcesPath : "") / "pipeline-seeds";
    auto candidate = root / (stageName + ".db");
    std::error_code error;
    if (std::filesystem::exists(candidate, error)) path = candidate.string();
  }
  if (!path.empty()) {
    std::string failure;
    if (sqlite3* db = open_pipeline_cache_seed_db(path)) {
      sqlite3_stmt* query = nullptr;
      if (sqlite3_prepare_v2(db, "SELECT type,config_version,config FROM pipeline_cache ORDER BY first_frame_used", -1,
                             &query, nullptr) == SQLITE_OK) {
        int status;
        unsigned rejected = 0;
        while ((status = sqlite3_step(query)) == SQLITE_ROW) {
          const auto type = sqlite3_column_int(query, 0);
          const auto version = sqlite3_column_int(query, 1);
          const auto size = sqlite3_column_bytes(query, 2);
          const auto* data = sqlite3_column_blob(query, 2);
          if (type == static_cast<int>(ShaderType::GX) && version == gx::GXPipelineConfigVersion &&
              size == sizeof(gx::PipelineConfig) && data) {
            gx::PipelineConfig config;
            std::memcpy(&config, data, sizeof(config));
            if (config.version == gx::GXPipelineConfigVersion)
              manifest.push_back({ShaderType::GX, config, UINT32_MAX});
            else ++rejected;
          } else if (type == static_cast<int>(ShaderType::Clear) && version == clear::ClearPipelineConfigVersion &&
                     size == sizeof(clear::PipelineConfig) && data) {
            clear::PipelineConfig config;
            std::memcpy(&config, data, sizeof(config));
            if (config.version == clear::ClearPipelineConfigVersion)
              manifest.push_back({ShaderType::Clear, config, UINT32_MAX});
            else ++rejected;
          } else ++rejected;
        }
        if (status != SQLITE_DONE) {
          failure = sqlite3_errmsg(db);
          manifest.clear();
        }
        if (rejected) {
          std::fprintf(stderr, "[gx stage prep] stage=%s rejected_rows=%u\n", stageName.c_str(), rejected);
          if (failure.empty()) failure = "unsupported or malformed configuration rows";
        }
        sqlite3_finalize(query);
      } else {
        failure = sqlite3_errmsg(db);
      }
      sqlite3_close(db);
      provenance = path;
      if (failure.empty() && manifest.empty()) failure = "no valid configurations";
    } else {
      failure = "open or schema validation failed";
    }
    if (!failure.empty()) {
      const auto count = petariManifestReadFailures.fetch_add(1, std::memory_order_relaxed) + 1;
      std::fprintf(stderr, "[gx stage prep] stage=%s manifest read failed: %s source=%s failures=%llu\n",
                   stageName.c_str(), failure.c_str(), path.c_str(), static_cast<unsigned long long>(count));
    }
  }
  if (manifest.empty()) {
    std::lock_guard lock{g_pipelineMutex};
    for (const auto& [key, known] : g_knownPipelines) {
      // Missing-stage fallback is observed history, not the speculative global queue.
      if (known.firstFrameUsed != UINT32_MAX) manifest.push_back(known);
    }
    provenance = "shared-observed-memory";
  }

  absl::flat_hash_set<PipelineRef> targets;
  absl::flat_hash_set<HashType> configs;
  const auto submit = [&](ShaderType type, const auto& config) {
    const auto key = xxh3_hash(config, static_cast<HashType>(type));
    configs.insert(key);
    if (globalBackground) remember_pipeline_config(type, config, UINT32_MAX, true);
    targets.insert(resolve_pipeline(type, config, scene, priority));
    if (offscreen.key != scene.key) targets.insert(resolve_pipeline(type, config, offscreen, priority));
  };
  for (unsigned mask = 0; mask != 8; ++mask) {
    clear::PipelineConfig config{};
    config.clearColor = (mask & 1) != 0;
    config.clearAlpha = (mask & 2) != 0;
    config.clearDepth = (mask & 4) != 0;
    submit(ShaderType::Clear, config);
  }
  for (const auto& item : manifest)
    std::visit([&](const auto& config) { submit(item.type, config); }, item.config);
  size_t pending = 0;
  {
    std::lock_guard lock{g_pipelineMutex};
    auto& stage = petariStages[stageName];
    stage.configs = std::move(configs);
    stage.targets.assign(targets.begin(), targets.end());
    for (const auto key : targets) pending += !g_pipelines.contains(key);
  }
  g_pipelineQueueCv.notify_all();
  std::fprintf(stderr, "[gx stage prep] stage=%s source=%s configs=%zu variants=%zu pending=%zu enqueue_ms=%.3f\n",
               stageName.c_str(), provenance.c_str(), manifest.size(), targets.size(), pending,
               PetariPipeline::milliseconds(PetariPipeline::Clock::now() - started));
}

extern "C" void petari_gx_pipeline_stage_wait() {
  PetariNative::HostAllocationScope petariHostAllocations;
  const auto start = PetariPipeline::Clock::now();
  size_t pending = 0, failed = 0;
  bool complete = false;
  std::string name;
  {
    std::unique_lock lock{g_pipelineMutex};
    name = petariActiveStage;
    if (name.empty()) return;
    auto targets = petariStages[name].targets;
    for (const auto& tag : petariOverlayStages) {
      const auto& extra = petariStages[tag].targets;
      targets.insert(targets.end(), extra.begin(), extra.end());
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    const auto ready = [&] {
      return g_pipelineThreadEnd || petariActiveStage != name ||
          std::all_of(targets.begin(), targets.end(), [](auto key) { return g_pipelines.contains(key); });
    };
    complete = g_pipelineReadyCv.wait_until(lock, start + std::chrono::seconds(10), ready);
    for (const auto key : targets) {
      const auto found = g_pipelines.find(key);
      if (found == g_pipelines.end()) ++pending;
      else if (!found->second.main) ++failed;
    }
    petariStages[name].prepared = true;
    for (const auto& tag : petariOverlayStages) petariStages[tag].prepared = true;
  }
  std::fprintf(stderr, "[gx stage ready] stage=%s residual_wait_ms=%.3f pending=%zu failed=%zu result=%s\n",
               name.c_str(), PetariPipeline::milliseconds(PetariPipeline::Clock::now() - start), pending, failed,
               complete ? "completed" : "timeout-proceed");
  petari_stage_flush_logs();
}

extern "C" void petari_gx_pipeline_report() {
  PetariNative::HostAllocationScope petariHostAllocations;
  petari_stage_flush_logs();
  std::lock_guard lock{g_pipelineMutex};
  petari_pipeline_summary();
}

extern "C" void petari_gx_pipeline_background_begin() {
  if (!PetariPipeline::backgroundGlobalPrecompile()) return;
  // Called on the host thread after prepareKnownPipelines returns, before game startup.
  petari_gx_pipeline_stage_begin("__global__", nullptr);
}

}

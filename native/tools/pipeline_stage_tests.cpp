// CPU-only stage scheduling tests against the production stage implementation.
#include "../gx/pipeline_profile.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include <sqlite3.h>
#include <stdexcept>
namespace absl {
template<class K, class V> using flat_hash_map = std::unordered_map<K, V>;
template<class K> using flat_hash_set = std::unordered_set<K>;
}
namespace fmt {
template<class... Args> std::string format(const char* text, Args&&...) { return text; }
}
namespace aurora::gfx {
using HashType = uint64_t;
using PipelineRef = uint64_t;
namespace gx { constexpr unsigned GXPipelineConfigVersion = 13; struct PipelineConfig { unsigned version = 13, id = 0; }; }
namespace clear { constexpr unsigned ClearPipelineConfigVersion = 1; struct PipelineConfig { unsigned version = 1; bool clearColor = false, clearAlpha = false, clearDepth = false; }; }
enum class ShaderType { Clear, GX };
enum class PipelinePriority { Background, Normal, Blocking };
struct KnownPipeline { ShaderType type; std::variant<gx::PipelineConfig, clear::PipelineConfig> config; unsigned frame; };
struct Layout { uint64_t key; unsigned colorAttachmentCount = 2, sampleCount = 4; };
static Layout scene_render_target_layout() { return {5}; }
namespace detail { static void finalize_render_target_layout(Layout& value) { value.key = 9; } }
static uint64_t xxh3_hash(uint64_t value, uint64_t seed) { return value * 7919 + seed; }
static uint64_t xxh3_hash(const gx::PipelineConfig& value, uint64_t seed) { return value.id * 100 + seed; }
static uint64_t xxh3_hash(const clear::PipelineConfig& value, uint64_t) { return 10000 + value.clearColor + value.clearAlpha * 2 + value.clearDepth * 4; }
struct Pending { PipelineRef hash; };
struct Sample { bool drawRequested = false; };
struct Compiled { bool main = true; };
static std::mutex g_pipelineMutex;
static std::condition_variable g_pipelineReadyCv, g_pipelineQueueCv;
static std::atomic<bool> g_pipelineThreadEnd{false};
static std::deque<Pending> g_pipelineQueue, g_backgroundPipelineQueue;
static std::unordered_map<PipelineRef, Sample> petariPipelineSamples;
static std::unordered_map<PipelineRef, Compiled> g_pipelines;
static std::unordered_map<HashType, KnownPipeline> g_knownPipelines;
static struct { const char* resourcesPath = "/nonexistent-petari-stage-test"; } g_config;
static auto find_pending_pipeline(std::deque<Pending>& queue, PipelineRef key) {
    return std::find_if(queue.begin(), queue.end(), [=](const auto& item) { return item.hash == key; });
}
static void promote_pending_pipeline(PipelineRef key, PipelinePriority) {
    auto it = find_pending_pipeline(g_backgroundPipelineQueue, key);
    if (it != g_backgroundPipelineQueue.end()) {
        g_pipelineQueue.push_back(*it);
        g_backgroundPipelineQueue.erase(it);
    }
}
template<class Config>
static PipelineRef resolve_pipeline(ShaderType type, const Config& config, Layout layout, PipelinePriority priority) {
    if (priority != PipelinePriority::Normal) throw std::runtime_error("stage begin must not block");
    const auto key = xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(type)));
    std::lock_guard lock(g_pipelineMutex);
    petariPipelineSamples.try_emplace(key);
    promote_pending_pipeline(key, priority);
    if (!g_pipelines.contains(key) && find_pending_pipeline(g_pipelineQueue, key) == g_pipelineQueue.end())
        g_pipelineQueue.push_back({key});
    return key;
}
static sqlite3* open_pipeline_cache_seed_db(const std::string&) { return nullptr; }
}
#include "../gx/pipeline_stage.inc"
using namespace aurora::gfx;
static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int main() {
    unsetenv("PETARI_PIPELINE_SEED_DIR");
    g_knownPipelines.emplace(1, KnownPipeline{ShaderType::GX, gx::PipelineConfig{13, 1}, 0});
    petari_gx_pipeline_stage_begin("AstroGalaxy", nullptr);
    const auto originalCount = g_pipelineQueue.size();
    require(originalCount == 18, "expected eight clear masks and one GX in two layouts");
    petari_gx_pipeline_stage_begin("AstroGalaxy", nullptr);
    require(g_pipelineQueue.size() == originalCount, "same-stage begin duplicated jobs");
    petariStages["AstroGalaxy"].prepared = true;
    petari_gx_pipeline_stage_begin("ScenarioSelect", nullptr);
    petari_gx_pipeline_stage_begin("GalaxyMap", nullptr);
    require(petariActiveStage == "AstroGalaxy" && petariStages["AstroGalaxy"].prepared,
            "overlay replaced active stage attribution or preparation status");
    require(g_backgroundPipelineQueue.empty() && g_pipelineQueue.size() == originalCount,
            "overlay demoted active jobs or duplicated them");
    petari_note_stage_config(12345, 999);
    require(petariStages["AstroGalaxy"].seen.contains(12345) && petariStages["ScenarioSelect"].seen.contains(12345),
            "first use missing active-stage or overlay attribution");
    require(petariStageLogs.size() == 3, "first-use tags missing");
    petari_note_stage_config(12345, 999);
    require(petariStageLogs.size() == 3, "duplicate first-use log");
    for (const auto& job : g_pipelineQueue) g_pipelines[job.hash] = {};
    petari_gx_pipeline_stage_wait();
    require(petariStages["AstroGalaxy"].prepared && petariStages["GalaxyMap"].prepared, "ready state not recorded");
    g_pipelineQueue.clear(); g_pipelines.clear();
    // Leave only an old speculative config and a real draw request outstanding.
    petariStages["AstroGalaxy"].targets = {100, 101};
    petariPipelineSamples[101].drawRequested = true;
    g_pipelineQueue.push_back({100}); g_pipelineQueue.push_back({101});
    petari_gx_pipeline_stage_begin("EggStarGalaxy", nullptr);
    require(petariActiveStage == "EggStarGalaxy" && petariOverlayStages.empty(), "stage switch kept obsolete overlay tags");
    require(find_pending_pipeline(g_backgroundPipelineQueue, 100) != g_backgroundPipelineQueue.end(), "old speculation not demoted");
    require(find_pending_pipeline(g_pipelineQueue, 101) != g_pipelineQueue.end(), "real draw request demoted");
    petari_stage_reset();
    require(petariActiveStage.empty() && petariStages.empty(), "shutdown retained stage state");
    std::puts("Stage preparation: idempotence, additive overlays, first-use tags, ready gate and priority demotion pass");
}

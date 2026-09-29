// Exercise the production worker body using controlled CPU-only compile jobs.
#include "../gx/pipeline_profile.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace absl {
template<class K, class V> using flat_hash_map = std::unordered_map<K, V>;
}
namespace aurora::gfx {
using PipelineRef = uint64_t;
using HashType = uint64_t;
enum class ShaderType { Clear, GX, Rml };
enum class PipelinePriority { Background, Normal, Blocking };
struct CompiledPipeline {
    bool main = false;
    uint32_t pipeline_count() const { return main ? 1 : 0; }
};
struct PendingPipeline {
    PipelineRef hash;
    std::function<CompiledPipeline()> create;
};
static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = true;
static unsigned g_pipelinesPerFrame = 0;
static constexpr unsigned BuildPipelinesPerFrame = 5;
static std::atomic<bool> g_pipelineThreadEnd{false};
static std::condition_variable g_pipelineQueueCv, g_pipelineReadyCv;
static std::deque<PendingPipeline> g_pipelineQueue, g_backgroundPipelineQueue;
static std::unordered_map<PipelineRef, CompiledPipeline> g_pipelines;
static std::unordered_set<PipelineRef> g_pendingPipelines;
static std::atomic<unsigned> queuedPipelines{0};
static void notify_pipeline_ready(bool queued, uint32_t) {
    if (queued) --queuedPipelines;
    g_pipelineReadyCv.notify_all();
}
#include "../gx/pipeline_runtime.inc"
#include "../gx/pipeline_worker.inc"

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void enqueue(uint64_t id, bool background, std::function<CompiledPipeline()> fn) {
    std::lock_guard lock(g_pipelineMutex);
    petariPipelineSamples[id].queued = PetariPipeline::Clock::now();
    (background ? g_backgroundPipelineQueue : g_pipelineQueue).push_back({id, std::move(fn)});
    g_pendingPipelines.insert(id);
    ++queuedPipelines;
    g_pipelineQueueCv.notify_one();
}
void waitReady(size_t count) {
    std::unique_lock lock(g_pipelineMutex);
    require(g_pipelineReadyCv.wait_for(lock, std::chrono::seconds(5), [=] { return g_pipelines.size() == count; }),
            "workers did not finish");
}
void stop(std::vector<std::thread>& workers) {
    g_pipelineThreadEnd = true;
    g_pipelineQueueCv.notify_all();
    for (auto& worker : workers) worker.join();
    require(queuedPipelines == 0 && g_pendingPipelines.empty(), "completion accounting is not balanced");
    g_pipelineThreadEnd = false;
    g_pipelines.clear();
    petariPipelineSamples.clear();
}
void priorityTest() {
    std::vector<int> order;
    std::promise<void> running, release;
    auto gate = release.get_future().share();
    enqueue(1, true, [&] { running.set_value(); gate.wait(); order.push_back(1); return CompiledPipeline{true}; });
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker);
    require(running.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "worker did not start");
    enqueue(2, true, [&] { order.push_back(2); return CompiledPipeline{true}; });
    enqueue(3, false, [&] { order.push_back(3); return CompiledPipeline{true}; });
    release.set_value();
    waitReady(3);
    stop(workers);
    require(order == std::vector<int>({1, 3, 2}), "requested pipeline must precede queued background work");
}
void parallelTest() {
    std::promise<void> startedA, startedB, release;
    auto gate = release.get_future().share();
    enqueue(11, false, [&] { PetariPipeline::stages.sourceMs = 11; startedA.set_value(); gate.wait(); return CompiledPipeline{true}; });
    enqueue(22, false, [&] { PetariPipeline::stages.sourceMs = 22; startedB.set_value(); gate.wait(); return CompiledPipeline{true}; });
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker);
    workers.emplace_back(pipeline_worker);
    const auto readyA = startedA.get_future().wait_for(std::chrono::seconds(5));
    const auto readyB = startedB.get_future().wait_for(std::chrono::seconds(5));
    release.set_value();
    require(readyA == std::future_status::ready && readyB == std::future_status::ready,
            "two compile jobs must be able to enter before either completes");
    waitReady(2);
    {
        std::lock_guard lock(g_pipelineMutex);
        require(petariPipelineSamples.at(11).stages.sourceMs == 11 && petariPipelineSamples.at(22).stages.sourceMs == 22,
                "compile stage accumulators leaked across worker threads");
        require(petariPipelineSamples.at(11).stages.runtimeKey == 11 && petariPipelineSamples.at(22).stages.runtimeKey == 22,
                "compile context belongs to wrong pipeline");
    }
    stop(workers);
}
}
int main(int argc, char** argv) {
    const bool expectAsync = argc == 2 && std::strcmp(argv[1], "async") == 0;
    aurora::gfx::require(PetariPipeline::asynchronous() == expectAsync, "pipeline policy is not opt-in");
    if (!std::getenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE"))
        aurora::gfx::require(!PetariPipeline::globalPrecompile(), "full global preparation must remain opt-in");
    aurora::gfx::priorityTest();
    aurora::gfx::parallelTest();
    std::puts("Pipeline worker: requested priority, concurrent progress, isolated timing state and shutdown pass");
}

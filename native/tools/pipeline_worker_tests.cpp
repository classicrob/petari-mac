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
    petariFailedPipelines = 0;
}
void priorityTest() {
    std::vector<int> order;
    std::promise<void> running, release;
    auto gate = release.get_future().share();
    enqueue(1, true, [&] { running.set_value(); gate.wait(); order.push_back(1); return CompiledPipeline{true}; });
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker, static_cast<unsigned>(workers.size()));
    require(running.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "worker did not start");
    enqueue(2, true, [&] { order.push_back(2); return CompiledPipeline{true}; });
    enqueue(3, false, [&] { order.push_back(3); return CompiledPipeline{true}; });
    release.set_value();
    waitReady(3);
    stop(workers);
    require(order == std::vector<int>({1, 3, 2}), "requested pipeline must precede queued background work");
}
void parallelTest() {
    petariActiveWorkers = 2;
    std::promise<void> startedA, startedB, release;
    auto gate = release.get_future().share();
    enqueue(11, false, [&] { PetariPipeline::stages.sourceMs = 11; startedA.set_value(); gate.wait(); return CompiledPipeline{true}; });
    enqueue(22, false, [&] { PetariPipeline::stages.sourceMs = 22; startedB.set_value(); gate.wait(); return CompiledPipeline{true}; });
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker, static_cast<unsigned>(workers.size()));
    workers.emplace_back(pipeline_worker, static_cast<unsigned>(workers.size()));
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
void retirementTest() {
    const auto normal = PetariPipeline::workerCount();
    petariActiveWorkers = normal + 1;
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    enqueue(41, true, [&] { started.set_value(); gate.wait(); return CompiledPipeline{true}; });
    std::thread extra(pipeline_worker, normal);
    const auto entered = started.get_future().wait_for(std::chrono::seconds(5));
    enqueue(42, true, [] { return CompiledPipeline{true}; });
    petari_gx_pipeline_startup_finished();
    release.set_value();
    extra.join();
    require(entered == std::future_status::ready, "extra startup worker did not enter");
    require(g_pipelines.size() == 1 && g_pendingPipelines.count(42),
            "extra worker took another compile after startup ended");
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker, 0);
    waitReady(2);
    stop(workers);
}
void backgroundCapTest() {
    petariActiveWorkers = 2;
    petariBackgroundCap = 1;
    std::promise<void> startedA, release;
    std::atomic<bool> startedB{false}, startedC{false};
    auto gate = release.get_future().share();
    enqueue(51, true, [&] { startedA.set_value(); gate.wait(); return CompiledPipeline{true}; });
    enqueue(52, true, [&] { startedB = true; return CompiledPipeline{true}; });
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker, 0u);
    workers.emplace_back(pipeline_worker, 1u);
    require(startedA.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready, "backlog job did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!startedB, "second backlog job exceeded the gameplay concurrency cap");
    enqueue(53, false, [&] { startedC = true; return CompiledPipeline{true}; });
    {
        std::unique_lock lock(g_pipelineMutex);
        require(g_pipelineReadyCv.wait_for(lock, std::chrono::seconds(5), [] { return g_pipelines.count(53) != 0; }),
                "requested job waited behind the backlog cap");
    }
    require(!startedB, "cap released early");
    release.set_value();
    waitReady(3);
    require(startedB && petariBackgroundInFlight == 0, "backlog did not resume after its slot opened");
    stop(workers);
    petariBackgroundCap = ~0u;
}
void gameplayThrottleTest() {
    // During gameplay: two stage-speculative compiles at a time, the global
    // backlog waits while stage work is queued, a draw-requested job starts at
    // once, and leaving gameplay releases everything parked.
    petariActiveWorkers = 4;
    petari_gx_pipeline_set_gameplay(true);
    std::promise<void> startedA, startedB, release;
    std::atomic<bool> startedC{false}, startedG{false};
    auto gate = release.get_future().share();
    enqueue(71, false, [&] { startedA.set_value(); gate.wait(); return CompiledPipeline{true}; });
    enqueue(72, false, [&] { startedB.set_value(); gate.wait(); return CompiledPipeline{true}; });
    enqueue(75, false, [&] { startedC = true; return CompiledPipeline{true}; });
    enqueue(73, true, [&] { startedG = true; return CompiledPipeline{true}; });
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i) workers.emplace_back(pipeline_worker, i);
    require(startedA.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
            startedB.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "two stage jobs did not start during gameplay");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!startedC, "gameplay exceeded the stage speculation cap");
    require(!startedG, "global backlog ran ahead of queued stage work");
    {
        std::lock_guard lock(g_pipelineMutex);
        petariPipelineSamples[74].drawBlocking = true;
        petariPipelineSamples[74].queued = PetariPipeline::Clock::now();
        g_pipelineQueue.push_front({74, [] { return CompiledPipeline{true}; }});
        g_pendingPipelines.insert(74);
        ++queuedPipelines;
    }
    g_pipelineQueueCv.notify_one();
    {
        std::unique_lock lock(g_pipelineMutex);
        require(g_pipelineReadyCv.wait_for(lock, std::chrono::seconds(5), [] { return g_pipelines.count(74) != 0; }),
                "draw-requested compile was throttled");
    }
    require(!startedC && !startedG, "throttle released early");
    petari_gx_pipeline_set_gameplay(false);
    {
        std::unique_lock lock(g_pipelineMutex);
        require(g_pipelineReadyCv.wait_for(lock, std::chrono::seconds(5), [] { return g_pipelines.count(75) && g_pipelines.count(73); }),
                "leaving gameplay did not release speculative work");
    }
    release.set_value();
    waitReady(5);
    require(petariStageSpeculativeInFlight == 0 && petariBackgroundInFlight == 0, "speculative accounting unbalanced");
    stop(workers);
}
void boundedDrawTest() {
    // Nothing compiles: the first wait spends the frame budget, the next one skips at once.
    const auto ready = [] { return g_pipelines.count(61) != 0; };
    const auto before = petariDeferredDraws;
    petari_pipeline_frame_budget();
    std::unique_lock lock(g_pipelineMutex);
    auto start = PetariPipeline::Clock::now();
    petari_bounded_draw_wait(lock, ready);
    const auto first = PetariPipeline::Clock::now() - start;
    start = PetariPipeline::Clock::now();
    petari_bounded_draw_wait(lock, ready);
    const auto second = PetariPipeline::Clock::now() - start;
    lock.unlock();
    require(first >= PetariPipeline::drawWaitBudget() - std::chrono::microseconds(200) &&
            first < PetariPipeline::drawWaitBudget() + std::chrono::milliseconds(50), "first draw wait ignored the budget");
    require(second < std::chrono::milliseconds(2), "exhausted frame budget still waited");
    require(petariDeferredDraws == before + 2, "deferred draws not counted");
    // A new frame restores the budget; a compile landing inside it is not deferred.
    petari_pipeline_frame_budget();
    std::thread finisher([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard guard(g_pipelineMutex);
        g_pipelines[61] = CompiledPipeline{true};
        g_pipelineReadyCv.notify_all();
    });
    lock.lock();
    petari_bounded_draw_wait(lock, ready);
    lock.unlock();
    finisher.join();
    require(petariDeferredDraws == before + 2, "ready pipeline counted as deferred");
    g_pipelines.clear();
}
void progressTest() {
    enqueue(31, true, [] { return CompiledPipeline{false}; });
    uint32_t total, pending, failed;
    petari_gx_pipeline_preparation_status(&total, &pending, &failed);
    require(total == 1 && pending == 1 && failed == 0, "queued progress is inconsistent");
    std::vector<std::thread> workers;
    workers.emplace_back(pipeline_worker, static_cast<unsigned>(workers.size()));
    waitReady(1);
    petari_gx_pipeline_preparation_status(&total, &pending, &failed);
    require(total == 1 && pending == 0 && failed == 1, "failed completion was reported as a successful warmup");
    stop(workers);
}
}
int main(int argc, char** argv) {
    const bool expectAsync = argc == 2 && std::strcmp(argv[1], "async") == 0;
    aurora::gfx::require(PetariPipeline::asynchronous() == expectAsync, "pipeline policy is not opt-in");
    if (!std::getenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE"))
        aurora::gfx::require(!PetariPipeline::globalPrecompile() && PetariPipeline::backgroundGlobalPrecompile(),
                             "background preparation (no blocking screen) must be the default");
    setenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE", "0", 1);
    aurora::gfx::require(!PetariPipeline::globalPrecompile() && !PetariPipeline::backgroundGlobalPrecompile(), "off override ignored");
    setenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE", "background", 1);
    aurora::gfx::require(!PetariPipeline::globalPrecompile() && PetariPipeline::backgroundGlobalPrecompile(), "background override ignored");
    setenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE", "1", 1);
    aurora::gfx::require(PetariPipeline::globalPrecompile() && !PetariPipeline::backgroundGlobalPrecompile(),
                         "explicit full preparation ignored");
    for (const auto cores : {0u, 1u, 2u, 4u, 6u, 8u, 9u})
        aurora::gfx::require(PetariPipeline::defaultStartupWorkerCount(cores) == 4,
                             "small-core startup default must remain four");
    for (const auto cores : {10u, 11u})
        aurora::gfx::require(PetariPipeline::defaultStartupWorkerCount(cores) == 5,
                             "startup default must use half the performance cores");
    for (const auto cores : {12u, 14u, 24u, 128u})
        aurora::gfx::require(PetariPipeline::defaultStartupWorkerCount(cores) == 6,
                             "startup default must cap at six");
    unsetenv("PETARI_PIPELINE_STARTUP_THREADS");
    const auto automatic = PetariPipeline::defaultStartupWorkerCount(PetariPipeline::performanceCores());
    aurora::gfx::require(PetariPipeline::startupWorkerCount() == automatic,
                         "startup default did not use performance core count");
    for (const auto value : {"", "0", "33", "invalid", "6junk"}) {
        setenv("PETARI_PIPELINE_STARTUP_THREADS", value, 1);
        aurora::gfx::require(PetariPipeline::startupWorkerCount() == automatic,
                             "invalid startup override did not fall back to automatic");
    }
    setenv("PETARI_PIPELINE_STARTUP_THREADS", "10", 1);
    aurora::gfx::require(PetariPipeline::startupWorkerCount() == 10, "startup tuning override ignored");
    setenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE", "background", 1);
    aurora::gfx::require(PetariPipeline::startupWorkerCount() == PetariPipeline::workerCount(), "background used startup pool");
    unsetenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE");
    aurora::gfx::require(PetariPipeline::startupWorkerCount() == PetariPipeline::workerCount(), "default used startup pool");
    unsetenv("PETARI_PIPELINE_STARTUP_THREADS");
    unsetenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE");
    aurora::gfx::priorityTest();
    aurora::gfx::parallelTest();
    aurora::gfx::progressTest();
    aurora::gfx::retirementTest();
    aurora::gfx::backgroundCapTest();
    aurora::gfx::gameplayThrottleTest();
    aurora::gfx::boundedDrawTest();
    std::puts("Pipeline worker: requested priority, concurrent progress, isolated timing state, backlog cap, gameplay throttle, bounded draw waits and shutdown pass");
}

#pragma once

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <petari/frame_telemetry.hpp>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <pthread.h>
#endif

namespace PetariPipeline {
using Clock = std::chrono::steady_clock;
inline double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}
inline bool diagnostics() {
    static const bool enabled = [] {
        const char* diagnostic = std::getenv("PETARI_PIPELINE_DIAG");
        const char* trace = std::getenv("PETARI_TRACE_BOOT");
        return (diagnostic && *diagnostic && *diagnostic != '0') ||
               (trace && *trace && *trace != '0');
    }();
    return enabled;
}
inline bool asynchronous() {
    static const bool enabled = [] {
        const char* policy = std::getenv("PETARI_PIPELINE_POLICY");
        return policy && std::strcmp(policy, "async") == 0;
    }();
    return enabled;
}
// PETARI_PIPELINE_POLICY=blocking restores unbounded GX draw waits. The default
// waits for a pending GX pipeline only within a small per-frame budget and then
// skips that draw until its compile lands (clear pipelines always block).
inline bool unboundedBlocking() {
    static const bool enabled = [] {
        const char* policy = std::getenv("PETARI_PIPELINE_POLICY");
        return policy && std::strcmp(policy, "blocking") == 0;
    }();
    return enabled;
}
inline unsigned millisecondsSetting(const char* name, unsigned fallback, unsigned maximum) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    return end && *end == '\0' && parsed <= maximum ? static_cast<unsigned>(parsed) : fallback;
}
// Total time the GX recording thread may wait for pending pipelines per frame.
inline Clock::duration drawWaitBudget() {
    static const unsigned ms = millisecondsSetting("PETARI_PIPELINE_DRAW_BUDGET_MS", 6, 100);
    return std::chrono::milliseconds(ms);
}
// Stage gate: how long scene start may wait for the stage manifest. Draws do not
// need it for correctness; it only reduces pop-in when compiles are nearly done.
inline Clock::duration stageGateBudget() {
    static const unsigned ms = millisecondsSetting("PETARI_STAGE_GATE_MS", 1500, 10000);
    return std::chrono::milliseconds(ms);
}
// Set on the GX recording thread while resolving a GX draw pipeline.
inline thread_local bool boundedDraw = false;
struct BoundedDrawScope {
    explicit BoundedDrawScope(bool enabled) { boundedDraw = enabled; }
    ~BoundedDrawScope() { boundedDraw = false; }
    BoundedDrawScope(const BoundedDrawScope&) = delete;
    BoundedDrawScope& operator=(const BoundedDrawScope&) = delete;
};
inline unsigned performanceCores() {
#if defined(__APPLE__)
    unsigned cores = 0;
    size_t size = sizeof(cores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores) return cores;
    size = sizeof(cores);
    if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores) return cores;
#endif
    return 2;
}
inline unsigned workerCount() {
    const unsigned cores = performanceCores();
    const unsigned automatic = std::min(4u, cores > 4 ? cores - 4 : 1u);
    const char* value = std::getenv("PETARI_PIPELINE_THREADS");
    if (!value || !*value) return automatic;
    char* end = nullptr;
    const auto count = std::strtoul(value, &end, 10);
    return end && *end == '\0' && count >= 1 && count <= 4 ? static_cast<unsigned>(count) : automatic;
}
// Blocking full preparation screen: opt-in only (PETARI_PIPELINE_GLOBAL_PRECOMPILE=1).
inline bool globalPrecompile() {
    const char* value = std::getenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE");
    return value && std::strcmp(value, "1") == 0;
}
inline unsigned defaultStartupWorkerCount(unsigned performanceCoreCount) {
    return std::clamp(performanceCoreCount / 2, 4u, 6u);
}
inline unsigned startupWorkerCount() {
    const unsigned normal = workerCount();
    if (!globalPrecompile()) return normal;
    const unsigned automatic = defaultStartupWorkerCount(performanceCores());
    const char* value = std::getenv("PETARI_PIPELINE_STARTUP_THREADS");
    if (!value || !*value) return automatic;
    char* end = nullptr;
    const auto count = std::strtoul(value, &end, 10);
    return end && *end == '\0' && count >= normal && count <= 32 ? static_cast<unsigned>(count) : automatic;
}
// Default: start the game at once and compile the global union in the background.
inline bool backgroundGlobalPrecompile() {
    const char* value = std::getenv("PETARI_PIPELINE_GLOBAL_PRECOMPILE");
    return !value || !*value || std::strcmp(value, "background") == 0;
}
// Concurrent global-backlog compiles while the game runs. Stage and draw
// requests may use every worker; the speculative backlog leaves cores free.
inline unsigned backgroundWorkerCap() {
    return std::max(1u, workerCount() / 2);
}
// Game, render and audio threads run at user-interactive or real-time priority,
// above both compile classes: stage/draw requests use user-initiated, the global backlog utility.
inline void compilationQoS(bool background) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
#else
    (void)background;
#endif
}
struct Stages {
    std::uint64_t runtimeKey = 0;
    double sourceMs = 0, moduleMs = 0, pipelineMs = 0, dumpMs = 0;
    std::uint64_t sourceBytes = 0, sourceLines = 0;
    unsigned shaderCount = 0, maxTevStages = 0, maxIndStages = 0;
};
inline thread_local Stages stages;

inline void dumpSource(std::uint64_t shaderKey, std::string_view source) {
    const char* directory = std::getenv("PETARI_PIPELINE_DUMP");
    if (!directory || !*directory) return;
    const auto start = Clock::now();
    std::error_code error;
    const std::filesystem::path root(directory);
    std::filesystem::create_directories(root, error);
    char filename[80];
    std::snprintf(filename, sizeof(filename), "%016llx-%016llx.wgsl",
                  static_cast<unsigned long long>(stages.runtimeKey),
                  static_cast<unsigned long long>(shaderKey));
    if (!error) {
        std::ofstream output(root / filename, std::ios::binary);
        output.write(source.data(), static_cast<std::streamsize>(source.size()));
        if (!output) std::fprintf(stderr, "[gx pipeline] could not write shader dump %s\n", filename);
    } else {
        std::fprintf(stderr, "[gx pipeline] could not create shader dump directory: %s\n", error.message().c_str());
    }
    stages.dumpMs += milliseconds(Clock::now() - start);
}
}

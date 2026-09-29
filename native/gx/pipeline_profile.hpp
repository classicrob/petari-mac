#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

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
inline unsigned workerCount() {
    const char* value = std::getenv("PETARI_PIPELINE_THREADS");
    if (!value || !*value) return 2;
    char* end = nullptr;
    const auto count = std::strtoul(value, &end, 10);
    return end && *end == '\0' && count >= 1 && count <= 4 ? static_cast<unsigned>(count) : 2;
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

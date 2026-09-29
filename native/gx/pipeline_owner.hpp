#pragma once
#include <petari/host_allocation.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <execinfo.h>
#include <dlfcn.h>

extern "C" void GXInsertDebugMarker(const char* label);
namespace PetariPipelineOwner {
inline thread_local uint64_t current = 0;
inline bool enabled() {
    static const bool value = [] {
        const auto* flag = std::getenv("PETARI_PIPELINE_OWNERS");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    return value;
}
inline void emit() {
    if (!enabled()) return;
    PetariNative::HostAllocationScope host;
    void* stack[24];
    const int count = backtrace(stack, 24);
    const std::string key(reinterpret_cast<const char*>(stack), count * sizeof(void*));
    static std::mutex mutex;
    static std::unordered_map<std::string, uint64_t> ids;
    uint64_t id;
    {
        std::lock_guard guard(mutex);
        const auto [it, inserted] = ids.try_emplace(key, ids.size() + 1);
        id = it->second;
        if (inserted) {
            auto** names = backtrace_symbols(stack, count);
            for (int frame = 0; frame < count; ++frame) {
                Dl_info image{};
                dladdr(stack[frame], &image);
                std::fprintf(stderr, "[gx draw owner] owner=%llu frame=%d pc=%p image_base=%p symbol=%s\n",
                             static_cast<unsigned long long>(id), frame, stack[frame], image.dli_fbase,
                             names ? names[frame] : "unknown");
            }
            std::free(names);
        }
    }
    char label[64];
    std::snprintf(label, sizeof(label), "petari-pipeline-owner:%llu", static_cast<unsigned long long>(id));
    GXInsertDebugMarker(label);
}
inline bool consume(const std::string& label) {
    constexpr const char* prefix = "petari-pipeline-owner:";
    if (!label.starts_with(prefix)) return false;
    current = std::strtoull(label.c_str() + std::strlen(prefix), nullptr, 10);
    return true;
}
}

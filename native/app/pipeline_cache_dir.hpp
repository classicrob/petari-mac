#pragma once
#include <filesystem>
#include <string>

namespace PetariNative::App {
// Where Aurora keeps pipeline_cache.db and dawn_cache.db for a non-background
// launch. Shader preparation belongs to the machine, not to a save directory:
// a normal launch uses ~/Library/Caches/Petari, so copying or switching --user
// directories stays warm. PETARI_CACHE_DIR overrides it. Test runs (fixtures,
// PETARI_SMOKE, unlocked-save) keep the old private <user> location.
class PipelineCacheDir {
public:
    PipelineCacheDir() = default;
    PipelineCacheDir(const PipelineCacheDir&) = delete;
    PipelineCacheDir& operator=(const PipelineCacheDir&) = delete;
    ~PipelineCacheDir();
    // Never fails the launch: any problem falls back to the user directory.
    void prepare(const std::filesystem::path& user, bool testRun, const char* cacheOverride);
    std::filesystem::path path;
private:
    int lock = -1;
};
}

#pragma once
#include <filesystem>
#include <string>

namespace PetariNative::App {
// Lives until process exit, including the fixture's immediate shutdown path.
class SmokeStorage {
public:
    SmokeStorage() = default;
    SmokeStorage(const SmokeStorage&) = delete;
    SmokeStorage& operator=(const SmokeStorage&) = delete;
    ~SmokeStorage();
    bool prepare(const std::filesystem::path& user, const char* cacheOverride, std::string* error);
    std::filesystem::path cache;
private:
    std::filesystem::path metalNamespace;
    std::filesystem::path tempNamespace;
    int userLock = -1;
    int cacheLock = -1;
};
}

#include "smoke_storage.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <copyfile.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sqlite3.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace PetariNative::App {
namespace {
namespace fs = std::filesystem;

int lockDirectory(const fs::path& directory) {
    fs::create_directories(directory);
    const int fd = open((directory / ".petari-instance.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("directory already in use or cannot be locked: " + directory.string());
    }
    return fd;
}

fs::path darwinDirectory(int key = _CS_DARWIN_USER_CACHE_DIR) {
    const size_t size = confstr(key, nullptr, 0);
    if (!size) throw std::runtime_error("cannot resolve Darwin cache directory");
    std::vector<char> path(size);
    if (!confstr(key, path.data(), path.size()))
        throw std::runtime_error("cannot read Darwin cache directory");
    auto result = fs::path(path.data()).lexically_normal();
    if (result.filename().empty()) result = result.parent_path();
    return result;
}

void seedDatabase(const fs::path& source, const fs::path& destination) {
    if (!fs::is_regular_file(source) || fs::exists(destination)) return;
    sqlite3* from = nullptr;
    sqlite3* to = nullptr;
    bool ok = sqlite3_open_v2(source.c_str(), &from, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
              sqlite3_open(destination.c_str(), &to) == SQLITE_OK;
    if (ok) {
        sqlite3_backup* backup = sqlite3_backup_init(to, "main", from, "main");
        ok = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
        if (backup) ok = sqlite3_backup_finish(backup) == SQLITE_OK && ok;
    }
    if (to) sqlite3_close(to);
    if (from) sqlite3_close(from);
    if (!ok) {
        fs::remove(destination);
        throw std::runtime_error("cannot snapshot cache " + source.string());
    }
}


std::string executableSha256() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::ifstream in(fs::weakly_canonical(buffer.data()), std::ios::binary);
    if (!in) return {};
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<char> chunk(1 << 20);
    while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || in.gcount() > 0)
        CC_SHA256_Update(&context, chunk.data(), static_cast<CC_LONG>(in.gcount()));
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

// A canonical seed is <dir>/seed.json {"exe_sha256": "..."} plus <dir>/metal/dev.petari.Petari.
// It is used only when the recorded build hash equals this executable's; otherwise
// (missing, stale, unreadable) the caller falls back to the shared Darwin cache.
std::filesystem::path canonicalMetalSeed(const char* directory, std::string* why) {
    if (!directory || !*directory) return {};
    const fs::path root(directory);
    std::ifstream in(root / "seed.json");
    if (!in) { *why = "no seed.json"; return {}; }
    std::stringstream text;
    text << in.rdbuf();
    const auto body = text.str();
    const auto key = body.find("\"exe_sha256\"");
    const auto open = key == std::string::npos ? key : body.find('"', body.find(':', key) + 1);
    const auto close = open == std::string::npos ? open : body.find('"', open + 1);
    if (close == std::string::npos) { *why = "seed.json has no exe_sha256"; return {}; }
    const auto recorded = body.substr(open + 1, close - open - 1);
    const auto current = executableSha256();
    if (current.empty() || recorded != current) { *why = "stale (seed build " + recorded.substr(0, 12) + ", this build " + current.substr(0, 12) + ")"; return {}; }
    if (!fs::is_directory(root / "metal/dev.petari.Petari")) { *why = "no metal data"; return {}; }
    return root / "metal/dev.petari.Petari";
}

void seedMetal(const fs::path& source, const fs::path& destination) {
    if (!fs::is_directory(source) || fs::exists(destination)) return;
    const auto partial = fs::path(destination.string() + ".partial");
    fs::remove_all(partial);
    fs::create_directories(partial);
    size_t count = 0;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(source)) {
            const auto target = partial / entry.path().lexically_relative(source);
            // Never follow a shared cache symlink into an unrelated directory.
            if (entry.is_symlink()) continue;
            if (entry.is_directory()) fs::create_directories(target);
            else if (entry.is_regular_file()) {
                // APFS clones are independent files, including later appends.
                if (copyfile(entry.path().c_str(), target.c_str(), nullptr, COPYFILE_ALL | COPYFILE_CLONE) != 0)
                    throw std::runtime_error("cannot seed Metal cache " + entry.path().string());
                ++count;
            }
        }
        fs::rename(partial, destination);
        std::fprintf(stderr, "PETARI SMOKE CACHE SEED: %zu Metal files copied read-only from %s\n", count, source.c_str());
    } catch (...) {
        fs::remove_all(partial);
        throw;
    }
}
}

SmokeStorage::~SmokeStorage() {
    for (const auto& entry : {std::pair{metalNamespace, cache / "metal"}, std::pair{tempNamespace, cache / "tmp"}}) {
        std::error_code ec;
        if (!entry.first.empty() && fs::is_symlink(entry.first, ec) && fs::read_symlink(entry.first, ec) == entry.second)
            fs::remove(entry.first, ec);
    }
    if (cacheLock >= 0) close(cacheLock);
    if (userLock >= 0) close(userLock);
}

bool SmokeStorage::prepare(const fs::path& user, const char* cacheOverride, std::string* error) {
    try {
        const auto realUser = fs::weakly_canonical(user);
        const char* home = std::getenv("HOME");
        if (home && realUser == fs::weakly_canonical(fs::path(home) / "Library/Application Support/Petari"))
            throw std::runtime_error("background automation requires an isolated --user directory");
        userLock = lockDirectory(realUser);
        cache = fs::weakly_canonical(cacheOverride && *cacheOverride ? fs::path(cacheOverride) : realUser / "cache");
        if (cache == realUser) throw std::runtime_error("background cache must be separate from the user root");
        using SetSuffix = int (*)(const char*);
        const auto setSuffix = reinterpret_cast<SetSuffix>(dlsym(RTLD_DEFAULT, "_set_user_dir_suffix"));
        if (!setSuffix || !setSuffix(nullptr)) throw std::runtime_error("cannot access Darwin cache namespace");
        const auto shared = darwinDirectory();
        const auto canonicalShared = fs::weakly_canonical(shared);
        const auto relative = cache.lexically_relative(canonicalShared);
        if (!relative.empty() && *relative.begin() != "..")
            throw std::runtime_error("background cache cannot use the shared Darwin cache directory");
        cacheLock = lockDirectory(cache);
        seedDatabase(realUser / "dawn_cache.db", cache / "dawn_cache.db");
        seedDatabase(realUser / "pipeline_cache.db", cache / "pipeline_cache.db");
        // A stable per-fixture namespace keeps warm data reusable without sharing writes.
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : cache.string()) { hash ^= c; hash *= 1099511628211ULL; }
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), "PetariSmoke-%016llx", static_cast<unsigned long long>(hash));
        const auto metal = cache / "metal";
        fs::create_directories(metal);
        std::string seedWhy;
        const auto canonical = canonicalMetalSeed(std::getenv("PETARI_SHADER_SEED_DIR"), &seedWhy);
        if (!canonical.empty()) {
            std::fprintf(stderr, "PETARI SMOKE SHADER SEED: canonical %s\n", canonical.c_str());
            seedMetal(canonical, metal / "dev.petari.Petari");
        } else {
            if (std::getenv("PETARI_SHADER_SEED_DIR"))
                std::fprintf(stderr, "PETARI SMOKE SHADER SEED: canonical unusable (%s); falling back to shared cache\n", seedWhy.c_str());
            seedMetal(shared / "dev.petari.Petari", metal / "dev.petari.Petari");
        }
        if (!setSuffix(suffix)) throw std::runtime_error("cannot isolate Darwin cache namespace");
        const auto isolated = darwinDirectory();
        if (isolated == shared || isolated.filename() != suffix)
            throw std::runtime_error("Darwin cache isolation was not applied");
        // Metal insists on a Darwin cache path. Only this owned namespace aliases
        // the fixture; no real user cache directory is modified or written back.
        if (fs::is_symlink(isolated)) {
            if (fs::read_symlink(isolated) != metal) throw std::runtime_error("cache namespace belongs to another fixture");
        } else {
            if (fs::exists(isolated) && (!fs::is_directory(isolated) || !fs::is_empty(isolated) || !fs::remove(isolated)))
                throw std::runtime_error("cache namespace unexpectedly contains data");
            fs::create_directory_symlink(metal, isolated);
        }
        metalNamespace = isolated;
        const auto temporary = cache / "tmp";
        fs::create_directories(temporary);
        const auto temporaryAlias = darwinDirectory(_CS_DARWIN_USER_TEMP_DIR);
        if (temporaryAlias.filename() != suffix) throw std::runtime_error("Darwin temp isolation was not applied");
        // Darwin's temporary namespace is OS-managed and may be protected
        // against replacement. Its unique suffix already isolates instances.
        fs::create_directories(temporaryAlias);
        tempNamespace = temporaryAlias;
        if (setenv("TMPDIR", temporary.c_str(), 1) != 0) throw std::runtime_error("cannot isolate temporary directory");
        std::fprintf(stderr, "PETARI SMOKE STORAGE: user=%s cache=%s metal=%s namespace=%s temp=%s temp_namespace=%s\n",
                     realUser.c_str(), cache.c_str(), metal.c_str(), isolated.c_str(), temporary.c_str(), temporaryAlias.c_str());
        return true;
    } catch (const std::exception& e) {
        *error = e.what();
        return false;
    }
}
}

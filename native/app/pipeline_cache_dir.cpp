#include "pipeline_cache_dir.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>

namespace PetariNative::App {
namespace {
namespace fs = std::filesystem;

// Consistent snapshot of a user-directory cache into a fresh machine cache.
bool snapshot(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || fs::exists(destination, ec)) return false;
    const auto partial = fs::path(destination.string() + ".partial");
    fs::remove(partial, ec);
    sqlite3* from = nullptr;
    sqlite3* to = nullptr;
    bool ok = sqlite3_open_v2(source.c_str(), &from, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
              sqlite3_open(partial.c_str(), &to) == SQLITE_OK;
    if (ok) {
        sqlite3_backup* backup = sqlite3_backup_init(to, "main", from, "main");
        ok = backup && sqlite3_backup_step(backup, -1) == SQLITE_DONE;
        if (backup) ok = sqlite3_backup_finish(backup) == SQLITE_OK && ok;
    }
    if (to) sqlite3_close(to);
    if (from) sqlite3_close(from);
    if (ok) fs::rename(partial, destination, ec);
    if (!ok || ec) {
        fs::remove(partial, ec);
        std::fprintf(stderr, "[gx pipeline cache] could not copy %s; starting that cache empty\n", source.c_str());
        return false;
    }
    return true;
}
}

PipelineCacheDir::~PipelineCacheDir() {
    if (lock >= 0) close(lock);
}

void PipelineCacheDir::prepare(const fs::path& user, bool testRun, const char* cacheOverride) {
    path = user;
    std::error_code ec;
    if (cacheOverride && *cacheOverride) {
        path = cacheOverride;
        fs::create_directories(path, ec);
        if (ec) path = user;
        std::fprintf(stderr, "[gx pipeline cache] dir=%s source=%s\n", path.c_str(),
                     ec ? "user-fallback" : "PETARI_CACHE_DIR");
        return;
    }
    const char* home = std::getenv("HOME");
    if (testRun || !home || !*home) {
        std::fprintf(stderr, "[gx pipeline cache] dir=%s source=%s\n", path.c_str(),
                     testRun ? "user-test-run" : "user-no-home");
        return;
    }
    const auto machine = fs::path(home) / "Library/Caches/Petari";
    fs::create_directories(machine, ec);
    int fd = ec ? -1 : open((machine / ".petari-instance.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        // Another normal launch owns the machine cache; do not share SQLite writers.
        std::fprintf(stderr, "[gx pipeline cache] dir=%s source=user-machine-cache-busy\n", path.c_str());
        return;
    }
    lock = fd;
    path = machine;
    // One-time adoption of an existing per-save cache keeps an old install warm.
    unsigned copied = 0;
    for (const char* name : {"pipeline_cache.db", "dawn_cache.db"}) copied += snapshot(user / name, machine / name);
    std::fprintf(stderr, "[gx pipeline cache] dir=%s source=machine adopted_from_user=%u\n", path.c_str(), copied);
}
}

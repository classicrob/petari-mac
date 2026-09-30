// Machine-wide pipeline cache selection, adoption of a per-save cache, busy and test-run fallbacks.
#include "../app/pipeline_cache_dir.hpp"
#include <sqlite3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using PetariNative::App::PipelineCacheDir;

static void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
static void makeDb(const fs::path& path, int rows) {
    sqlite3* db = nullptr;
    require(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "fixture open failed");
    std::string sql = "CREATE TABLE pipeline_cache(hash INTEGER);";
    for (int i = 0; i < rows; ++i) sql += "INSERT INTO pipeline_cache VALUES(" + std::to_string(i) + ");";
    require(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "fixture write failed");
    sqlite3_close(db);
}
static int rows(const fs::path& path) {
    sqlite3* db = nullptr;
    int count = -1;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* query = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM pipeline_cache", -1, &query, nullptr) == SQLITE_OK &&
            sqlite3_step(query) == SQLITE_ROW)
            count = sqlite3_column_int(query, 0);
        sqlite3_finalize(query);
    }
    sqlite3_close(db);
    return count;
}

int main() try {
    const auto root = fs::temp_directory_path() /
        ("petari-cache-dir-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto home = root / "home", save = root / "save", copy = root / "copied-save";
    fs::create_directories(save);
    fs::create_directories(copy);
    makeDb(save / "pipeline_cache.db", 3);
    makeDb(save / "dawn_cache.db", 2);
    setenv("HOME", home.c_str(), 1);
    const auto machine = home / "Library/Caches/Petari";
    {
        PipelineCacheDir first;
        first.prepare(save, false, nullptr);
        require(first.path == machine, "normal launch did not use the machine cache");
        require(rows(machine / "pipeline_cache.db") == 3 && rows(machine / "dawn_cache.db") == 2,
                "existing per-save caches were not adopted");
        PipelineCacheDir second;
        second.prepare(copy, false, nullptr);
        require(second.path == copy, "a second concurrent launch shared the machine cache writer");
    }
    makeDb(copy / "pipeline_cache.db", 7);
    {
        PipelineCacheDir again;
        again.prepare(copy, false, nullptr);
        require(again.path == machine, "machine cache lock not released at exit");
        require(rows(machine / "pipeline_cache.db") == 3, "a copied save's cache replaced the warm machine cache");
    }
    {
        PipelineCacheDir test;
        test.prepare(copy, true, nullptr);
        require(test.path == copy, "test runs must keep their private user-directory cache");
        PipelineCacheDir overridden;
        overridden.prepare(copy, true, (root / "override").c_str());
        require(overridden.path == root / "override" && fs::is_directory(root / "override"), "PETARI_CACHE_DIR ignored");
    }
    require(!fs::exists(machine / "pipeline_cache.db.partial"), "partial adoption file left behind");
    fs::remove_all(root);
    std::puts("Pipeline cache dir: machine cache, one-time adoption, busy fallback, test-run isolation and override pass");
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}

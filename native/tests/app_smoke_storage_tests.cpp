#include "../app/smoke_storage.hpp"
#include <mach-o/dyld.h>
#include <sys/wait.h>
#include <cstdlib>
#include <unistd.h>
#include <sqlite3.h>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::current_path() / ("smoke-storage-test-" + std::to_string(getpid()));
    fs::create_directories(root / "first");
    sqlite3* db = nullptr;
    sqlite3_open((root / "first/dawn_cache.db").c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE evidence(value); INSERT INTO evidence VALUES(42)", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    PetariNative::App::SmokeStorage first;
    std::string error;
    if (!first.prepare(root / "first", nullptr, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str()); return 1;
    }
    if (!fs::is_regular_file(first.cache / "dawn_cache.db")) return 2;
    const pid_t child = fork();
    if (child == 0) {
        PetariNative::App::SmokeStorage conflict;
        if (conflict.prepare(root / "first", nullptr, &error)) _exit(3);
        {
            PetariNative::App::SmokeStorage cacheConflict;
            const std::string occupied = first.cache.string();
            if (cacheConflict.prepare(root / "other", occupied.c_str(), &error)) _exit(8);
        }
        {
            PetariNative::App::SmokeStorage second;
            if (!second.prepare(root / "second", nullptr, &error)) _exit(4);
            std::ofstream(second.cache / "sentinel") << "second only";
        }
        _exit(0);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 5;
    if (fs::exists(first.cache / "sentinel") || !fs::exists(root / "second/cache/sentinel")) return 6;
    sqlite3_open((first.cache / "dawn_cache.db").c_str(), &db);
    sqlite3_exec(db, "UPDATE evidence SET value=99", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    sqlite3_open_v2((root / "first/dawn_cache.db").c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "SELECT value FROM evidence", -1, &statement, nullptr);
    const bool untouched = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 42;
    sqlite3_finalize(statement); sqlite3_close(db);
    if (!untouched) return 7;
    // Canonical shader seed: used only when its recorded build hash equals this executable's.
    {
        char path[4096];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) != 0) return 9;
        std::string digest;
        if (FILE* pipe = popen(("shasum -a 256 '" + fs::weakly_canonical(path).string() + "'").c_str(), "r")) {
            char line[256] = {};
            if (std::fgets(line, sizeof(line), pipe)) digest.assign(line, 64);
            pclose(pipe);
        }
        if (digest.size() != 64) return 10;
        for (const bool current : {true, false}) {
            const auto seed = root / (current ? "seed-current" : "seed-stale");
            fs::create_directories(seed / "metal/dev.petari.Petari");
            std::ofstream(seed / "metal/dev.petari.Petari/canonical-marker") << "x";
            std::ofstream(seed / "seed.json") << "{\"exe_sha256\": \"" << (current ? digest : std::string(64, '0')) << "\"}";
            const pid_t seeded = fork();
            if (seeded == 0) {
                setenv("PETARI_SHADER_SEED_DIR", seed.c_str(), 1);
                PetariNative::App::SmokeStorage storage;
                if (!storage.prepare(root / (current ? "seeded-current" : "seeded-stale"), nullptr, &error)) _exit(11);
                const bool found = fs::exists(storage.cache / "metal/dev.petari.Petari/canonical-marker");
                _exit(found == current ? 0 : 12);
            }
            if (seeded < 0 || waitpid(seeded, &status, 0) != seeded || !WIFEXITED(status) || WEXITSTATUS(status)) return 13;
        }
    }
    std::puts("Private caches, two processes, same-user exclusion and read-only seed and build-keyed canonical shader seed passed.");
    fs::remove_all(root);
    return 0;
}

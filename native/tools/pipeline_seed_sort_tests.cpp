// Reproduce the seed reader's read-only-VFS temporary-sort failure without a GPU.
#include <sqlite3.h>
#include <cstdio>
#include <stdexcept>
static sqlite3_vfs* base;
static unsigned rejected;
static int openReadOnly(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* actual) {
    if ((flags & SQLITE_OPEN_READWRITE) || !name) { ++rejected; return SQLITE_CANTOPEN; }
    return base->xOpen(base, name, file, flags, actual);
}
static int query(const char* path, bool memory, unsigned* rows) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, "petari-sort-test") != SQLITE_OK) throw std::runtime_error("open failed");
    sqlite3_exec(db, memory ? "PRAGMA temp_store=MEMORY" : "PRAGMA temp_store=FILE", nullptr, nullptr, nullptr);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT type,config_version,config FROM pipeline_cache ORDER BY first_frame_used", -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("prepare failed");
    int code;
    *rows = 0;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) ++*rows;
    sqlite3_finalize(statement); sqlite3_close(db);
    return code;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    sqlite3_initialize(); base = sqlite3_vfs_find(nullptr);
    auto vfs = *base; vfs.zName = "petari-sort-test"; vfs.xOpen = openReadOnly;
    sqlite3_vfs_register(&vfs, 0);
    unsigned originalRows, memoryRows, indexedRows;
    const auto original = query(argv[1], false, &originalRows);
    const auto memory = query(argv[1], true, &memoryRows);
    const auto indexed = query(argv[2], false, &indexedRows);
    std::printf("unindexed_file_sort=%d rows=%u rejected_temp_opens=%u memory_sort=%d rows=%u indexed_file_sort=%d rows=%u\n",
                original, originalRows, rejected, memory, memoryRows, indexed, indexedRows);
    sqlite3_vfs_unregister(&vfs);
    return original != SQLITE_CANTOPEN || !rejected || memory != SQLITE_DONE || indexed != SQLITE_DONE || memoryRows != indexedRows;
}

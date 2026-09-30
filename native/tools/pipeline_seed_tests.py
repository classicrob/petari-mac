#!/usr/bin/env python3
"""CPU regressions for seed provenance and generated draw-priority scheduling."""
import argparse
import importlib.util
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from export_pipeline_stage_seeds import export

ROOT = Path(__file__).resolve().parents[2]
AURORA = ROOT.parent / 'aurora-reference'


class PipelineSeedTests(unittest.TestCase):
    def test_global_import_does_not_invent_draws(self):
        spec = importlib.util.spec_from_file_location('patch_pipeline', ROOT / 'native/gx/patch_aurora_pipeline.py')
        patcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patcher)
        patched = patcher.patch((AURORA / 'lib/gfx/pipeline_cache.cpp').read_text())
        start = patched.index('static void seed_pipeline_cache_path(')
        end = patched.index('\nstatic ', start + 1)
        function = patched[start:end]
        self.assertIn('seed_pipeline_cache_path(path.string(), true);', patched)
        harness = r'''
#include <sqlite3.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <string>
#include <vector>
#include <cstdio>
using PipelineRef = uint64_t;
enum class ShaderType : uint8_t { Clear, GX };
struct ByteBuffer : std::vector<uint8_t> { using std::vector<uint8_t>::vector; };
struct PipelineCacheWrite { ShaderType type; PipelineRef hash; uint32_t configVersion; ByteBuffer config; uint32_t firstFrameUsed; };
struct Logger {
 template<class... T> void warn(const char*, T&&...) {}
 template<class... T> void error(const char*, T&&...) {}
 template<class... T> void info(const char*, T&&...) {}
} Log;
bool g_pipelineCacheBroken = false;
sqlite3* g_pipelineCacheDb = reinterpret_cast<sqlite3*>(1);
sqlite3_stmt* g_pipelineCacheUpsertStmt = reinterpret_cast<sqlite3_stmt*>(1);
namespace sqlite { struct Transaction { Transaction(sqlite3*, Logger&, bool) {} operator bool() { return true; } void commit() {} }; }
void pipeline_cache_abort() { g_pipelineCacheBroken = true; }
std::vector<uint32_t> firstUses;
bool write_pipeline_cache_record(const PipelineCacheWrite& write) { firstUses.push_back(write.firstFrameUsed); return true; }
sqlite3* open_pipeline_cache_seed_db(const std::string&) {
 sqlite3* db = nullptr; sqlite3_open(":memory:", &db);
 sqlite3_exec(db, "CREATE TABLE pipeline_cache(type,hash,config_version,config_size,config,first_frame_used);"
                 "INSERT INTO pipeline_cache VALUES(1,42,13,1,x'00',7);", nullptr, nullptr, nullptr);
 return db;
}
''' + function + r'''
int main() {
 seed_pipeline_cache_path("fixture", true);
 if (g_pipelineCacheBroken || firstUses != std::vector<uint32_t>{UINT32_MAX}) return 1;
 firstUses.clear(); seed_pipeline_cache_path("fixture");
 if (g_pipelineCacheBroken || firstUses != std::vector<uint32_t>{7}) return 2;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'test.cpp'
            executable = Path(temporary) / 'test'
            source.write_text(harness)
            subprocess.run(['c++', '-std=c++20', str(source), '-lsqlite3', '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_shared_and_attributed_exports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'source.db'
            with sqlite3.connect(source) as db:
                db.executescript('CREATE TABLE aurora_schema(value); INSERT INTO aurora_schema VALUES(1); '
                                 'CREATE TABLE pipeline_cache(type,hash,config_version,config_size,config,first_frame_used);')
                db.executemany('INSERT INTO pipeline_cache VALUES(?,?,?,?,?,?)',
                               [(0, 5, 1, 1, b'a', 0), (1, -1, 13, 1, b'b', 2), (1, 7, 13, 1, b'c', 3)])
            original = source.read_bytes()
            self.assertEqual(export(source, root / 'all', ['AstroGalaxy']), {'AstroGalaxy': 3})
            log = root / 'run.log'
            log.write_text('[gx stage config] stage=AstroGalaxy config=ffffffffffffffff phase=after_prep\n')
            self.assertEqual(export(source, root / 'filtered', ['AstroGalaxy'], [log]), {'AstroGalaxy': 2})
            with sqlite3.connect(root / 'filtered/AstroGalaxy.db') as db:
                self.assertEqual(db.execute('SELECT hash FROM pipeline_cache WHERE type=1').fetchall(), [(-1,)])
            self.assertEqual(source.read_bytes(), original)
            with self.assertRaises(ValueError):
                export(source, root / 'bad', ['EggStarGalaxy'], [log])
            with self.assertRaises(ValueError):
                export(source, root, ['../source'])

    def test_production_promotion(self):
        spec = importlib.util.spec_from_file_location('patch_pipeline', ROOT / 'native/gx/patch_aurora_pipeline.py')
        patcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(patcher)
        patched = patcher.patch((AURORA / 'lib/gfx/pipeline_cache.cpp').read_text())
        start = patched.index('static void promote_pending_pipeline(')
        end = patched.index('static std::optional<PendingPipeline> take_pending_pipeline', start)
        function = patched[start:end]
        harness = r'''
#include <deque>
#include <algorithm>
#include <cassert>
using PipelineRef = unsigned;
enum class PipelinePriority { Background, Normal, Blocking };
struct Pending { unsigned hash; };
std::deque<Pending> g_pipelineQueue, g_backgroundPipelineQueue;
auto find_pending_pipeline(std::deque<Pending>& queue, unsigned hash) {
    return std::find_if(queue.begin(), queue.end(), [=](auto p) { return p.hash == hash; });
}
''' + function + r'''
int main() {
    g_pipelineQueue = {{1}, {2}, {3}}; g_backgroundPipelineQueue = {{4}, {5}};
    promote_pending_pipeline(3, PipelinePriority::Blocking);
    assert(g_pipelineQueue.front().hash == 3 && g_pipelineQueue.size() == 3);
    promote_pending_pipeline(4, PipelinePriority::Blocking);
    assert(g_pipelineQueue.front().hash == 4 && g_backgroundPipelineQueue.size() == 1);
    promote_pending_pipeline(5, PipelinePriority::Normal);
    assert(g_pipelineQueue.back().hash == 5 && g_backgroundPipelineQueue.empty());
    promote_pending_pipeline(2, PipelinePriority::Background);
    assert(g_pipelineQueue.front().hash == 4);
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'test.cpp'
            executable = Path(temporary) / 'test'
            source.write_text(harness)
            subprocess.run(['c++', '-std=c++20', str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--aurora', type=Path, default=AURORA)
    args, remaining = parser.parse_known_args()
    AURORA = args.aurora
    unittest.main(argv=[sys.argv[0], *remaining])

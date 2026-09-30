#!/usr/bin/env python3
"""Patch Aurora's pipeline scheduling, essential warmup and opt-in async GX policy."""
from pathlib import Path
import sys


def patch(text):
    here = Path(__file__).resolve().parent

    def replace(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError(f'Aurora pipeline patch anchor mismatch: {old}')
        text = text.replace(old, new)

    text = f'#include "{here / "pipeline_profile.hpp"}"\n#include <vector>\n' + text
    replace('\n  bool schemaMatch = false;', '''
  // This immutable SDL VFS cannot create SQLite sort spill files. Larger stage
  // manifests may need a temporary sort when supplied without a load-order index.
  if (sqlite3_exec(seedDb, "PRAGMA temp_store=MEMORY", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_close(seedDb);
    return nullptr;
  }
  bool schemaMatch = false;''')
    replace('static void seed_pipeline_cache() {',
            'static void seed_pipeline_cache_path(const std::string& seedPath, bool speculative = false) {')
    replace('          .firstFrameUsed = static_cast<uint32_t>(firstFrameUsedValue),',
            '          .firstFrameUsed = speculative ? UINT32_MAX : static_cast<uint32_t>(firstFrameUsedValue),')
    replace('  const auto seedPath = pipeline_cache_seed_path();\n', '')
    replace('  seed_pipeline_cache();', '  seed_pipeline_cache_path(pipeline_cache_seed_path());\n  petari_seed_global();')
    replace('static void start_pipeline_cache_writer() {',
            (here / 'pipeline_global.inc').read_text() + '\nstatic void start_pipeline_cache_writer() {')
    replace('    const auto firstFrameUsed = static_cast<uint32_t>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 1));',
            '    const auto firstFrameUsed = static_cast<uint32_t>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 1));\n'
            '    if (!PetariPipeline::globalPrecompile() && firstFrameUsed == UINT32_MAX) continue;')
    replace('  const size_t loadedCount = load_pipeline_cache();\n  rebuild_pipeline_cache();', '''  petariGlobalStarted = PetariPipeline::Clock::now();
  const size_t loadedCount = load_pipeline_cache();
  rebuild_pipeline_cache();
  if (PetariPipeline::globalPrecompile()) {
    {
      std::lock_guard lock{g_pipelineMutex};
      petariGlobalInitialPending = g_pendingPipelines.size();
      petariGlobalActive = true;
      petariGlobalLastLog = {};
      std::fprintf(stderr, "[gx global prep] configs_known=%zu pending_variants=%zu workers=%u performance_cores=%u background_qos=utility\\n",
                   g_knownPipelines.size(), petariGlobalInitialPending, PetariPipeline::startupWorkerCount(), PetariPipeline::performanceCores());
    }
    petari_global_progress();
  }''')
    replace('static std::thread g_pipelineThread;', 'static std::vector<std::thread> g_pipelineThreads;')
    anchor = '''enum class PipelinePriority {
  Background, // cache warmup
  Normal,     // async skip draw
  Blocking,   // block until compiled
};'''
    replace(anchor, anchor + '\n\n' + (here / 'pipeline_runtime.inc').read_text())
    replace('  const bool blocking = priority == PipelinePriority::Blocking;',
            '  const bool blocking = priority == PipelinePriority::Blocking;\n'
            '  PetariPipelineWait petariPipelineWait(blocking, runtimeKey);')
    replace('      g_pendingPipelines.insert(runtimeKey);',
            '      petariPipelineSamples[runtimeKey].queued = PetariPipeline::Clock::now();\n'
            '      g_pendingPipelines.insert(runtimeKey);')
    anchor = '  const auto runtimeKey = xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(type)));'
    replace(anchor, anchor + '\n  petari_note_variant(runtimeKey, xxh3_hash(config, static_cast<HashType>(type)), type, priority);', 2)
    anchor = '''  return find_pipeline_impl(
      xxh3_hash(config, static_cast<HashType>(type)),
      [config] { return CompiledPipeline{.main = rmlui::create_pipeline(config)}; }, priority);'''
    replace(anchor, '''  const auto runtimeKey = xxh3_hash(config, static_cast<HashType>(type));
  petari_note_variant(runtimeKey, runtimeKey, type, priority);
  return find_pipeline_impl(runtimeKey,
      [config] { return CompiledPipeline{.main = rmlui::create_pipeline(config)}; }, priority);''')
    replace('  return resolve_pipeline(ShaderType::GX, config, layout, PipelinePriority::Normal);',
            '  const PetariPipeline::BoundedDrawScope petariBoundedDraw{!PetariPipeline::unboundedBlocking()};\n'
            '  return resolve_pipeline(ShaderType::GX, config, layout, PetariPipeline::asynchronous()\n'
            '      ? PipelinePriority::Normal : PipelinePriority::Blocking);')
    # Bounded GX draws wait only within the frame budget; clear/UI pipelines still block.
    replace('''    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });''',
            '''    const auto petariReady = [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; };
    if (PetariPipeline::boundedDraw) petari_bounded_draw_wait(lock, petariReady);
    else g_pipelineReadyCv.wait(lock, petariReady);''')
    replace('return resolve_pipeline(ShaderType::Clear, config, layout, PipelinePriority::Normal);',
            'return resolve_pipeline(ShaderType::Clear, config, layout, PipelinePriority::Blocking);')

    # Real draw requests must also jump ahead of speculative stage jobs.
    replace('static void promote_pending_pipeline(PipelineRef hash, PipelinePriority priority) {\n  if (priority == PipelinePriority::Background) {\n    return;\n  }\n', 'static void promote_pending_pipeline(PipelineRef hash, PipelinePriority priority) {\n  if (priority == PipelinePriority::Background) {\n    return;\n  }\n  if (priority == PipelinePriority::Blocking) {\n    auto requestedIt = find_pending_pipeline(g_pipelineQueue, hash);\n    if (requestedIt != g_pipelineQueue.end()) {\n      auto pending = std::move(*requestedIt);\n      g_pipelineQueue.erase(requestedIt);\n      g_pipelineQueue.emplace_front(std::move(pending));\n      return;\n    }\n  }\n')

    start = text.index('static void pipeline_worker() {')
    end = text.index('\ntemplate <typename PipelineConfig>\nstatic size_t load_pipeline_cache_entries', start)
    text = text[:start] + (here / 'pipeline_worker.inc').read_text() + text[end:]
    replace('    g_pipelineThread = std::thread(pipeline_worker);', '''    const unsigned count = PetariPipeline::startupWorkerCount();
    petariActiveWorkers = count;
    std::fprintf(stderr, "[gx pipeline] policy=%s compile_workers=%u\\n",
                 PetariPipeline::asynchronous() ? "async" : "blocking", count);
    for (unsigned i = 0; i < count; ++i) g_pipelineThreads.emplace_back(pipeline_worker, i);''')
    replace('    g_pipelineThread.join();',
            '    for (auto& worker : g_pipelineThreads) worker.join();\n    g_pipelineThreads.clear();')
    replace('  stop_pipeline_cache_writer();',
            '  petari_pipeline_summary();\n  petariPipelineSamples.clear();\n'
            '  petariSkippedDraws.store(0, std::memory_order_relaxed);\n  stop_pipeline_cache_writer();')
    replace('  petariSkippedDraws.store(0, std::memory_order_relaxed);',
            '  petariSkippedDraws.store(0, std::memory_order_relaxed);\n  petariFailedPipelines = 0;\n'
            '  petariDeferredDraws = 0;')

    replace('  g_pipelineLayoutKey = scene.key;', '''  g_pipelineLayoutKey = scene.key;
  // Clear masks form a finite family. Queue all of them before route-derived
  // configurations so startup preparation includes even never-observed masks.
  for (unsigned mask = 0; mask != 8; ++mask) {
    clear::PipelineConfig config{};
    config.clearColor = (mask & 1) != 0;
    config.clearAlpha = (mask & 2) != 0;
    config.clearDepth = (mask & 4) != 0;
    resolve_pipeline(ShaderType::Clear, config, scene, PipelinePriority::Background);
    if (offscreen.key != scene.key)
      resolve_pipeline(ShaderType::Clear, config, offscreen, PipelinePriority::Background);
  }''')

    # GX render() checks readiness once per draw. Count there through get_pipeline,
    # with the same lock as compilation publication; no hot-path formatting.
    replace('''  if (it == g_pipelines.end() || !it->second.main) {
    return false;
  }''', '''  if (it == g_pipelines.end() || !it->second.main) {
    const auto sampleIt = petariPipelineSamples.find(ref);
    if (sampleIt != petariPipelineSamples.end() && sampleIt->second.type == ShaderType::GX) {
      auto& sample = sampleIt->second;
      const auto now = PetariPipeline::Clock::now();
      if (sample.skipped++ == 0) sample.firstSkip = now;
      sample.lastSkip = now;
      petariSkippedDraws.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }''')
    replace('  remember_pipeline_config(ShaderType::GX, config, current_frame(), true);',
            '  petari_note_stage_config(xxh3_hash(config, static_cast<HashType>(ShaderType::GX)),\n'
            '      xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(ShaderType::GX))));\n'
            '  remember_pipeline_config(ShaderType::GX, config, current_frame(), true);')
    replace('void begin_pipeline_frame() {',
            'void begin_pipeline_frame() {\n  petari_stage_flush_logs();\n  petari_pipeline_frame_budget();')
    replace('  petari_pipeline_summary();', '  petari_stage_flush_logs();\n  petari_stage_reset();\n  petari_pipeline_summary();')
    text += '\n' + (here / 'pipeline_stage.inc').read_text()
    text += '''
extern "C" bool petari_gx_waiting_for_pipeline() {
  return aurora::gfx::petariPipelineWaiters.load(std::memory_order_relaxed) != 0;
}
extern "C" uint64_t petari_gx_pipeline_skipped_draws() {
  return aurora::gfx::petariSkippedDraws.load(std::memory_order_relaxed);
}
'''
    return text


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: patch_aurora_pipeline.py SOURCE OUTPUT')
    source, output = map(Path, sys.argv[1:])
    try:
        text = patch(source.read_text())
    except ValueError as error:
        raise SystemExit(str(error)) from error
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Do not drop first-use geometry while compiling its native graphics pipeline."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
for kind in ('GX', 'Clear'):
    old = f'return resolve_pipeline(ShaderType::{kind}, config, layout, PipelinePriority::Normal);'
    if text.count(old) != 1:
        raise SystemExit(f'Aurora {kind} pipeline priority patch anchor mismatch')
    text = text.replace(old, f'return resolve_pipeline(ShaderType::{kind}, config, layout, PipelinePriority::Blocking);')
guard_anchor = "  const bool blocking = priority == PipelinePriority::Blocking;"
if text.count(guard_anchor) != 1:
    raise SystemExit('Aurora blocking-pipeline guard anchor mismatch')
text = text.replace(guard_anchor, guard_anchor + '\n  PetariPipelineWait petariPipelineWait(blocking);')
text = '''#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
namespace {
std::atomic<unsigned> petariPipelineWaiters{0};
struct PetariPipelineWait {
    bool active;
    bool trace;
    std::chrono::steady_clock::time_point start;
    explicit PetariPipelineWait(bool value) : active(value) {
        static const bool enabled = [] {
            const char* value = std::getenv("PETARI_TRACE_BOOT");
            return value && *value && *value != '0';
        }();
        trace = active && enabled;
        if (trace) start = std::chrono::steady_clock::now();
        if (active) petariPipelineWaiters.fetch_add(1, std::memory_order_relaxed);
    }
    ~PetariPipelineWait() {
        if (active) petariPipelineWaiters.fetch_sub(1, std::memory_order_relaxed);
        if (trace) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (ms >= 10.0) std::fprintf(stderr, "[gx pipeline] blocking resolve %.2f ms\\n", ms);
        }
    }
};
}
extern "C" bool petari_gx_waiting_for_pipeline() {
    return petariPipelineWaiters.load(std::memory_order_relaxed) != 0;
}
''' + text
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

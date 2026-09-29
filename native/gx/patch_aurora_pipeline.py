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
namespace {
std::atomic<unsigned> petariPipelineWaiters{0};
struct PetariPipelineWait {
    bool active;
    explicit PetariPipelineWait(bool value) : active(value) {
        if (active) petariPipelineWaiters.fetch_add(1, std::memory_order_relaxed);
    }
    ~PetariPipelineWait() {
        if (active) petariPipelineWaiters.fetch_sub(1, std::memory_order_relaxed);
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

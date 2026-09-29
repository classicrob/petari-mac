#!/usr/bin/env python3
"""Isolate renderer-owned allocations from the currently selected game heap."""
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:])
text = source.read_text()
anchors = {
    'aurora.cpp': [
        'AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {',
        'void aurora_shutdown() {', 'const AuroraEvent* aurora_update() {',
        'bool aurora_begin_frame() {', 'void aurora_end_frame() {',
    ],
    'GXManage.cpp': ['GXFifoObj* GXInit(void* base, u32 size) {', 'void GXDrawDone() {'],
    'render_worker.cpp': ['void worker_main(std::stop_token token) {'],
    'texture_replacement.cpp': ['void worker_main(std::stop_token token) {'],
    'fifo.cpp': ['void process_to(uint64_t target, std::memory_order order) noexcept {'],
}
for anchor in anchors[source.name]:
    if text.count(anchor) != 1:
        raise SystemExit(f'Aurora allocation patch anchor mismatch: {anchor}')
    text = text.replace(anchor, anchor + '\n  PetariNative::HostAllocationScope petariHostAllocations;')
text = '#include <petari/host_allocation.hpp>\n' + text
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

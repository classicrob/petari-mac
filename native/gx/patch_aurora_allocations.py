#!/usr/bin/env python3
"""Isolate renderer-owned allocations from the currently selected game heap.

Also drains Objective-C autorelease pools on the renderer's long-lived threads. Dawn's Metal
backend autoreleases objects (presentDrawable: dictionaries, command-buffer bookkeeping), and a
thread with no pool of its own keeps them until it exits. The soak measured the render worker
growing by about 0.35 MB per stage cycle this way. The render worker pops a pool per work item;
the FIFO processor pops one per process_to; the main thread pops one around each frame entry point.
"""
import sys
from pathlib import Path

source, output = map(Path, sys.argv[1:])
text = source.read_text()
if 'petari/host_allocation.hpp' in text:
    raise SystemExit(f'Aurora allocation patch: {source} is already patched')
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
# Scopes that pop an autorelease pool on exit. The texture workers only decode files on the CPU.
pools = {
    'aurora.cpp': ['const AuroraEvent* aurora_update() {', 'bool aurora_begin_frame() {', 'void aurora_end_frame() {'],
    'fifo.cpp': ['void process_to(uint64_t target, std::memory_order order) noexcept {'],
    'render_worker.cpp': ['      ZoneScopedN("QueueItem work");\n'],
}
POOL = '  PetariAutoreleasePool petariAutoreleasePool;'


def replace_once(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f'Aurora allocation patch anchor mismatch: {old}')
    text = text.replace(old, new)


for anchor in anchors[source.name]:
    replace_once(anchor, anchor + '\n  PetariNative::HostAllocationScope petariHostAllocations;')
for anchor in pools.get(source.name, []):
    # A line anchor gets the pool on the next line at its own indentation; a function anchor opens its body.
    indent = anchor[:len(anchor) - len(anchor.lstrip())]
    replace_once(anchor, anchor + (indent + POOL.strip() + '\n' if anchor.endswith('\n') else '\n' + POOL))

prelude = '#include <petari/host_allocation.hpp>\n'
if source.name in pools:
    prelude += '''#ifdef __APPLE__
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);
namespace {
struct PetariAutoreleasePool {
  void* pool = objc_autoreleasePoolPush();
  PetariAutoreleasePool() = default;
  PetariAutoreleasePool(const PetariAutoreleasePool&) = delete;
  PetariAutoreleasePool& operator=(const PetariAutoreleasePool&) = delete;
  ~PetariAutoreleasePool() { objc_autoreleasePoolPop(pool); }
};
} // namespace
#else
namespace {
struct PetariAutoreleasePool {};
} // namespace
#endif
'''
text = prelude + text
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

#!/usr/bin/env python3
"""Route Aurora's FIFO processing through Petari's GX synchronisation.

usage: patch_aurora_sync.py SOURCE OUTPUT

SOURCE is one of Aurora's lib/gx/fifo.cpp, lib/gx/command_processor.cpp,
lib/dolphin/gx/GXManage.cpp, lib/dolphin/gx/GXFifo.cpp or
lib/dolphin/gx/GXCpu2Efb.cpp. fifo.cpp and GXManage.cpp may be the output of
patch_aurora_allocations.py (run that first); the others are the originals.
Patched files include "sync_backend.h", so the target needs native/gx on its
include path.

- fifo.cpp: the processor stops at the FIFO breakpoint, stops each process()
  call at synchronisation BPs, reports them in stream order outside
  sBufferMutex, applies GXAbortFrame's discard, and fifo::drain waits through
  the platform (yielding the OS CPU baton). process() runs on a processor-owned
  copy of the published stream, so sBufferMutex is never held across a batch
  (first-use pipeline compiles block inside one for seconds); game threads
  that still find the lock busy wait with the OS CPU released. Exports the
  petari_aurora_fifo_* functions.
- command_processor.cpp: process() also returns after PE token BPs (0x47,
  0x48) and records which synchronisation BP ended the call; index-buffer
  reservations for triangle fans and strips of fewer than 3 vertices no
  longer wrap to about 8 GiB.
- GXManage.cpp: GXDrawDone, GXSetDrawDone, GXSetDrawDoneCallback removed
  (sync_bridge.cpp defines them).
- GXFifo.cpp: GXGetGPStatus, GXGetFifoPtrs removed; GXGetCPUFifo/GXGetGPFifo
  become petari_aurora_cpu_fifo/petari_aurora_gp_fifo (sync_bridge.cpp
  defines the SDK's GXBool GXGetCPUFifo(GXFifoObj*)).
- GXCpu2Efb.cpp: GXPeekZ removed (the renderer's token-time snapshot
  readback defines it).
"""
import re
import sys
from pathlib import Path

ALLOCATION_SCOPE = '  PetariNative::HostAllocationScope petariHostAllocations;\n'


def fail(message):
    raise SystemExit(f'Aurora sync patch: {message}')


def function_span(text, signature):
    """Start and end (after the closing brace line) of a definition."""
    if text.count(signature) != 1:
        fail(f'anchor mismatch: {signature}')
    start = text.index(signature)
    depth = 0
    for i in range(text.index('{', start), len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                end = i + 1
                if text.startswith('\n', end):
                    end += 1
                return start, end
    fail(f'unterminated definition: {signature}')


def remove_function(text, signature):
    start, end = function_span(text, signature)
    return text[:start] + text[end:]


def replace_function(text, signature, body):
    """Replaces a definition, keeping an allocation scope the allocation patch
    put at the top of it."""
    start, end = function_span(text, signature)
    head = signature + '\n'
    if text[start + len(signature):].startswith('\n' + ALLOCATION_SCOPE):
        head += ALLOCATION_SCOPE
    return text[:start] + head + body + text[end:]


def replace_once(text, old, new):
    if text.count(old) != 1:
        fail(f'anchor mismatch: {old.strip().splitlines()[0]}')
    return text.replace(old, new)


MIRROR_AND_LOCK = '''
// Petari: the processor executes a private copy of the published stream.
// sBufferMutex is held only while a range is copied out, and while game
// threads grow or rebase the buffer; never while process() runs, which can
// block for seconds in first-use pipeline compiles. A game thread waiting for
// the lock holds the OS CPU, so a lock held across a batch stopped every game
// thread (audio included) for the whole compile burst. Published bytes never
// change (patch_u32 writes only above the published watermark), and process()
// keeps no pointer into its input once it returns (vertex and index data are
// copied into the frame's buffers).
// Processor thread only (and fifo::init with the processor stopped). Never
// destroyed: the processor may still run during static destruction.
std::vector<uint8_t>& sPetariMirror = *new std::vector<uint8_t>;
uint64_t sPetariMirrorBase = 0; // stream position of sPetariMirror[0]

// Petari: game threads take sBufferMutex through this. If it stays busy past a
// short spin (the processor copying a large range out), the OS CPU is released
// for the wait and `work` (sync_backend.h), so other game threads keep running.
template <typename Work>
void petari_with_buffer_lock(Work&& work) {
  std::unique_lock lock{sBufferMutex, std::defer_lock};
  const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
  while (!lock.try_lock()) {
    if (std::chrono::steady_clock::now() >= spinUntil) {
      const bool released = petari_gx_sync_begin_host_wait() != 0;
      lock.lock();
      work();
      lock.unlock();
      if (released) {
        petari_gx_sync_end_host_wait();
      }
      return;
    }
    std::this_thread::yield();
  }
  work();
}
'''


def patch_fifo(text):
    text = replace_once(text, '#include <mutex>\n', '#include <chrono>\n#include <mutex>\n#include <thread>\n#include <vector>\n\n#include "sync_backend.h"\n')
    text = replace_once(text, 'namespace {\nconstexpr Module Log{"aurora::gx::fifo"};',
                        '// Petari: which synchronisation BP ended the last process() call\n'
                        '// (command_processor.cpp), under sBufferMutex.\n'
                        'extern uint32_t petariSyncBp;\n'
                        '// Petari: asks process() to stop at the next command boundary.\n'
                        'extern std::atomic<bool> petariAbortRequested;\n\n'
                        'namespace {\nconstexpr Module Log{"aurora::gx::fifo"};')
    text = replace_once(text, 'std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};\n',
                        'std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};\n'
                        '// Petari: GXAbortFrame discards the stream up to here without processing it.\n'
                        'std::atomic<uint64_t> sPetariAbortTo{0};\n\n'
                        'void petari_store_max(std::atomic<uint64_t>& value, uint64_t candidate) noexcept {\n'
                        '  uint64_t current = value.load(std::memory_order_relaxed);\n'
                        '  while (current < candidate &&\n'
                        '         !value.compare_exchange_weak(current, candidate, std::memory_order_acq_rel)) {\n'
                        '  }\n'
                        '}\n' + MIRROR_AND_LOCK)
    text = replace_function(text, 'void write_data_grow(const void* data, uint32_t length) {', '''\
  // Petari: sized and copied under the lock, which a game thread may have
  // waited for with the OS CPU released (petari_with_buffer_lock).
  const auto grow = [data, length] {
    const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
    AURORA_ASSERT(needed64 <= std::numeric_limits<uint32_t>::max(), "fifo::write_data: buffer size overflow");
    const auto needed = static_cast<uint32_t>(needed64);
    if (needed > detail::sBufferCapacity) {
      const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
      const auto newCapacity = static_cast<uint32_t>(
          std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
      auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
      AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
      detail::sBufferData = resized;
      detail::sBufferCapacity = newCapacity;
    }
    std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
    detail::sBufferSize = needed;
  };
  if (sWorkerThread.joinable()) {
    petari_with_buffer_lock(grow);
  } else {
    grow();
  }
}
''')
    text = replace_once(text, '''\
  {
    std::lock_guard lock{sBufferMutex};
    sStreamBase = target;
    detail::sBufferSize = 0;
  }
  sPendingDraws = 0;
''', '''\
  petari_with_buffer_lock([target] {
    sStreamBase = target;
    detail::sBufferSize = 0;
  });
  sPendingDraws = 0;
''')
    text = replace_once(text, '''\
  sWorkerWake.store(0, std::memory_order_relaxed);

  start_worker();
''', '''\
  sWorkerWake.store(0, std::memory_order_relaxed);
  sPetariMirror.clear(); // positions restart: nothing mirrored is valid
  sPetariMirrorBase = 0;

  start_worker();
''')
    # Draw done is a GP interrupt delivered by the platform, not a call here.
    text = remove_function(text, 'void dispatch_draw_done() noexcept {')
    text = replace_function(text, 'void process_to(uint64_t target, std::memory_order order) noexcept {', '''\
  static_assert(kProcessingMode == ProcessingMode::Thread, "Petari GX sync expects the FIFO processor thread");
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (processed < target) {
    // Petari: GXAbortFrame discarded everything written before it.
    if (const uint64_t abortTo = sPetariAbortTo.load(std::memory_order_acquire); abortTo > processed) {
      petariAbortRequested.store(false, std::memory_order_relaxed);
      processed = abortTo;
      sProcessed.store(processed, order);
      sProcessed.notify_all();
      petari_gx_sync_abort_applied(processed, sPublished.load(std::memory_order_acquire));
      continue;
    }
    // Petari: never process at or past the FIFO breakpoint.
    const uint64_t limit = petari_gx_sync_process_limit(processed, target);
    if (limit == processed) {
      return; // halted; worker_main waits for wake_worker
    }
    // Petari: copy what is not mirrored yet of [processed, limit), then
    // process outside sBufferMutex. The mirror restarts once consumed.
    if (processed < sPetariMirrorBase || processed >= sPetariMirrorBase + sPetariMirror.size()) {
      sPetariMirror.clear();
      sPetariMirrorBase = processed;
    }
    if (const uint64_t mirrored = sPetariMirrorBase + sPetariMirror.size(); limit > mirrored) {
      std::lock_guard lock{sBufferMutex};
      AURORA_ASSERT(mirrored >= sStreamBase && limit <= sStreamBase + detail::sBufferSize,
                    "FIFO processing range [{}, {}) is outside buffered range [{}, {})", mirrored, limit, sStreamBase,
                    sStreamBase + detail::sBufferSize);
      const uint8_t* from = detail::sBufferData + static_cast<uint32_t>(mirrored - sStreamBase);
      sPetariMirror.insert(sPetariMirror.end(), from, from + (limit - mirrored));
    }
    petariSyncBp = 0;
    const ProcessResult result = process(sPetariMirror.data() + (processed - sPetariMirrorBase),
                                         static_cast<uint32_t>(limit - processed));
    const uint32_t syncBp = petariSyncBp;
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= limit - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  limit - processed);
    processed += result.bytesProcessed;
    sProcessed.store(processed, order);
    sProcessed.notify_all();
    // Petari: command boundary, outside sBufferMutex. Reports draw done and
    // tokens in stream order; a token interrupt runs the snapshot hook here.
    petari_gx_sync_processed(processed, sPublished.load(std::memory_order_acquire), syncBp);
  }
}
''')
    text = replace_function(text, 'void worker_main(std::stop_token token) noexcept {', '''\
  std::stop_callback wakeOnStop{token, wake_worker};
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    if (published != processed) {
      process_to(published, std::memory_order_release);
      if (sProcessed.load(std::memory_order_relaxed) != processed) {
        continue;
      }
      // Petari: halted at the FIFO breakpoint until it moves or is disabled.
    }

    if (token.stop_requested()) {
      break;
    }
    sWorkerWake.wait(event, std::memory_order_acquire);
  }
}
''')
    text = replace_once(text, '''\
    uint64_t processed = sProcessed.load(std::memory_order_acquire);
    if (processed < target) {
      do {
        sProcessed.wait(processed, std::memory_order_acquire);
        processed = sProcessed.load(std::memory_order_acquire);
      } while (processed < target);
    }
    break;''', '''\
    // Petari: game threads sleep and yield the OS CPU baton; a breakpoint
    // may hold the processor until another game thread moves it.
    petari_gx_sync_wait_processed(target);
    break;''')
    # sPublished only grows; GXAbortFrame may raise it from interrupt context.
    text = replace_once(text, '''\
    sPublished.store(target, std::memory_order_release);
    wake_worker();

''', '''\
    petari_store_max(sPublished, target);
    wake_worker();

''')
    text = replace_once(text, '''\
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_release);
''', '''\
    sPendingDraws = 0;
    petari_store_max(sPublished, target);
''')
    text += '''
// Petari: C ABI for sync_bridge.cpp (sync_backend.h).
extern "C" {
uint64_t petari_aurora_fifo_write_position(void) {
  return aurora::gx::fifo::sStreamBase + aurora::gx::fifo::detail::sBufferSize;
}

uint64_t petari_aurora_fifo_processed_position(void) {
  return aurora::gx::fifo::sProcessed.load(std::memory_order_acquire);
}

void petari_aurora_fifo_publish(void) { aurora::gx::fifo::publish(); }

void petari_aurora_fifo_publish_all(void) {
  using namespace aurora::gx::fifo;
  sPendingDraws = 0;
  petari_store_max(sPublished, sStreamBase + detail::sBufferSize);
  wake_worker();
}

// Game thread, or interrupt context while the writer is blocked (as
// handleGXAbortAlarm runs during GXDrawDone).
void petari_aurora_fifo_abort(void) {
  using namespace aurora::gx::fifo;
  const uint64_t written = sStreamBase + detail::sBufferSize;
  sPendingDraws = 0;
  petari_store_max(sPublished, written); // before the abort target: the processor reads them in reverse
  petari_store_max(sPetariAbortTo, written);
  petariAbortRequested.store(true, std::memory_order_release);
  wake_worker();
}

void petari_aurora_fifo_wake(void) { aurora::gx::fifo::wake_worker(); }
}
'''
    return text


def patch_command_processor(text):
    text = replace_once(text, '#include "texture.hpp"\n', '#include "texture.hpp"\n\n#include "sync_backend.h"\n\n#include <atomic>\n')
    text = replace_once(text, 'ProcessResult process(const u8* data, u32 size) noexcept {',
                        '// Petari: raw value of the synchronisation BP (draw done 0x45, PE token\n'
                        '// 0x47, PE token interrupt 0x48) that ended the last process() call, or 0.\n'
                        '// Written and read under fifo.cpp\'s sBufferMutex.\n'
                        'uint32_t petariSyncBp = 0;\n'
                        '// Petari: set by GXAbortFrame (fifo.cpp); process() stops at the next\n'
                        '// command boundary so nothing more of the discarded stream runs.\n'
                        'std::atomic<bool> petariAbortRequested{false};\n\n'
                        'ProcessResult process(const u8* data, u32 size) noexcept {')
    text = replace_once(text, '''\
  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();''', '''\
  while (!reader.empty()) {
    if (reader.offset() != 0 && petariAbortRequested.load(std::memory_order_acquire)) {
      return {static_cast<u32>(reader.offset()), false};
    }
    const u8 cmd = reader.read<u8>();''')
    text = replace_once(text, '''\
      handle_bp(value);
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }''', '''\
      handle_bp(value);
      // Petari: stop at every synchronisation BP so fifo.cpp reports it at
      // this command boundary, before any later command is processed.
      if (const u32 reg = reg_get(value, 8, 24); reg == GX_BP_REG_DRAWDONE || reg == 0x47 || reg == 0x48) {
        petariSyncBp = value;
        return {static_cast<u32>(reader.offset()), reg == GX_BP_REG_DRAWDONE};
      }''')
    # Petari: a triangle fan or strip with fewer than 3 vertices (legal GX; it
    # draws nothing) made the unsigned reservation (vtxCount - 3) * 3 + 3 wrap
    # to about 8 GiB, which ByteBuffer::reserve_extra reallocated and zeroed
    # once and kept for the life of the process (the static index buffers).
    # Measured in the soak: host malloc 84 MB -> 8.3 GB at the first stage.
    text = replace_once(text, '''\
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));''', '''\
    buf.reserve_extra((vtxCount >= 3 ? (u32(vtxCount) - 3) * 3 + 3 : u32(vtxCount)) * sizeof(u16));''')
    text = replace_once(text, '''\
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));''', '''\
    buf.reserve_extra((vtxCount >= 3 ? (static_cast<u32>(vtxCount) - 3) * 3 + 3 : static_cast<u32>(vtxCount)) *
                      sizeof(u16));''')
    return text


def patch_manage(text):
    text = replace_once(text, 'constexpr u32 kDrawDoneCommand = static_cast<u32>(GX_BP_REG_DRAWDONE) << 24 | 2;\n', '')
    text = remove_function(text, 'void GXDrawDone() {')
    text = remove_function(text, 'void GXSetDrawDone() {')
    text = remove_function(text, 'GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {')
    return text


def patch_fifo_objects(text):
    text = replace_once(text, '#include "gx.hpp"\n', '#include "gx.hpp"\n\n#include "sync_backend.h"\n')
    text = remove_function(text, 'void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {')
    text = remove_function(text, 'void GXGetFifoPtrs(GXFifoObj* fifo, void** readPtr, void** writePtr) {')
    text = replace_once(text, 'GXFifoObj* GXGetCPUFifo() { return CPUFifo; }', 'const void* petari_aurora_cpu_fifo(void) { return CPUFifo; }')
    text = replace_once(text, 'GXFifoObj* GXGetGPFifo() { return GPFifo; }', 'const void* petari_aurora_gp_fifo(void) { return GPFifo; }')
    return text


def patch_cpu2efb(text):
    return remove_function(text, 'void GXPeekZ(u16 x, u16 y, u32* z) {')


PATCHES = {
    'fifo.cpp': patch_fifo,
    'command_processor.cpp': patch_command_processor,
    'GXManage.cpp': patch_manage,
    'GXFifo.cpp': patch_fifo_objects,
    'GXCpu2Efb.cpp': patch_cpu2efb,
}


def main():
    if len(sys.argv) != 3:
        fail('usage: patch_aurora_sync.py SOURCE OUTPUT')
    source, output = map(Path, sys.argv[1:])
    if source.name not in PATCHES:
        fail(f'no sync patch for {source.name}')
    text = source.read_text()
    if 'sync_backend.h' in text or 'petari_aurora_' in text:
        fail(f'{source} is already patched')
    text = PATCHES[source.name](text)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    main()

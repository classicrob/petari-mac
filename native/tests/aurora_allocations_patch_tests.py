#!/usr/bin/env python3
"""Tests for native/gx/patch_aurora_allocations.py against the pinned Aurora sources.

usage: aurora_allocations_patch_tests.py AURORA_DIR NATIVE_GX_DIR [CXX]

Soak finding: the render worker never popped an autorelease pool, so the objects Dawn's Metal backend
autoreleased while presenting and submitting (NSDictionary from -[_MTLCommandBuffer presentDrawable:],
plus AutoreleasePoolPage) stayed alive until the thread exited, about 0.35 MB per stage cycle. The
patch pops a pool per render work item, per FIFO process_to and per main-thread frame entry point.
With a compiler (CXX, on macOS), the generated pool is also compiled and checked to release an
autoreleased object on a thread that has no pool of its own.
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

checks = 0
POOL = "PetariAutoreleasePool petariAutoreleasePool;"
SCOPE = "PetariNative::HostAllocationScope petariHostAllocations;"
FILES = ["lib/aurora.cpp", "lib/dolphin/gx/GXManage.cpp", "lib/gx/fifo.cpp", "lib/gfx/render_worker.cpp",
         "lib/gfx/texture_replacement.cpp"]


def check(condition, label):
    global checks
    checks += 1
    if not condition:
        print(f"FAIL: {label}", file=sys.stderr)
        sys.exit(1)


def body(text, signature):
    """The brace-matched body of the function whose definition starts with signature."""
    start = text.index(signature)
    depth, i = 0, text.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
        if depth == 0:
            return text[start:i]


PROBE = r"""
#include <objc/message.h>
#include <objc/runtime.h>
#include <cstdio>
#include <optional>
#include <thread>
extern "C" id objc_autorelease(id value);
extern "C" id objc_initWeak(id* location, id value);
extern "C" id objc_loadWeakRetained(id* location);
extern "C" void objc_destroyWeak(id* location);
extern "C" void objc_release(id value);

// Autoreleases a new object on a fresh thread, the way Dawn does on the render worker, and reports
// whether it is still alive after the (optional) pool around the work item is gone.
static bool aliveAfterWorkItem(bool pool) {
  bool alive = false;
  std::thread([&] {
    id weak = nullptr;
    {
      std::optional<PetariAutoreleasePool> item;
      if (pool) item.emplace();
      id object = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
          reinterpret_cast<id>(objc_getClass("NSObject")), sel_registerName("new"));
      objc_initWeak(&weak, object);
      objc_autorelease(object);
    }
    id strong = objc_loadWeakRetained(&weak);
    alive = strong != nullptr;
    if (strong) objc_release(strong);
    objc_destroyWeak(&weak);
  }).join();
  return alive;
}

int main() {
  const bool leaked = aliveAfterWorkItem(false), kept = aliveAfterWorkItem(true);
  std::printf("%d %d\n", leaked, kept);
  return 0;
}
"""


def main():
    aurora, native_gx = Path(sys.argv[1]), Path(sys.argv[2])
    cxx = sys.argv[3] if len(sys.argv) > 3 else shutil.which("c++")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        patched = {}
        for relative in FILES:
            output = tmp / Path(relative).name
            result = subprocess.run([sys.executable, str(native_gx / "patch_aurora_allocations.py"),
                                     str(aurora / relative), str(output)], capture_output=True, text=True)
            check(result.returncode == 0, f"the patch applies to {relative}: {result.stderr.strip()}")
            patched[output.name] = output.read_text()

        worker = patched["render_worker.cpp"]
        main_loop = body(worker, "void worker_main(std::stop_token token) {")
        check(worker.count(POOL) == 1 and main_loop.count(POOL) == 1, "the render worker has exactly one pool, in worker_main")
        check(re.search(r"if \(item->work\) \{[^{}]*" + re.escape(POOL) + r"\s*item->work\(\);\s*\}", main_loop),
              "worker_main pops a pool per work item: it is scoped to the block that runs item->work()")
        check('extern "C" void* objc_autoreleasePoolPush(void);' in worker and
              'extern "C" void objc_autoreleasePoolPop(void* pool);' in worker, "the pool functions are declared extern \"C\"")
        check(POOL not in body(worker, "void enqueue(QueueItem item) {"), "enqueue's inline work runs inside the caller's pool")

        fifo = patched["fifo.cpp"]
        check(POOL in body(fifo, "void process_to(uint64_t target, std::memory_order order) noexcept {"),
              "the FIFO processor pops a pool per process_to")
        frontend = patched["aurora.cpp"]
        for entry in ["const AuroraEvent* aurora_update() {", "bool aurora_begin_frame() {", "void aurora_end_frame() {"]:
            check(POOL in body(frontend, entry), f"the main thread pops a pool around {entry}")
        check(POOL not in patched["texture_replacement.cpp"] and "objc_" not in patched["texture_replacement.cpp"],
              "the texture workers (CPU decode only) get no pool")
        check(POOL not in patched["GXManage.cpp"], "GX init/draw-done run inside the caller's pool")
        for name, text in patched.items():
            check(text.startswith("#include <petari/host_allocation.hpp>\n"), f"{name} includes host_allocation.hpp first")
            check(SCOPE in text, f"{name} still isolates renderer allocations")

        again = subprocess.run([sys.executable, str(native_gx / "patch_aurora_allocations.py"),
                                str(tmp / "render_worker.cpp"), str(tmp / "x" / "render_worker.cpp")],
                               capture_output=True, text=True)
        check(again.returncode != 0, "an already patched file is refused")

        if sys.platform == "darwin" and cxx:
            pool = re.search(r"#ifdef __APPLE__\n(.*?)#else\n", worker, re.S)[1]
            source = tmp / "probe.cpp"
            source.write_text(pool + PROBE)
            binary = tmp / "probe"
            # CMake passes the toolchain's compiler, which does not find the SDK on its own.
            sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
            sysroot = ["-isysroot", sdk] if sdk else []
            built = subprocess.run([cxx, *sysroot, "-std=c++20", "-O1", str(source), "-o", str(binary), "-lobjc"],
                                   capture_output=True, text=True)
            check(built.returncode == 0, f"the generated pool compiles and links against libobjc: {built.stderr.strip()}")
            leaked, kept = subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.split()
            check(leaked == "1", "without a pool an autoreleased object outlives its work item (the soak's leak)")
            check(kept == "0", "the generated pool releases the work item's autoreleased objects when it pops")
    print(f"aurora allocations patch tests passed ({checks} checks)")


if __name__ == "__main__":
    main()

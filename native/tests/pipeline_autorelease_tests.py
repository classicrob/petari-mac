#!/usr/bin/env python3
"""Compile the actual per-job pool and verify release and exception unwinding."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
worker = (root / 'native/gx/pipeline_worker.inc').read_text()
assert re.search(r'auto result = \[&\] \{\s*PetariPipelineAutoreleasePool pool;\s*return pending.create\(\);\s*\}\(\);', worker)
assert worker.index('PetariPipelineAutoreleasePool pool;') < worker.index('const auto finished =')
if sys.platform != 'darwin':
    print('Pool placement passed; Objective-C runtime probe requires macOS')
    raise SystemExit(0)
pool = worker[:worker.index('static void pipeline_worker')]
probe = r'''
#include <objc/message.h>
#include <objc/runtime.h>
#include <thread>
#include <cstdio>
extern "C" id objc_autorelease(id);
extern "C" id objc_initWeak(id*, id);
extern "C" id objc_loadWeakRetained(id*);
extern "C" void objc_destroyWeak(id*);
extern "C" void objc_release(id);
static bool run(bool fail) {
  bool released = false;
  std::thread([&] {
    id weak = nullptr;
    try {
      auto result = [&] {
        PetariPipelineAutoreleasePool pool;
        id object = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
          reinterpret_cast<id>(objc_getClass("NSObject")), sel_registerName("new"));
        if (!object) throw 2;
        objc_initWeak(&weak, object);
        objc_autorelease(object);
        if (fail) throw 1;
        return 42;
      }();
      if (result != 42) return;
    } catch (int reason) { if (reason != 1) return; }
    id strong = objc_loadWeakRetained(&weak);
    released = strong == nullptr;
    if (strong) objc_release(strong);
    objc_destroyWeak(&weak);
  }).join();
  return released;
}
int main() {
  if (!run(false) || !run(true)) return 1;
  std::puts("Per-compile autorelease pool drains on return and exception");
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'probe.cpp'
    binary = Path(directory) / 'probe'
    source.write_text(pool + probe)
    sdk = subprocess.check_output(['xcrun', '--show-sdk-path'], text=True).strip()
    subprocess.run([sys.argv[1], '-isysroot', sdk, '-std=c++20', '-O1', str(source), '-o', str(binary), '-lobjc'], check=True)
    subprocess.run([str(binary)], check=True)

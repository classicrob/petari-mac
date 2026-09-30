#!/usr/bin/env python3
"""Prepare ImGui's WebGPU pipeline during renderer initialization, before OS startup."""
from pathlib import Path
import sys


def patch(text):
    anchor = '    ImGui_ImplWGPU_Init(&info);'
    if text.count(anchor) != 1:
        raise ValueError('Aurora ImGui pipeline prewarm anchor mismatch')
    replacement = '''    ImGui_ImplWGPU_Init(&info);
    // NewFrame otherwise compiles this pipeline lazily on the first game frame.
    const auto petariImGuiStart = std::chrono::steady_clock::now();
    const bool petariImGuiReady = ImGui_ImplWGPU_CreateDeviceObjects();
    const auto petariImGuiMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - petariImGuiStart).count();
    std::fprintf(stderr, "[gx pipeline prewarm] kind=imgui ready=%d build_ms=%.3f\\n",
                 petariImGuiReady ? 1 : 0, petariImGuiMs);'''
    return '#include <chrono>\n#include <cstdio>\n' + text.replace(anchor, replacement)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: patch_aurora_pipeline_imgui.py SOURCE OUTPUT')
    source, output = map(Path, sys.argv[1:])
    try:
        result = patch(source.read_text())
    except ValueError as error:
        raise SystemExit(str(error))
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != result:
        output.write_text(result)

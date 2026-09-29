#!/usr/bin/env python3
"""Opt-in FIFO-ordered producer stacks for runtime pipeline first-use attribution."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:3])
kind = sys.argv[3]
text = source.read_text()
header = Path(__file__).with_name('pipeline_owner.hpp').resolve()
text = f'#include "{header}"\n' + text
if kind == 'command_processor.cpp':
    old = '''  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));'''
    new = '''  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    if (!PetariPipelineOwner::consume(label)) gfx::insert_debug_marker(std::move(label));'''
else:
    old = 'void pre_begin() {' if kind == 'GXVert.cpp' else 'void GXCallDisplayList(const void* data, u32 nbytes) {'
    new = old + '\n  if (!aurora::gx::fifo::in_display_list()) PetariPipelineOwner::emit();'
if text.count(old) != 1: raise SystemExit('Pipeline owner anchor mismatch: ' + kind)
text = text.replace(old, new)
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text: output.write_text(text)

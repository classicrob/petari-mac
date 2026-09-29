#!/usr/bin/env python3
"""Add mid-frame EFB submissions while preserving Aurora's mapped staging lifetime."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
here = Path(__file__).resolve().parent
if source.name == 'recording.cpp':
    text = '#include "efb_snapshot.hpp"\n#include <petari/host_allocation.hpp>\n#include "../dolphin/vi/vi_internal.hpp"\n' + text
    text += '\n' + (here / 'efb_recording.inc').read_text()
elif source.name == 'frame.cpp':
    text = '#include "efb_snapshot.hpp"\n#include <future>\n' + text
    text += '\n' + (here / 'efb_frame.inc').read_text()
else:
    raise SystemExit(f'Unsupported Aurora readback patch source: {source}')
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

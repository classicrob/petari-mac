#!/usr/bin/env python3
"""Keep negative GX texture LOD bias conversions defined on the host."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
old = 'static_cast<u8>(32.0f * clampedBias)'
if text.count(old) != 2:
    raise SystemExit('Aurora texture LOD patch anchor mismatch')
text = text.replace(old, 'static_cast<u8>(static_cast<int>(32.0f * clampedBias))')
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

#!/usr/bin/env python3
"""Apply the game's depth scale/offset to Aurora viewport writes."""
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:])
text = source.read_text()
for old, new in [
    ('zmin = 1.6777215e7f * nearZ;', 'zmin = petari_gx_depth_scale() * nearZ;'),
    ('zmax = 1.6777215e7f * farZ;', 'zmax = petari_gx_depth_scale() * farZ;'),
    ('oz = zmax;', 'oz = zmax + petari_gx_depth_offset();'),
]:
    if text.count(old) != 1:
        raise SystemExit(f'Aurora viewport patch anchor mismatch: {old}')
    text = text.replace(old, new)
text = 'extern "C" float petari_gx_depth_scale();\nextern "C" float petari_gx_depth_offset();\n' + text
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

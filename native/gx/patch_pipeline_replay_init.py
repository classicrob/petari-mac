#!/usr/bin/env python3
"""Extract Aurora's CPU register-shadow initialization for offline API recording."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
start, end = '  // Initialize shadow registers:', '  // VAT initialization:'
if text.count(start) != 1 or text.count(end) != 1:
    raise SystemExit('GXInit shadow initialization anchors changed')
body = text[text.index(start):text.index(end)]
result = 'static void reset_shadow() {\n  std::memset(__gx, 0, sizeof(*__gx));\n  u32 i;\n' + body + '}\n'
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != result: output.write_text(result)

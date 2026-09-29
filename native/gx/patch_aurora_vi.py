#!/usr/bin/env python3
"""Keep Aurora window helpers while the platform library supplies the Wii VI API."""
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:])
text = source.read_text()
start = 'void VIInit() {}\n'
end = 'void VIFlush() {}\n'
if text.count(start) != 1 or text.count(end) != 1:
    raise SystemExit('Aurora VI patch anchor mismatch')
first = text.index(start)
last = text.index(end, first) + len(end)
text = text[:first] + text[last:]
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

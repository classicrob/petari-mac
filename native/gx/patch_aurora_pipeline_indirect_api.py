#!/usr/bin/env python3
"""Match the Wii SDK's NULL indirect-order conversion before BP packing."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
anchor = "void GXSetIndTexOrder(GXIndTexStageID indStage, GXTexCoordID texCoord, GXTexMapID texMap) {"
if text.count(anchor) != 1 or 'if (texMap == GX_TEXMAP_NULL)' in text:
    raise SystemExit('Indirect API patch anchor mismatch')
text = text.replace(anchor, anchor + "\n  if (texMap == GX_TEXMAP_NULL) texMap = GX_TEXMAP0;\n  if (texCoord == GX_TEXCOORD_NULL) texCoord = GX_TEXCOORD0;")
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

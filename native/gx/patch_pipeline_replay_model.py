#!/usr/bin/env python3
"""Extract the game's pure model-resource preprocessing for offline replay."""
from pathlib import Path
import re
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
functions = []
for name in ('isEnvelope', 'isUseTexMtx', 'isUseTexMtxEnvMap', 'isUseTexMtxProjMap',
             'setShapeVcdVatCmdSelf', 'initEnvelopeAndEnvMapOrProjMapModelData', 'downFracVtx', 'isUseFur'):
    pattern = r'^    (?:inline )?(?:bool|void) ' + name + r'\([^\n]*\) \{'
    matches = list(re.finditer(pattern, text, re.M))
    if len(matches) != 1: raise SystemExit('Model replay anchor changed: ' + name)
    start = matches[0].start(); pos = text.index('{', start); depth = 1; pos += 1
    while depth:
        if text[pos] == '{': depth += 1
        elif text[pos] == '}': depth -= 1
        pos += 1
    functions.append(text[start:pos])
result = 'namespace ReplayModel {\n' + '\n'.join(functions).replace('copyMemory(arr, vcdVatCmd,', 'std::memcpy(arr, vcdVatCmd,') + '\n}\n'
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != result: output.write_text(result)

#!/usr/bin/env python3
"""Keep negative GX texture LOD bias conversions defined on the host, and sample
depth-format textures as the texture unit does."""
from pathlib import Path
import sys
source, output = map(Path, sys.argv[1:])
text = source.read_text()
old = 'static_cast<u8>(32.0f * clampedBias)'
if text.count(old) != 2:
    raise SystemExit('Aurora texture LOD patch anchor mismatch')
text = text.replace(old, 'static_cast<u8>(static_cast<int>(32.0f * clampedBias))')

# The texture unit's format field is 4 bits (init_texobj_common writes
# format & 0xF into image0): GX_TF_Z8, GX_TF_Z16 and GX_TF_Z24X8 (I8, IA8 and
# RGBA8 | _GX_TF_ZTF) are sampled as those base formats; the ZTF bit only
# matters to GXSetZTexture. Aurora's converter knows only the base formats and
# aborted ("unknown texture format 22") when a draw sampled texture map 0 still
# holding the game's 4x4 GX_TF_Z24X8 depth-clear texture (Petari: Good Egg's
# launch star). GXGetTexObjFmt keeps the full format.
old = '  GX_WRITE_U32(obj.format());\n'
if text.count(old) != 1:
    raise SystemExit('Aurora texture depth-format patch anchor mismatch')
text = text.replace(old, '''  u32 sampledFormat = obj.format();
  if ((sampledFormat & (_GX_TF_ZTF | _GX_TF_CTF)) == _GX_TF_ZTF) {
    sampledFormat &= 0xF;
    static bool sLogged[16] = {};
    if (!sLogged[sampledFormat]) {
      sLogged[sampledFormat] = true;
      std::fprintf(stderr, "[gx texture] depth-format texture 0x%x (%ux%u at %p) loaded for sampling; sampled as base format 0x%x\\n",
                   obj.format(), obj.width(), obj.height(), obj.data, sampledFormat);
    }
  }
  GX_WRITE_U32(sampledFormat);
''')
if '#include <cstdio>' not in text:
    text = text.replace('#include <algorithm>\n', '#include <algorithm>\n#include <cstdio>\n', 1)
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

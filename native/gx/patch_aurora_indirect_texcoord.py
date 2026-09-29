#!/usr/bin/env python3
"""Apply GX's indirect-coordinate fallback in shader analysis and generation."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()

def replace(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f'Aurora indirect texcoord patch anchor mismatch: {old}')
    text = text.replace(old, new)

replace('namespace {\n', '''namespace {
// populate_pipeline_config copies only enabled texgens; inactive entries retain
// GX_MAX_TEXGENSRC. GX indirect orders outside that range select coordinate 0.
// See Dolphin PixelShaderGen.cpp (Luigi's Mansion bug 11462).
GXTexCoordID indirect_texcoord(const ShaderConfig& config, GXTexCoordID requested) {
  if (requested < config.tcgs.size() && config.tcgs[requested].src != GX_MAX_TEXGENSRC) {
    return requested;
  }
  return config.tcgs[0].src != GX_MAX_TEXGENSRC ? GX_TEXCOORD0 : GX_TEXCOORD_NULL;
}

''')

if source.name == 'shader_info.cpp':
    replace('    info.sampledTexCoords.set(indStage.texCoordId);', '''    const auto indCoord = indirect_texcoord(config, indStage.texCoordId);
    if (indCoord != GX_TEXCOORD_NULL) {
      info.sampledTexCoords.set(indCoord);
    }''')
elif source.name == 'shader.cpp':
    replace('    const u32 texCoordId = underlying(indStage.texCoordId);',
            '    const u32 texCoordId = underlying(indirect_texcoord(config, indStage.texCoordId));')
    replace('''    const auto scaleExpr =
        fmt::format("tex{0}_uv * ubuf.texcoord_scale[{0}].xy * vec2f({1}, {2}) / ubuf.tex{3}_size_bias.xy", texCoordId,
                    ind_scale(indStage.scaleS), ind_scale(indStage.scaleT), texMapId);''', '''    // With no generated coordinates, use Dolphin's zero-coordinate approximation.
    const auto scaleExpr = texCoordId == GX_TEXCOORD_NULL ? std::string("vec2f(0.0)") :
        fmt::format("tex{0}_uv * ubuf.texcoord_scale[{0}].xy * vec2f({1}, {2}) / ubuf.tex{3}_size_bias.xy", texCoordId,
                    ind_scale(indStage.scaleS), ind_scale(indStage.scaleT), texMapId);''')
else:
    raise SystemExit(f'Unexpected Aurora source: {source.name}')

output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

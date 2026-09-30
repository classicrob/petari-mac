#!/usr/bin/env python3
"""Preserve the complete XF post-matrix row bank, including unaligned selectors."""
from pathlib import Path
import sys


def patch(text, kind):
    header = Path(__file__).resolve().with_name('pipeline_postmatrix.hpp')
    text = f'#include "{header}"\n' + text

    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError(f'Post-matrix patch anchor mismatch ({kind}): {old[:100]}')
        text = text.replace(old, new)

    if kind == 'regs':
        replace('bool copy_xf_data(u32 addr, const u8* data, u32 len, std::endian e) noexcept {', '''bool copy_xf_data(u32 addr, const u8* data, u32 len, std::endian e) noexcept {
  if (addr >= 0x500 && addr < 0x600) {
    bool changed = false;
    const u32 offset = addr - 0x500;
    const u32 count = std::min(len, 0x100u - offset);
    for (u32 i = 0; i < count; ++i) {
      const u32 word = offset + i;
      auto& row = petariPostMatrixRows[word / 4];
      auto& component = row[word % 4];
      const auto value = read_bits<u32>(data + i * 4, e);
      changed |= store_xf_f32(component, value);
      // Keep the legacy complete-matrix view coherent for non-shader consumers.
      if (word < MaxPTTexMtx * 12) {
        auto* matrix = reinterpret_cast<f32*>(&g_gxState.ptTexMtxs[word / 12]);
        store_xf_f32(matrix[word % 12], value);
      }
    }
    if (changed) g_gxState.dirty |= DirtyUniform;
    return true;
  }''')
    elif kind == 'shader_info':
        replace('''    if (tcg.postMtx != GX_PTIDENTITY) {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      info.usesPTTexMtx.set(postMtxIdx);
    }''', '''    // This bitset is only a presence flag: every sampled texgen reads the
    // same complete row bank, including the SDK's writable identity rows.
    info.usesPTTexMtx.set(0);''')
        replace('info.uniformSize += sizeof(Mat3x4<float>) * MaxPTTexMtx;',
                'info.uniformSize += sizeof(Vec4<float>) * petariPostMatrixRows.size();')
        replace('''    for (int i = 0; i < info.usesPTTexMtx.size(); ++i) {
      buf.append(g_gxState.ptTexMtxs[i]);
    }''', '    buf.append(petariPostMatrixRows);')
    elif kind == 'shader':
        replace('''    if (tcg.postMtx == GX_PTIDENTITY) {
      vtxXfrAttrs += fmt::format("\\n    var tc{0}_proj = tc{0}_tmp;", i);
    } else {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      vtxXfrAttrs +=
          fmt::format("\\n    var tc{0}_proj = vec4f(tc{0}_tmp.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
    }''', '''    vtxXfrAttrs += fmt::format(
        "\\n    var tc{0}_proj = vec3f("
        "dot(vec4f(tc{0}_tmp.xyz, 1.0), ubuf.postmtx[{1}]), "
        "dot(vec4f(tc{0}_tmp.xyz, 1.0), ubuf.postmtx[{2}]), "
        "dot(vec4f(tc{0}_tmp.xyz, 1.0), ubuf.postmtx[{3}]));",
        i, petari_postmatrix_row(unsigned(tcg.postMtx)),
        petari_postmatrix_row(unsigned(tcg.postMtx), 1), petari_postmatrix_row(unsigned(tcg.postMtx), 2));''')
        replace('uniBufAttrs += fmt::format("\\n    postmtx: array<mat3x4f, {}>,", MaxPTTexMtx);',
                'uniBufAttrs += "\\n    postmtx: array<vec4f, 64>,";')
    else:
        raise ValueError(f'Unsupported post-matrix patch kind {kind}')
    return text


if __name__ == '__main__':
    source, output = map(Path, sys.argv[1:3])
    text = patch(source.read_text(), sys.argv[3])
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)

#!/usr/bin/env python3
"""Build a local copy of pinned Aurora's BP decoder for Wii texture addresses.

The source checkout stays untouched. Exact anchors deliberately fail if the
pinned backend changes, rather than silently applying an incompatible patch.
"""
import sys
from pathlib import Path

source, output = map(Path, sys.argv[1:])
text = source.read_text()
def replace(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f'Aurora register patch anchor mismatch: {old[:90]}')
    text = text.replace(old, new)

replace('namespace aurora::gx::fifo {', '''extern "C" void* PetariNativePhysicalToHost(uintptr_t address);

namespace aurora::gx::fifo {''')
replace('// TLUT load trigger (0x65)', '''// Legacy J3D addresses palette memory by its Wii TMEM block, while Aurora's
// API addresses it by a small logical slot. Retain all TMEM block bindings and
// materialize each texture unit's palette when its TLUT register is written.
struct PetariPalette { const void* data = nullptr; u16 entries = 0; };
std::array<PetariPalette, 1024> petariPalettes{};

// TLUT load trigger (0x65)''')
replace('''  const auto idx = reg_get(value, 10, 0);
  if (idx < MaxTluts) {''', '''  const auto idx = reg_get(value, 10, 0);
  const auto address = reg_get(g_gxState.bpRegCache[0x64], 24, 0) << 5;
  if (address != 0) {
    petariPalettes[idx] = {PetariNativePhysicalToHost(address),
                          static_cast<u16>(reg_get(value, 11, 10) * 16)};
  }
  if (idx < MaxTluts) {''')
replace('''  case 1: // Mode1
    slot.mode1 = value;
    break;''', '''  case 1: // Mode1
    slot.mode1 = value;
    if ((slot.image3 & 0xFFFFFF) != 0) {
      slot.flags = (slot.flags & ~1u) | (reg_get(value, 8, 8) != 0 ? 1u : 0u);
    }
    break;''')
replace('''  case 5: // Image3
    slot.image3 = value;
    break;''', '''  case 5: // Image3
    slot.image3 = value;
    if (const auto address = reg_get(value, 24, 0) << 5; address != 0) {
      slot.data = PetariNativePhysicalToHost(address);
      slot.mWidth = 0;
      slot.mHeight = 0;
      slot.mFormat = gfx::InvalidTextureFormat;
      slot.texObjId = 0;
      slot.texDataVersion = 0;
      slot.flags = reg_get(slot.mode1, 8, 8) != 0 ? 1 : 0;
    }
    break;
  case 6: // TLUT address and format
    if ((slot.image3 & 0xFFFFFF) != 0) {
      const auto& source = petariPalettes[reg_get(value, 10, 0)];
      CHECK(source.data != nullptr && source.entries != 0, "J3D palette used before loading");
      auto& palette = g_gxState.loadedTluts[texMapId];
      palette.data = source.data;
      palette.numEntries = source.entries;
      palette.format = static_cast<GXTlutFmt>(reg_get(value, 2, 10));
      palette.tlutObjId = 0;
      palette.tlutDataVersion = 0;
      palette.flags = 0;
      slot.tlut = static_cast<GXTlut>(texMapId);
    }
    break;''')
replace('regs[base + 0x18 + i] = {};                      // TLUT region TMEM offset',
        'regs[base + 0x18 + i] = {bp_tex, DirtyTextures, true}; // TLUT region TMEM offset')
# Scissor offset is part of the Wii rasterizer state, including display lists.
replace('  const u32 scis0 = g_gxState.bpRegCache[0x20];',
        '  const u32 scis0 = g_gxState.bpRegCache[0x20];\n  const u32 offset = g_gxState.bpRegValid[0x59] ? g_gxState.bpRegCache[0x59] : (171u | (171u << 10));')
replace('static_cast<s32>(reg_get(scis0, 11, 0)) - 342', 'static_cast<s32>(reg_get(scis0, 11, 0)) - static_cast<s32>(reg_get(offset, 10, 10) * 2)')
replace('static_cast<s32>(reg_get(scis0, 11, 12)) - 342', 'static_cast<s32>(reg_get(scis0, 11, 12)) - static_cast<s32>(reg_get(offset, 10, 0) * 2)')
replace('static_cast<s32>(reg_get(scis1, 11, 0)) - 342', 'static_cast<s32>(reg_get(scis1, 11, 0)) - static_cast<s32>(reg_get(offset, 10, 10) * 2)')
replace('static_cast<s32>(reg_get(scis1, 11, 12)) - 342', 'static_cast<s32>(reg_get(scis1, 11, 12)) - static_cast<s32>(reg_get(offset, 10, 0) * 2)')
replace('  regs[0x21] = {bp_scissor};', '  regs[0x21] = {bp_scissor};\n  regs[0x59] = {bp_scissor};')
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

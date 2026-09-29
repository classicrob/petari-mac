#!/usr/bin/env python3
"""Frame telemetry for first-use GX textures (petari/frame_telemetry.hpp): time spent
hashing texture sources and creating/converting/uploading static textures."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
if source.parent.name == 'gx':
    anchor = ('TextureKeys hash_texture_source(const GXTexObj_& obj, const GXTlutObj_* tlut, bool buildSourceKey) {\n'
              '  ZoneScoped;\n')
    counter = 'TextureHash'
elif source.parent.name == 'gfx':
    anchor = ('TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,\n'
              '                                    bool tlut, const char* label) noexcept {\n'
              '  ZoneScoped;\n')
    counter = 'TextureUpload'
else:
    raise SystemExit(f'Unsupported Aurora texture timing source: {source}')
if text.count(anchor) != 1:
    raise SystemExit(f'Aurora texture timing patch anchor mismatch in {source}')
text = text.replace(anchor, anchor + f'  PetariNative::FrameTelemetry::Scope petariTiming{{PetariNative::FrameTelemetry::{counter}}};\n')
text = '#include <petari/frame_telemetry.hpp>\n' + text
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    output.write_text(text)

#!/usr/bin/env python3
"""Display copies and XFB presentation for Aurora (native/gx/present/README.md).

usage: patch_aurora_present.py SOURCE OUTPUT

SOURCE is Aurora's lib/dolphin/gx/GXFrameBuffer.cpp (original) or lib/aurora.cpp
(original or the output of patch_aurora_allocations.py; run that first).

- GXFrameBuffer.cpp: GXCopyDisp snapshots the EFB display-copy rectangle into a
  render texture kept by XFB address, in FIFO order, through Aurora's texture
  copy path. The display-copy setters keep real state; GXSetCopyClamp is
  defined. Texture-copy state is restored after each display copy.
- aurora.cpp: aurora_end_frame presents the XFB that VI latched (see
  present_aurora.hpp) instead of the EFB, with VI black and dimming, at the
  display aspect. The patched file includes "present_aurora.hpp", so the
  target needs native/gx/present on its quote include path.
"""
import sys
from pathlib import Path


def fail(message):
    raise SystemExit(f'Aurora present patch: {message}')


def replace_once(text, old, new):
    if text.count(old) != 1:
        fail(f'anchor mismatch: {old.strip().splitlines()[0]}')
    return text.replace(old, new)


FRAMEBUFFER_STATE = '''
namespace {
// Display-copy state (GXSetDispCopySrc/Dst/YScale, GXSetCopyFilter,
// GXSetDispCopyGamma, GXSetCopyClamp). Defaults are GXInit's for a 640x480
// render mode.
struct DisplayCopyState {
  u16 left = 0;
  u16 top = 0;
  u16 width = 640;
  u16 height = 480;
  u16 stride = 640;
  f32 yScale = 1.0f;
  GXBool aa = GX_FALSE;
  u8 samplePattern[12][2] = {};
  GXBool vfilterEnabled = GX_FALSE;
  u8 vfilter[7] = {};
  GXGamma gamma = GX_GM_1_0;
  GXFBClamp clamp = static_cast<GXFBClamp>(GX_CLAMP_TOP | GX_CLAMP_BOTTOM);
};
DisplayCopyState sDisplayCopy;

// Last texture-copy source and destination. A display copy loads its own
// rectangle and size into the command processor's copy registers; these are
// loaded again afterwards so a later GXCopyTex sees the game's settings, as
// on hardware, where the two copies keep separate registers.
struct TexCopyState {
  bool srcSet = false;
  u32 left = 0, top = 0, width = 0, height = 0;
  bool dstSet = false;
  u32 dstWidth = 0, dstHeight = 0, format = 0;
  u8 mipmap = 0;
};
TexCopyState sTexCopy;

void write_copy_src(u32 left, u32 top, u32 width, u32 height) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_SRC);
  GX_WRITE_U32(left);
  GX_WRITE_U32(top);
  GX_WRITE_U32(width);
  GX_WRITE_U32(height);
}

void write_copy_dst(u32 width, u32 height, u32 format, u8 mipmap) {
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DST);
  GX_WRITE_U32(width);
  GX_WRITE_U32(height);
  GX_WRITE_U32(format);
  GX_WRITE_U8(mipmap);
}
} // namespace
'''

DISPLAY_FUNCTIONS = '''void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
  // Kept as state. The resolve does not apply the AA sample pattern or the
  // vertical (deflicker) filter; see native/gx/present/README.md.
  sDisplayCopy.aa = aa;
  if (sample_pattern != nullptr) {
    std::memcpy(sDisplayCopy.samplePattern, sample_pattern, sizeof(sDisplayCopy.samplePattern));
  }
  sDisplayCopy.vfilterEnabled = vf;
  if (vfilter != nullptr) {
    std::memcpy(sDisplayCopy.vfilter, vfilter, sizeof(sDisplayCopy.vfilter));
  }
}

void GXSetDispCopyGamma(GXGamma gamma) { sDisplayCopy.gamma = gamma; }

void GXSetCopyClamp(GXFBClamp clamp) { sDisplayCopy.clamp = clamp; }

void GXCopyDisp(void* dest, GXBool clear) {
  if (sDisplayCopy.gamma != GX_GM_1_0) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      std::fprintf(stderr, "GXCopyDisp: display gamma %d is not applied\\n", static_cast<int>(sDisplayCopy.gamma));
    }
  }
  // The XFB receives the source rectangle scaled vertically by the display
  // copy's Y scale; its lines are counted as the hardware does.
  const u32 lines = GXGetNumXfbLines(sDisplayCopy.height, sDisplayCopy.yScale);
  write_copy_src(sDisplayCopy.left, sDisplayCopy.top, sDisplayCopy.width, sDisplayCopy.height);
  write_copy_dst(sDisplayCopy.width, lines, GX_TF_RGBA8, 0);
  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DEST);
  GX_WRITE_U64(reinterpret_cast<u64>(dest));
  u32 copy = 0;
  SET_REG_FIELD(0, copy, 1, 11, clear != GX_FALSE);
  SET_REG_FIELD(0, copy, 1, 14, 0);
  SET_REG_FIELD(0, copy, 8, 24, 0x52);
  GX_WRITE_RAS_REG(copy);
  if (sTexCopy.srcSet) {
    write_copy_src(sTexCopy.left, sTexCopy.top, sTexCopy.width, sTexCopy.height);
  }
  if (sTexCopy.dstSet) {
    write_copy_dst(sTexCopy.dstWidth, sTexCopy.dstHeight, sTexCopy.format, sTexCopy.mipmap);
  }
  aurora::gx::fifo::publish();
}
'''


def patch_framebuffer(text):
    text = replace_once(text, '#include <algorithm>\n#include <cmath>\n',
                        '#include <algorithm>\n#include <cmath>\n#include <cstdio>\n#include <cstring>\n')
    # State after the file's own anonymous namespace.
    text = replace_once(text, '} // namespace\n\nnamespace aurora::gx {\n',
                        '} // namespace\n' + FRAMEBUFFER_STATE + '\nnamespace aurora::gx {\n')
    text = replace_once(text,
                        'void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {}\n\n'
                        'void GXSetDispCopyGamma(GXGamma gamma) {}\n\n'
                        'void GXCopyDisp(void* dest, GXBool clear) {}\n',
                        DISPLAY_FUNCTIONS)
    text = replace_once(text, 'void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {}\n',
                        'void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {\n'
                        '  sDisplayCopy.left = left;\n'
                        '  sDisplayCopy.top = top;\n'
                        '  sDisplayCopy.width = wd;\n'
                        '  sDisplayCopy.height = ht;\n'
                        '}\n')
    text = replace_once(text,
                        'void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {\n'
                        '  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_SRC);\n'
                        '  GX_WRITE_U32(left);\n'
                        '  GX_WRITE_U32(top);\n'
                        '  GX_WRITE_U32(wd);\n'
                        '  GX_WRITE_U32(ht);\n'
                        '}\n',
                        'void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {\n'
                        '  sTexCopy.srcSet = true;\n'
                        '  sTexCopy.left = left;\n'
                        '  sTexCopy.top = top;\n'
                        '  sTexCopy.width = wd;\n'
                        '  sTexCopy.height = ht;\n'
                        '  write_copy_src(left, top, wd, ht);\n'
                        '}\n')
    text = replace_once(text, 'void GXSetDispCopyDst(u16 wd, u16 ht) {}\n',
                        '// As on hardware, only the width (the XFB stride) matters; the number of\n'
                        '// lines follows from the source height and the Y scale.\n'
                        'void GXSetDispCopyDst(u16 wd, u16 ht) {\n'
                        '  (void)ht;\n'
                        '  sDisplayCopy.stride = wd;\n'
                        '}\n')
    text = replace_once(text,
                        'void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {\n'
                        '  GX_WRITE_AURORA(GX_AURORA_LOAD_COPY_DST);\n'
                        '  GX_WRITE_U32(wd);\n'
                        '  GX_WRITE_U32(ht);\n'
                        '  GX_WRITE_U32(fmt);\n'
                        '  GX_WRITE_U8(mipmap != GX_FALSE);\n'
                        '}\n',
                        'void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {\n'
                        '  sTexCopy.dstSet = true;\n'
                        '  sTexCopy.dstWidth = wd;\n'
                        '  sTexCopy.dstHeight = ht;\n'
                        '  sTexCopy.format = fmt;\n'
                        '  sTexCopy.mipmap = mipmap != GX_FALSE;\n'
                        '  write_copy_dst(wd, ht, fmt, mipmap != GX_FALSE);\n'
                        '}\n')
    text = replace_once(text, '// TODO GXSetDispCopyFrame2Field\n// TODO GXSetCopyClamp\n',
                        '// TODO GXSetDispCopyFrame2Field\n')
    text = replace_once(text, 'u32 GXSetDispCopyYScale(f32 vscale) { return 0; }\n',
                        'u32 GXSetDispCopyYScale(f32 vscale) {\n'
                        '  sDisplayCopy.yScale = vscale;\n'
                        '  return GXGetNumXfbLines(sDisplayCopy.height, vscale);\n'
                        '}\n')
    return text


def patch_aurora(text):
    text = replace_once(text, '#include "webgpu/gpu_prof.hpp"\n',
                        '#include "webgpu/gpu_prof.hpp"\n#include "present_aurora.hpp"\n')
    text = replace_once(
        text,
        '  const auto& presentSource = webgpu::present_source();\n'
        '  const auto viewport = webgpu::calculate_present_viewport(webgpu::g_graphicsConfig.surfaceConfiguration.width,\n'
        '                                                           webgpu::g_graphicsConfig.surfaceConfiguration.height,\n'
        '                                                           presentSource.size.width, presentSource.size.height);\n',
        '  // Petari: the frame shows the XFB that VI latched (present_aurora.hpp).\n'
        '  auto petariPresent = petari_present::take();\n'
        '  const auto& presentSource = webgpu::present_source();\n'
        '  const auto viewport = webgpu::calculate_present_viewport(\n'
        '      webgpu::g_graphicsConfig.surfaceConfiguration.width, webgpu::g_graphicsConfig.surfaceConfiguration.height,\n'
        '      petariPresent.active ? petariPresent.aspectWidth : presentSource.size.width,\n'
        '      petariPresent.active ? petariPresent.aspectHeight : presentSource.size.height);\n')
    text = replace_once(text,
                        '  gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,\n',
                        '  gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,\n'
                        '                  petariPresent = std::move(petariPresent),\n')
    text = replace_once(
        text,
        '      wgpu::BindGroup presentBindGroup;\n'
        '      if (rmlBindGroup && !rmlOverlay) {\n'
        '        presentBindGroup = rmlBindGroup;\n'
        '      } else {\n',
        '      wgpu::BindGroup presentBindGroup;\n'
        '      bool petariDrawImage = true;\n'
        '      bool petariDim = false;\n'
        '      if (rmlBindGroup && !rmlOverlay) {\n'
        '        presentBindGroup = rmlBindGroup;\n'
        '      } else if (petariPresent.active) {\n'
        '        petariDrawImage = petari_present::bind_image(petariPresent, presentBindGroup);\n'
        '        petariDim = petariDrawImage && petariPresent.dimOpacity > 0.f;\n'
        '      } else {\n')
    text = replace_once(
        text,
        '        pass.SetPipeline(webgpu::g_CopyPipeline);\n'
        '        pass.SetBindGroup(0, presentBindGroup, 0, nullptr);\n'
        '        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,\n'
        '                             webgpu::g_graphicsConfig.surfaceConfiguration.height);\n'
        '\n'
        '        pass.Draw(3);\n',
        '        set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,\n'
        '                             webgpu::g_graphicsConfig.surfaceConfiguration.height);\n'
        '        // Petari: VI black (or no XFB) leaves the cleared image black.\n'
        '        if (petariDrawImage) {\n'
        '          pass.SetPipeline(webgpu::g_CopyPipeline);\n'
        '          pass.SetBindGroup(0, presentBindGroup, 0, nullptr);\n'
        '          pass.Draw(3);\n'
        '        }\n'
        '        if (petariDim) {\n'
        '          petari_present::draw_dim(pass, petariPresent);\n'
        '        }\n')
    return text


def main():
    if len(sys.argv) != 3:
        fail('usage: patch_aurora_present.py SOURCE OUTPUT')
    source, output = map(Path, sys.argv[1:])
    text = source.read_text()
    if source.name == 'GXFrameBuffer.cpp':
        text = patch_framebuffer(text)
    elif source.name == 'aurora.cpp':
        text = patch_aurora(text)
    else:
        fail(f'unexpected source {source.name}')
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    main()

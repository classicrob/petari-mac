#!/usr/bin/env python3
"""Time shader generation and Dawn calls; compose after the indirect-texcoord patch."""
from pathlib import Path
import sys


def patch(text, kind):
    header = Path(__file__).resolve().with_name('pipeline_profile.hpp')
    text = f'#include "{header}"\n#include <algorithm>\n' + text

    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError(f'Aurora pipeline stages anchor mismatch: {old}')
        text = text.replace(old, new)

    if kind == 'shader.cpp':
        replace('  const auto shaderSource = build_shader_source(config, dstAlphaMode, normalAttachment);', '''  const auto petariSourceStart = PetariPipeline::Clock::now();
  const auto shaderSource = build_shader_source(config, dstAlphaMode, normalAttachment);
  PetariPipeline::stages.sourceMs += PetariPipeline::milliseconds(PetariPipeline::Clock::now() - petariSourceStart);
  PetariPipeline::stages.sourceBytes += shaderSource.size();
  PetariPipeline::stages.sourceLines += std::count(shaderSource.begin(), shaderSource.end(), '\\n');
  ++PetariPipeline::stages.shaderCount;
  PetariPipeline::stages.maxTevStages = std::max(PetariPipeline::stages.maxTevStages, unsigned(config.tevStageCount));
  PetariPipeline::stages.maxIndStages = std::max(PetariPipeline::stages.maxIndStages, unsigned(config.numIndStages));''')
        replace('  return webgpu::g_device.CreateShaderModule(&shaderDescriptor);', '''  PetariPipeline::dumpSource(hash, shaderSource);
  const auto petariModuleStart = PetariPipeline::Clock::now();
  auto petariModule = webgpu::g_device.CreateShaderModule(&shaderDescriptor);
  PetariPipeline::stages.moduleMs += PetariPipeline::milliseconds(PetariPipeline::Clock::now() - petariModuleStart);
  return petariModule;''')
    elif kind == 'gx.cpp':
        replace('  return g_device.CreateRenderPipeline(&descriptor);', '''  const auto petariPipelineStart = PetariPipeline::Clock::now();
  auto petariPipeline = g_device.CreateRenderPipeline(&descriptor);
  PetariPipeline::stages.pipelineMs += PetariPipeline::milliseconds(PetariPipeline::Clock::now() - petariPipelineStart);
  return petariPipeline;''')
    else:
        raise ValueError(f'Unsupported pipeline timing input: {kind}')
    return text


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit('usage: patch_aurora_pipeline_stages.py SOURCE OUTPUT [shader.cpp|gx.cpp]')
    source, output = map(Path, sys.argv[1:3])
    try:
        text = patch(source.read_text(), sys.argv[3] if len(sys.argv) == 4 else source.name)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    main()

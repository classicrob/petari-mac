#!/usr/bin/env python3
"""Optional timing patch for pinned Dawn Metal sources (not the prebuilt package)."""
from pathlib import Path
import sys

PREAMBLE = '''#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
namespace {
using PetariMetalClock = std::chrono::steady_clock;
void petariMetalTiming(std::string_view label, const char* phase, PetariMetalClock::time_point start,
                       size_t bytes = 0) {
    const auto elapsed = std::chrono::duration<double, std::milli>(PetariMetalClock::now() - start).count();
    const char* enabled = std::getenv("PETARI_PIPELINE_DIAG");
    if (enabled && *enabled && *enabled != '0')
        std::fprintf(stderr, "[gx metal compile] label=%.*s phase=%s ms=%.3f bytes=%zu\\n",
                     static_cast<int>(label.size()), label.data(), phase, elapsed, bytes);
}
}
'''


def patch(text, kind):
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError(f'Dawn pipeline timing anchor mismatch: {old}')
        text = text.replace(old, new)

    if kind == 'ShaderModuleMTL.mm':
        replace('''    CacheResult<MslCompilation> mslCompilation;
    DAWN_TRY_ASSIGN(mslCompilation,''', '''    const auto petariTranslationStart = PetariMetalClock::now();
    CacheResult<MslCompilation> mslCompilation;
    DAWN_TRY_ASSIGN(mslCompilation,''')
        anchor = '                                   GetStrictMath().value_or(false), pipelineImmediateMask));'
        replace(anchor, anchor + '''
    petariMetalTiming(GetLabel(), "tint_or_blob_cache", petariTranslationStart, mslCompilation->msl.size());''')
        replace('''        library = AcquireNSPRef([mtlDevice newLibraryWithSource:mslSource.Get()
                                                        options:compileOptions.Get()
                                                          error:&error]);''', '''        const auto petariLibraryStart = PetariMetalClock::now();
        library = AcquireNSPRef([mtlDevice newLibraryWithSource:mslSource.Get()
                                                        options:compileOptions.Get()
                                                          error:&error]);
        petariMetalTiming(GetLabel(), "newLibraryWithSource", petariLibraryStart, mslCompilation->msl.size());''')
    elif kind == 'RenderPipelineMTL.mm':
        anchor = '''    mMtlRenderPipelineState =
        AcquireNSPRef([mtlDevice newRenderPipelineStateWithDescriptor:descriptorMTL error:&error]);'''
        replace(anchor, '    const auto petariStateStart = PetariMetalClock::now();\n' + anchor + '''
    petariMetalTiming(GetLabel(), "newRenderPipelineState", petariStateStart);''')
    else:
        raise ValueError(f'Unsupported Dawn source: {kind}')
    return PREAMBLE + text


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: patch_pipeline_dawn_timings.py SOURCE OUTPUT')
    source, output = map(Path, sys.argv[1:])
    try:
        result = patch(source.read_text(), source.name)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(result)

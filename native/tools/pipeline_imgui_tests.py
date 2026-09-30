#!/usr/bin/env python3
"""Exercise the patched Aurora initializer and real first-frame logic with CPU stubs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gx'))
from patch_aurora_pipeline_imgui import patch


def function(text, signature):
    start = text.index(signature)
    cursor = text.index('{', start)
    depth = 1
    end = cursor + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


STUBS = r'''
#include <chrono>
#include <cstdio>
#include <cmath>
#define ZoneScoped
using WGPUTextureFormat = int;
struct SDL_Renderer {};
struct ImVec2 { float x, y; };
struct AuroraWindowSize { int width=640,height=480,native_fb_width=640,native_fb_height=480; float scale=1; };
static unsigned builds=0, heldBuilds=0;
static bool batonHeld=false, useSDL=false;
struct Font { bool built=false; bool IsBuilt() {return built;} };
static Font fonts;
struct IO { Font* Fonts=&fonts; ImVec2 DisplayFramebufferScale{},DisplaySize{}; };
using ImGuiIO = IO;
namespace ImGui { static IO io; IO& GetIO(){return io;} void NewFrame(){} }
namespace window { SDL_Renderer* get_sdl_renderer(){static SDL_Renderer r;return useSDL?&r:nullptr;} void* get_sdl_window(){return nullptr;} }
namespace webgpu { struct Device {int Get(){return 1;}} g_device; struct {struct {int format=1;} surfaceConfiguration;} g_graphicsConfig; }
struct ImGui_ImplWGPU_InitInfo { int Device=0,RenderTargetFormat=0; };
struct ImGui_ImplWGPU_Data { bool pipelineState=false; };
static ImGui_ImplWGPU_Data backend;
ImGui_ImplWGPU_Data* ImGui_ImplWGPU_GetBackendData(){return &backend;}
void ImGui_ImplSDL3_InitForSDLRenderer(void*,SDL_Renderer*){}
void ImGui_ImplSDLRenderer3_Init(SDL_Renderer*){}
void ImGui_ImplWGPU_Init(ImGui_ImplWGPU_InitInfo*){}
bool ImGui_ImplWGPU_CreateDeviceObjects(){++builds;heldBuilds+=batonHeld;backend.pipelineState=true;fonts.built=true;return true;}
void ImGui_ImplSDLRenderer3_NewFrame(){}
void ImGui_ImplSDL3_NewFrame(){}
void SDL_GetRenderScale(SDL_Renderer*,float* x,float* y){*x=*y=1;}
void SDL_GetRenderOutputSize(SDL_Renderer*,int* w,int* h){*w=640;*h=480;}
static float g_scale=0;
static bool g_useSdlRenderer=false;
'''
MAIN = r'''
int main() {
    initialize();
    if (builds!=1 || heldBuilds) return 1;
    batonHeld=true;
    new_frame(AuroraWindowSize{});
    if (builds!=1 || heldBuilds) return 2;
    batonHeld=false; useSDL=true;
    initialize();
    new_frame(AuroraWindowSize{});
    if (builds!=1 || heldBuilds) return 3;
    std::puts("ImGui pipeline prewarm: initialized before baton, no first-frame recompile, SDL path unchanged");
}
'''


def main(args):
    original = (args.aurora / 'lib/imgui.cpp').read_text()
    backend = args.backend.read_text()
    updated = patch(original)
    for invalid in ('no anchor', original + '\n    ImGui_ImplWGPU_Init(&info);'):
        try: patch(invalid)
        except ValueError: pass
        else: raise RuntimeError('patch accepted an ambiguous source')
    init = function(updated, 'void initialize() noexcept')
    first = function(updated, 'void new_frame(const AuroraWindowSize& size) noexcept')
    lazy = function(backend, 'void ImGui_ImplWGPU_NewFrame()')
    with tempfile.TemporaryDirectory(prefix='pipeline-imgui-', dir='build') as temporary:
        root = Path(temporary)
        source, binary = root / 'test.cpp', root / 'test'
        source.write_text(STUBS + '\n' + lazy + '\n' + init + '\n' + first + '\n' + MAIN)
        subprocess.run(['c++', '-std=c++20', '-O1', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary.resolve())], check=True)
        lazy_main = MAIN.replace('    if (builds!=1 || heldBuilds) return 1;\n', '')
        source.write_text(STUBS + '\n' + lazy + '\n' + function(original, 'void initialize() noexcept') + '\n' + first + '\n' + lazy_main)
        subprocess.run(['c++', '-std=c++20', '-O1', str(source), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary.resolve())])
        if result.returncode != 2: raise RuntimeError('original first frame did not compile while holding the baton')
        print('Original lazy initializer rejected by exit status; no intentional abort')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--aurora', type=Path, required=True)
    parser.add_argument('--backend', type=Path, default=Path('build/aurora/_deps/imgui-src/backends/imgui_impl_wgpu.cpp'))
    main(parser.parse_args())

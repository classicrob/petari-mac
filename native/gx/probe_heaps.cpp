#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <revolution/gx.h>
#include <cstdio>
#include <cstdlib>

namespace {
JKRExpHeap* root;
JKRExpHeap* scene;
s32 emptySceneBytes;

void createScene() {
    scene = JKRExpHeap::create(2 * 1024 * 1024, root, false);
    if (!scene) std::abort();
    scene->becomeCurrentHeap();
    emptySceneBytes = scene->getTotalFreeSize();
}
}

extern "C" void petari_probe_init_heaps() {
    root = JKRExpHeap::createRoot(1, false);
    if (!root) std::abort();
    createScene();
    void* fifo = root->alloc(0x40000, 32);
    if (!fifo || !GXInit(fifo, 0x40000)) std::abort();
    if (scene->getTotalFreeSize() != emptySceneBytes) std::abort();
}

extern "C" void* petari_probe_texture_storage(unsigned bytes) {
    void* memory = root->alloc(bytes, 32);
    if (!memory) std::abort();
    return memory;
}

extern "C" void petari_probe_check_heap(unsigned frame) {
    if (!scene->check() || scene->getTotalFreeSize() != emptySceneBytes) {
        std::fprintf(stderr, "Renderer allocated into the scene heap at frame %u\n", frame);
        std::abort();
    }
    if (frame && frame % 60 == 0) {
        root->becomeCurrentHeap();
        JKRHeap::destroy(scene);
        createScene();
    }
}

extern "C" void petari_probe_finish_heaps() {
    root->becomeCurrentHeap();
    JKRHeap::destroy(scene);
    scene = nullptr;
    std::puts("Renderer scene-heap isolation and repeated scene-heap destruction passed.");
}

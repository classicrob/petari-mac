#pragma once
// One-shot screenshot of the next presented game frame (the XFB the game copied,
// at the renderer's resolution, without ImGui overlays), written as PNG by the
// presenter (native/gx/present/present_aurora.cpp). Header-only so game-side
// callers need no renderer dependency; one binary shares the inline state.
#include <atomic>
#include <cstdio>
#include <cstring>

namespace PetariNative::Screenshot {
inline std::atomic<bool> pending{false};
inline char path[1024] = {};  // written before `pending` is set, read after it is taken

// Any thread. False if a screenshot is still waiting to be written.
inline bool request(const char* file) {
    if (pending.load(std::memory_order_acquire)) return false;
    std::snprintf(path, sizeof(path), "%s", file);
    pending.store(true, std::memory_order_release);
    return true;
}
}  // namespace PetariNative::Screenshot

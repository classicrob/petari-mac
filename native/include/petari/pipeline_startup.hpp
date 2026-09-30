#pragma once

#include <cstdint>

struct SDL_Window;

namespace PetariNative {
struct PipelinePreparationView {
    std::uint32_t completed = 0;
    std::uint32_t total = 0;
    double remainingSeconds = -1.0;
    bool global = true;
};
namespace App {
// Before game/OS startup. Returns false if the window closes or compilation fails.
bool preparePipelines(SDL_Window* window);
}
namespace HomeMenu {
bool drawPipelinePreparation(const PipelinePreparationView& view, float width, float height);
}
}

extern "C" void petari_gx_pipeline_preparation_status(std::uint32_t* total, std::uint32_t* pending,
                                                     std::uint32_t* failed);
extern "C" bool petari_gx_pipeline_full_preparation();

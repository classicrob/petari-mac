#include <petari/pipeline_startup.hpp>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() try {
    using namespace PetariNative;
    check(!HomeMenu::drawPipelinePreparation({}, 1280, 720), "missing ImGui context cannot draw");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr; io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int atlasWidth, atlasHeight;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
    const ImVec2 sizes[] = {{1280, 720}, {640, 480}, {1024, 768}, {375, 812}};
    for (const auto size : sizes) {
        io.DisplaySize = size;
        for (double remaining : {-1.0, 22.0, 215.0, std::numeric_limits<double>::quiet_NaN()}) {
            ImGui::NewFrame();
            check(HomeMenu::drawPipelinePreparation({1234, 8713, remaining, true}, size.x, size.y), "startup overlay missing");
            ImGui::Render();
            const auto* data = ImGui::GetDrawData();
            check(data && data->TotalVtxCount > 500, "progress text/bar produced insufficient geometry");
            for (int list = 0; list < data->CmdListsCount; ++list) {
                for (const auto& vertex : data->CmdLists[list]->VtxBuffer) {
                    check(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y), "invalid startup vertex");
                    check(vertex.pos.x >= -1 && vertex.pos.y >= -1 && vertex.pos.x <= size.x + 1 && vertex.pos.y <= size.y + 1,
                          "startup overlay exceeds resized window");
                }
            }
        }
    }
    ImGui::DestroyContext();
    std::puts("Preparation overlay: text/bar geometry, ETA states and four window sizes pass");
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}

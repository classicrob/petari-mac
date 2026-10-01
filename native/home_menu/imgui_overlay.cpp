// Draws the native menu with Dear ImGui. Only ImGui and the menu's own header
// are included here, so ImGui's types never meet the SDK's GX headers.

#include "petari/home_menu.hpp"
#include "petari/launch_stage.hpp"
#include "petari/progress.hpp"
#include "petari/pipeline_startup.hpp"

#include <imgui.h>

#ifdef PETARI_HOME_MENU_INPUT
#include <string>

#include "petari/host_allocation.hpp"
#include "petari/input.hpp"
#endif

#include <algorithm>
#include <vector>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace PetariNative::HomeMenu {
namespace App = PetariNative::App;

namespace {

struct ImageSpace {
    float x, y, width, height;

    ImVec2 point(float kx, float ky) const { return {x + (kx + 1.0f) * 0.5f * width, y + (ky + 1.0f) * 0.5f * height}; }
};

ImU32 color(float r, float g, float b, float a) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, std::clamp(a, 0.0f, 1.0f)));
}

// Text centered on a point, scaled down if needed to fit maxWidth.
void centeredText(ImDrawList* list, ImFont* font, float size, float maxWidth, ImVec2 center, ImU32 col,
                  const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    if (extent.x > maxWidth && extent.x > 0.0f) {
        size *= maxWidth / extent.x;
        extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    }
    list->AddText(font, size, ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), col, text);
}

// Text starting at a point's x, vertically centered on it, scaled down if
// needed to fit maxWidth.
void leftText(ImDrawList* list, ImFont* font, float size, float maxWidth, ImVec2 left, ImU32 col, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    if (extent.x > maxWidth && extent.x > 0.0f) {
        size *= maxWidth / extent.x;
        extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    }
    list->AddText(font, size, ImVec2(left.x, left.y - extent.y * 0.5f), col, text);
}

}  // namespace

Rect titleHintRect() {
    // Above the logo: the title's own text ("Press both A and B", the
    // copyright) fills the bottom of the image.
    return {-0.62f, -0.955f, 0.62f, -0.855f};
}

void drawTitleHint(const char* text, float imageX, float imageY, float imageWidth, float imageHeight) {
    if (text == nullptr || text[0] == '\0' || !(imageWidth > 0.0f) || !(imageHeight > 0.0f)) {
        return;
    }
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const ImageSpace image{imageX, imageY, imageWidth, imageHeight};
    const Rect r = titleHintRect();
    const ImVec2 barMin = image.point(r.x0, r.y0);
    const ImVec2 barMax = image.point(r.x1, r.y1);
    list->PushClipRect(image.point(-1.0f, -1.0f), image.point(1.0f, 1.0f), false);
    list->AddRectFilled(barMin, barMax, color(0.02f, 0.03f, 0.08f, 0.62f), (barMax.y - barMin.y) * 0.5f);
    centeredText(list, ImGui::GetFont(), imageHeight * 0.034f, (barMax.x - barMin.x) * 0.92f,
                 ImVec2((barMin.x + barMax.x) * 0.5f, (barMin.y + barMax.y) * 0.5f), color(1.0f, 1.0f, 1.0f, 0.95f),
                 text);
    list->PopClipRect();
}

// My Progress badges (petari/progress.hpp): a check and the best time under each mission star the player cleared
// themselves, while the mission select is up. Positions come from the game's own star panes (normalized game-image
// coordinates), so they follow the image at any window size.
void drawProgressBadges(float imageX, float imageY, float imageWidth, float imageHeight) {
    std::vector<Progress::BadgeStar> stars;
    std::string stage;
    if (!(imageWidth > 0.0f) || !(imageHeight > 0.0f) || !Progress::currentBadges(&stars, &stage)) {
        return;
    }
    ImDrawList* list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    list->PushClipRect(ImVec2(imageX, imageY), ImVec2(imageX + imageWidth, imageY + imageHeight), false);
    const float textSize = imageHeight * 0.03f;
    const float height = imageHeight * 0.045f;
    for (const Progress::BadgeStar& star : stars) {
        if (!star.cleared) {
            continue;
        }
        char text[32];
        const int whole = static_cast<int>(star.bestTimeS);
        std::snprintf(text, sizeof(text), "%d:%02d.%d", whole / 60, whole % 60, static_cast<int>((star.bestTimeS - whole) * 10.0));
        const ImVec2 extent = font->CalcTextSizeA(textSize, 1.0e9f, 0.0f, text);
        const float pad = height * 0.35f;
        const float width = height + extent.x + pad * 2.0f;
        const ImVec2 center(imageX + star.u * imageWidth, imageY + star.v * imageHeight + imageHeight * 0.085f);
        const ImVec2 min(center.x - width * 0.5f, center.y - height * 0.5f);
        const ImVec2 max(center.x + width * 0.5f, center.y + height * 0.5f);
        list->AddRectFilled(min, max, color(0.02f, 0.03f, 0.08f, 0.82f), height * 0.5f);
        list->AddRect(min, max, color(0.35f, 0.85f, 0.45f, 0.9f), height * 0.5f, 0, imageHeight * 0.002f);
        const ImVec2 badge(min.x + height * 0.5f, center.y);
        list->AddCircleFilled(badge, height * 0.34f, color(0.2f, 0.72f, 0.32f, 1.0f));
        const float unit = height * 0.34f;
        list->AddPolyline(std::array<ImVec2, 3>{ImVec2(badge.x - unit * 0.5f, badge.y), ImVec2(badge.x - unit * 0.12f, badge.y + unit * 0.4f),
                                                  ImVec2(badge.x + unit * 0.55f, badge.y - unit * 0.4f)}.data(),
                          3, color(1.0f, 1.0f, 1.0f, 1.0f), 0, imageHeight * 0.004f);
        list->AddText(font, textSize, ImVec2(min.x + height + pad * 0.6f, center.y - extent.y * 0.5f), color(1.0f, 1.0f, 1.0f, 0.97f), text);
    }
    // "x/y cleared by you" for the galaxy, in a corner of the image where the mission select leaves room.
    if (const App::LaunchStage::Galaxy* galaxy = App::LaunchStage::find(stage)) {
        int cleared = 0;
        for (int mission = 1; mission <= galaxy->missions; ++mission) {
            cleared += Progress::find(galaxy->stage, mission, nullptr) ? 1 : 0;
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%d/%d cleared by you", cleared, galaxy->missions);
        const float size = imageHeight * 0.03f;
        const ImVec2 extent = font->CalcTextSizeA(size, 1.0e9f, 0.0f, text);
        const float padX = size * 0.8f, padY = size * 0.4f;
        const ImVec2 min(imageX + imageWidth * 0.02f, imageY + imageHeight * 0.02f);
        const ImVec2 max(min.x + extent.x + padX * 2.0f, min.y + extent.y + padY * 2.0f);
        list->AddRectFilled(min, max, color(0.02f, 0.03f, 0.08f, 0.82f), (max.y - min.y) * 0.5f);
        list->AddRect(min, max, color(0.35f, 0.85f, 0.45f, 0.9f), (max.y - min.y) * 0.5f, 0, imageHeight * 0.002f);
        list->AddText(font, size, ImVec2(min.x + padX, min.y + padY), color(1.0f, 1.0f, 1.0f, 0.97f), text);
    }
    list->PopClipRect();
}

void drawImGuiOverlay(float imageX, float imageY, float imageWidth, float imageHeight) {
    const View view = publishedView();
#ifdef PETARI_HOME_MENU_INPUT
    // The title's "press A and B": say which keys do that. The text is built
    // from the bindings each time the prompt appears.
    static bool sPromptWasActive = false;
    static std::string sHint;
    const bool prompt = Input::titlePromptActive();
    if (prompt && !sPromptWasActive) {
        HostAllocationScope scope;  // this may be a game thread
        sHint = Input::titleHint(Input::bindings());
    }
    sPromptWasActive = prompt;
    if (prompt && !view.visible) {
        drawTitleHint(sHint.c_str(), imageX, imageY, imageWidth, imageHeight);
    }
#endif
    if (!view.visible) {
        drawProgressBadges(imageX, imageY, imageWidth, imageHeight);
    }
    if (!view.visible || !(imageWidth > 0.0f) || !(imageHeight > 0.0f)) {
        return;
    }

    // The foreground list draws over everything without a window, so the
    // overlay never takes ImGui focus or input.
    ImDrawList* list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImageSpace image{imageX, imageY, imageWidth, imageHeight};
    const ImVec2 imageMin = image.point(-1.0f, -1.0f);
    const ImVec2 imageMax = image.point(1.0f, 1.0f);
    list->PushClipRect(imageMin, imageMax, false);

    if (view.dimOpacity > 0.0f) {
        list->AddRectFilled(imageMin, imageMax, color(0.0f, 0.0f, 0.0f, view.dimOpacity));
    }

    const float alpha = view.panelOpacity;
    if (alpha > 0.0f) {
        const float rounding = imageHeight * 0.02f;
        const ImVec2 panelMin = image.point(view.panel.x0, view.panel.y0);
        const ImVec2 panelMax = image.point(view.panel.x1, view.panel.y1);
        list->AddRectFilled(panelMin, panelMax, color(0.06f, 0.08f, 0.16f, 0.94f * alpha), rounding);
        list->AddRect(panelMin, panelMax, color(0.55f, 0.65f, 0.95f, 0.6f * alpha), rounding, 0, imageHeight * 0.003f);

        const float panelCenterX = (panelMin.x + panelMax.x) * 0.5f;
        const float textWidth = (panelMax.x - panelMin.x) * 0.9f;
        const float titleSize = imageHeight * 0.05f;
        const float messageSize = imageHeight * 0.034f;
        const float itemSize = imageHeight * 0.04f;
        const float titleY = image.point(0.0f, view.titleY).y;
        const float messageY = image.point(0.0f, view.messageY).y;
        centeredText(list, font, titleSize, textWidth, ImVec2(panelCenterX, titleY), color(1.0f, 1.0f, 1.0f, alpha),
                     view.title);
        centeredText(list, font, messageSize, textWidth, ImVec2(panelCenterX, messageY),
                     color(0.78f, 0.82f, 0.92f, alpha), view.message);

        // Controls page: action names on the left, inputs on the right, one
        // row each, with a light band on every other row for reading across.
        if (view.lineCount > 0) {
            const ImVec2 areaMin = image.point(view.linesArea.x0, view.linesArea.y0);
            const ImVec2 areaMax = image.point(view.linesArea.x1, view.linesArea.y1);
            const float rowHeight = (areaMax.y - areaMin.y) / static_cast<float>(view.lineCount);
            const float areaWidth = areaMax.x - areaMin.x;
            const float textSize = std::min(rowHeight * 0.72f, imageHeight * 0.034f);
            const float pad = areaWidth * 0.02f;
            bool padColumn = false;
            for (int i = 0; i < view.lineCount; i++) {
                padColumn = padColumn || view.lines[i].pad[0] != '\0';
            }
            // Action | keyboard and mouse [| controller].
            const float split = areaMin.x + areaWidth * (padColumn ? 0.3f : 0.42f);
            const float split2 = padColumn ? areaMin.x + areaWidth * 0.69f : areaMax.x;
            for (int i = 0; i < view.lineCount; i++) {
                const float top = areaMin.y + rowHeight * static_cast<float>(i);
                const float middle = top + rowHeight * 0.5f;
                const bool header = view.lines[i].action[0] == '\0';
                if (i % 2 == 0 || header) {
                    list->AddRectFilled(ImVec2(areaMin.x, top), ImVec2(areaMax.x, top + rowHeight),
                                        color(1.0f, 1.0f, 1.0f, (header ? 0.12f : 0.06f) * alpha));
                }
                const ImU32 headerColor = color(0.78f, 0.82f, 0.92f, alpha);
                leftText(list, font, textSize, split - areaMin.x - 2.0f * pad, ImVec2(areaMin.x + pad, middle),
                         color(0.78f, 0.82f, 0.92f, alpha), view.lines[i].action);
                leftText(list, font, textSize, split2 - split - 2.0f * pad, ImVec2(split + pad, middle),
                         header ? headerColor : color(1.0f, 0.92f, 0.55f, alpha), view.lines[i].inputs);
                if (padColumn) {
                    leftText(list, font, textSize, areaMax.x - split2 - 2.0f * pad, ImVec2(split2 + pad, middle),
                             header ? headerColor : color(0.62f, 0.86f, 1.0f, alpha), view.lines[i].pad);
                }
            }
        }

        for (int i = 0; i < view.itemCount; i++) {
            const ViewItem& item = view.items[i];
            const ImVec2 itemMin = image.point(item.rect.x0, item.rect.y0);
            const ImVec2 itemMax = image.point(item.rect.x1, item.rect.y1);
            const ImVec2 center((itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f);
            const float labelWidth = (itemMax.x - itemMin.x) * 0.9f;
            if (item.focused) {
                list->AddRectFilled(itemMin, itemMax, color(0.98f, 0.80f, 0.22f, alpha), rounding);
                centeredText(list, font, itemSize, labelWidth, center, color(0.08f, 0.06f, 0.02f, alpha), item.label);
            } else {
                list->AddRectFilled(itemMin, itemMax, color(0.22f, 0.27f, 0.42f, 0.9f * alpha), rounding);
                centeredText(list, font, itemSize, labelWidth, center, color(0.92f, 0.94f, 1.0f, alpha), item.label);
            }
        }
    }

    if (view.blackOpacity > 0.0f) {
        list->AddRectFilled(imageMin, imageMax, color(0.0f, 0.0f, 0.0f, view.blackOpacity));
    }

    list->PopClipRect();
}

bool drawPipelinePreparation(const PipelinePreparationView& view, float width, float height) {
    if (!ImGui::GetCurrentContext() || width <= 0 || height <= 0) return false;
    ImDrawList* list = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float scale = std::min(width / 1280.0f, height / 720.0f);
    const float center = width * 0.5f;
    const float contentWidth = width * 0.82f;
    const float fraction = view.total ? std::clamp(float(view.completed) / view.total, 0.0f, 1.0f) : 1.0f;
    list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width, height),
        color(0.025f, 0.045f, 0.11f, 1), color(0.025f, 0.045f, 0.11f, 1),
        color(0.08f, 0.12f, 0.23f, 1), color(0.08f, 0.12f, 0.23f, 1));
    centeredText(list, font, 34 * scale, contentWidth, ImVec2(center, height * 0.34f),
                 color(0.96f, 0.98f, 1, 1), "Preparing shaders for smooth play");
    centeredText(list, font, 21 * scale, contentWidth, ImVec2(center, height * 0.405f),
                 color(0.65f, 0.76f, 0.91f, 1), view.global ? "This may take a few minutes after installation or updates" : "Preparing saved shaders");
    char count[96];
    std::snprintf(count, sizeof(count), "%u / %u  (%u%%)", view.completed, view.total,
                  static_cast<unsigned>(fraction * 100));
    centeredText(list, font, 25 * scale, contentWidth, ImVec2(center, height * 0.47f),
                 color(0.94f, 0.97f, 1, 1), count);
    const ImVec2 barMin(width * 0.21f, height * 0.515f), barMax(width * 0.79f, height * 0.537f);
    list->AddRectFilled(barMin, barMax, color(0.16f, 0.22f, 0.34f, 1), 7 * scale);
    if (fraction > 0)
        list->AddRectFilled(barMin, ImVec2(barMin.x + (barMax.x - barMin.x) * fraction, barMax.y),
                            color(0.45f, 0.75f, 1, 1), 7 * scale);
    char estimate[96] = "Estimating time remaining...";
    if (std::isfinite(view.remainingSeconds) && view.remainingSeconds >= 0) {
        const unsigned seconds = static_cast<unsigned>(std::min(59999.0, std::ceil(view.remainingSeconds)));
        if (seconds < 60) std::snprintf(estimate, sizeof(estimate), "About %u seconds remaining", seconds);
        else std::snprintf(estimate, sizeof(estimate), "About %u min %02u sec remaining", seconds / 60, seconds % 60);
    }
    centeredText(list, font, 20 * scale, contentWidth, ImVec2(center, height * 0.585f),
                 color(0.65f, 0.76f, 0.91f, 1), estimate);
    centeredText(list, font, 22 * scale, contentWidth, ImVec2(center, height * 0.70f),
                 color(0.94f, 0.89f, 0.65f, 1), "Press Return to start now (the rest keeps preparing in the background)");
    centeredText(list, font, 18 * scale, contentWidth, ImVec2(center, height * 0.75f),
                 color(0.65f, 0.76f, 0.91f, 1), "Controller A / Start also works");
    return true;
}

}  // namespace PetariNative::HomeMenu

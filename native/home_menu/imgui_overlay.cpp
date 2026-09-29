// Draws the native menu with Dear ImGui. Only ImGui and the menu's own header
// are included here, so ImGui's types never meet the SDK's GX headers.

#include "petari/home_menu.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace PetariNative::HomeMenu {

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

}  // namespace

void drawImGuiOverlay(float imageX, float imageY, float imageWidth, float imageHeight) {
    const View view = publishedView();
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
        const float firstItemTop = view.itemCount > 0 ? image.point(0.0f, view.items[0].rect.y0).y : panelMax.y;
        const float titleY = panelMin.y + (firstItemTop - panelMin.y) * 0.33f;
        const float messageY = panelMin.y + (firstItemTop - panelMin.y) * 0.66f;
        centeredText(list, font, titleSize, textWidth, ImVec2(panelCenterX, titleY), color(1.0f, 1.0f, 1.0f, alpha),
                     view.title);
        centeredText(list, font, messageSize, textWidth, ImVec2(panelCenterX, messageY),
                     color(0.78f, 0.82f, 0.92f, alpha), view.message);

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

}  // namespace PetariNative::HomeMenu

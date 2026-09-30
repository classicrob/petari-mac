// Draws the native menu with Dear ImGui. Only ImGui and the menu's own header
// are included here, so ImGui's types never meet the SDK's GX headers.

#include "petari/home_menu.hpp"

#include <imgui.h>

#ifdef PETARI_HOME_MENU_INPUT
#include <string>

#include "petari/host_allocation.hpp"
#include "petari/input.hpp"
#endif

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
            const float split = areaMin.x + areaWidth * 0.42f;
            for (int i = 0; i < view.lineCount; i++) {
                const float top = areaMin.y + rowHeight * static_cast<float>(i);
                const float middle = top + rowHeight * 0.5f;
                if (i % 2 == 0) {
                    list->AddRectFilled(ImVec2(areaMin.x, top), ImVec2(areaMax.x, top + rowHeight),
                                        color(1.0f, 1.0f, 1.0f, 0.06f * alpha));
                }
                leftText(list, font, textSize, split - areaMin.x - 2.0f * pad, ImVec2(areaMin.x + pad, middle),
                         color(0.78f, 0.82f, 0.92f, alpha), view.lines[i].action);
                leftText(list, font, textSize, areaMax.x - split - 2.0f * pad, ImVec2(split + pad, middle),
                         color(1.0f, 0.92f, 0.55f, alpha), view.lines[i].inputs);
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

}  // namespace PetariNative::HomeMenu

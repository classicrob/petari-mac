#pragma once

// Native-only UI observation for the automated smoke run (native/app, petari/ui_observe.hpp):
// screen positions and pointing state of pointable targets, published by game code.
// Observation only; nothing here changes game state. Not compiled for the Wii.
#ifdef PETARI_NATIVE
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/LayoutCoreUtil.hpp"
#include "Game/Screen/LayoutManager.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <nw4r/lyt/pane.h>
#include <petari/ui_observe.hpp>

namespace MR {
    namespace Native {
        // Both publish only while petari_ui_observing(). pId must have static storage.
        // rScreenPos is in the star pointer's screen space (MR::getScreenWidth x MR::getScreenHeight).
        inline void publishUiTarget(const char* pId, s32 index, const TVec2f& rScreenPos, u32 flags) {
            if (!petari_ui_observing()) {
                return;
            }

            petari_ui_target(pId, index, rScreenPos.x / MR::getScreenWidth(), rScreenPos.y / MR::getScreenHeight(), flags);
        }

        // Publishes the centre of a bounding pane, taken as LayoutManager::isPointing bounds it.
        inline void publishUiPaneTarget(const LayoutActor* pHost, const char* pPaneName, const char* pId, s32 index, u32 flags) {
            if (!petari_ui_observing()) {
                return;
            }

            const nw4r::lyt::Pane* pPane = pHost->getLayoutManager()->getPane(pPaneName);
            if (pPane == nullptr) {
                return;
            }

            s32 horizontalPosition = static_cast< u8 >(pPane->mBasePosition % 3);
            s32 verticalPosition = static_cast< u8 >(pPane->mBasePosition / 3);
            TVec2f localCenter(0.0f, 0.0f);
            localCenter.x = horizontalPosition == 0 ? pPane->mSize.width / 2.0f : (horizontalPosition == 2 ? -pPane->mSize.width / 2.0f : 0.0f);
            localCenter.y = verticalPosition == 0 ? -pPane->mSize.height / 2.0f : (verticalPosition == 2 ? pPane->mSize.height / 2.0f : 0.0f);

            TVec2f screenPos;
            MR::convertPaneLocalPosToScreenPos(&screenPos, pPane, localCenter);
            publishUiTarget(pId, index, screenPos, flags);
        }
    }  // namespace Native
}  // namespace MR
#endif

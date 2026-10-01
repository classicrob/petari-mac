#pragma once

#ifdef PETARI_NATIVE
// Photo mode (docs/dev/ODYSSEY_CAMERA.md "Photo mode"): GameScene freezes like its
// pause menu (no movement list, no pause menu), the HUD is not drawn, and a free
// camera flies over the frozen scene; leaving restores the game's view exactly.
namespace PhotoCamera {
    // Game thread, once per GameScene update while not in photo mode: true when a
    // PhotoMode press asks to enter and the scene can pause now (else refused).
    bool takeEnterRequest(bool canPause);
    void start();
    // One frame in photo mode: fly, screenshots. True when the player leaves.
    bool update();
    void end();
    bool isActive();
    // GameScene's movement list ran one frame (the game's own clock).
    void noteSceneFrame();
}  // namespace PhotoCamera
#endif

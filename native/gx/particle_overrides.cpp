// Native build of the game's JParticle overrides (emitter init, draw and field functions,
// JPAEmitterManager::calcYBBCam) from src/Game/System/Overwrite.cpp. The same source is used;
// the native game build of Overwrite.cpp omits this section, so there is one definition.
// Depends on the game's MR:: vector helpers (Game/Util/MathUtil.cpp).
#define PETARI_OVERWRITE_PARTICLES 1
#include "../../src/Game/System/Overwrite.cpp"

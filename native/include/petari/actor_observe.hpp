#pragma once
// Where the game's actors are, for the automated smoke run's mission scripts
// (native/app/smoke_goodegg.hpp). Game-side native-only hooks publish once per
// frame while an actor is active (its control() runs); the app reads once per
// frame at the frame seam. Observation only: publishing never changes game
// behaviour. Plain C, no SDK or Aurora types.
//
// An actor not published in a frame is not active that frame: dead, hidden,
// clipped (far from the camera) or not yet created. Scripts must treat absence
// as "unknown", not as "gone", unless the actor is near Mario.
//
// Kinds (static strings) and their states are listed in
// include/Game/Util/NativeActorObserve.hpp.

#ifdef __cplusplus
extern "C" {
#endif

/* Actor flag bits (kind-specific meanings are listed with the kinds). */
#define PETARI_ACTOR_READY 1u   /* usable now (a launch star accepting Mario, a star to touch, a ball to hit) */
#define PETARI_ACTOR_BOUND 2u   /* holds Mario (a launch star in flight, a vine he hangs on) */
#define PETARI_ACTOR_HOSTILE 4u /* touching it hurts Mario now */

/* An active actor this frame: its kind (static string), its position and a
 * direction (front or up, per kind), a kind-specific state number and flags.
 * Game thread. */
void petari_actor(const char* kind, float x, float y, float z, float dx, float dy, float dz, int state,
                  unsigned flags);

#ifdef __cplusplus
}
#endif

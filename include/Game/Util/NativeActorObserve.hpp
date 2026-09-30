#pragma once

// Native-only actor observation for the automated smoke run's mission scripts
// (native/app/smoke_goodegg.hpp, petari/actor_observe.hpp): where actors are and
// what they are doing, published each frame they are active (from control(), or
// as noted: CrystalCage from exeWait, Plant from calcAnim).
// Observation only; nothing here changes game state. Not compiled for the Wii.
//
// Kinds, direction and state (flags: petari/actor_observe.hpp):
//   "LaunchStar"   SuperSpinDriver. dir: up. state 1 Wait, 2 Capture, 3 ShootStart,
//                  4 Shoot, 5 CoolDown, 6 Appear, 0 otherwise. READY: accepts Mario
//                  (Wait past its bind delay). BOUND: holds Mario.
//   "SlingStar"    SpinDriver. dir: up. state 1 Wait, 2 Capture/ShootStart, 3 Shoot,
//                  0 otherwise. READY: Wait. BOUND: holds Mario.
//   "Luma"         Tico. dir: up. state 0.
//   "DinoPiranha"  DinoPackun. dir: front. state: the fight's sequence (DinoPackunVs1)
//                  0 start/opening demo, 1 egg, 2 cry demo, 3 level 1, 4 angry demo,
//                  5 level 2, 6 level 3, 7 down demo, 8 other. HOSTILE: in a battle level.
//   "DinoBall"     DinoPackunBall (the tail's end). dir: up. state 1 Wait, 0 otherwise.
//                  READY: Wait (a spin punches it during a battle level).
//   "PowerStar"    PowerStar. dir: up. state 1 Wait, 2 appear demo, 3 stage-clear demo,
//                  0 otherwise. READY: Wait (touching it collects it).
//   "StarChip"     ChipBase (YellowChip, BlueChip). dir: up. state: the chip type.
//                  READY: collectible (waiting or flashing).
//   "PiranhaPlant" PackunPetit. dir: front. state 0, HOSTILE; 1 knocked down.
//   "HammerHead"   HammerHeadPackun, at its "Head" joint. dir: front. HOSTILE. READY: its
//                  head can be hit now (a spin swoons it; landing on it kills it).
//   "CrystalCage"  CrystalCage. dir: up. READY. Published only while intact.
//   "Goomba"       Kuribo. dir: up. HOSTILE.   "Octoomba" Takobo. dir: up. HOSTILE.
//   "Karipon"      Karikari (clings to Mario; a spin shakes it off). dir: up. state 1
//                  clinging. HOSTILE.
//   "Rock"         Rock (a rolling boulder, body radius 225 x scale). dir: its motion since
//                  the previous frame (units/frame). state: its type. HOSTILE.
//   "StarBall"     Tamakoro. dir: up. state 1 Mario rides it (BOUND), 0 waiting (READY).
//   "Ray"          SurfRay. dir: its front. state: 1 ridden (BOUND) plus 2 on the water.
//   "FlipPanel"    FlipPanel (from calcAndSetBaseMtx). dir: up. state 1 flipped to its back, 0 front.
//   "Vine"         Plant. position: its moving part. state: 1 growing, 2 Mario hangs,
//                  3 grown, 0 otherwise. BOUND: Mario hangs on it.
#ifdef PETARI_NATIVE
#include <JSystem/JGeometry/TVec.hpp>
#include <petari/actor_observe.hpp>
#include <petari/ui_observe.hpp>

namespace MR {
    namespace Native {
        // pKind must have static storage.
        inline void publishActor(const char* pKind, const TVec3f& rPos, const TVec3f& rDir, s32 state, u32 flags) {
            if (!petari_ui_observing()) {
                return;
            }

            petari_actor(pKind, rPos.x, rPos.y, rPos.z, rDir.x, rDir.y, rDir.z, state, flags);
        }
    }  // namespace Native
}  // namespace MR
#endif

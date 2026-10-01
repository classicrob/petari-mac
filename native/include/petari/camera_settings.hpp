#pragma once
// Odyssey-style orbit camera mod (docs/dev/ODYSSEY_CAMERA.md): its settings and
// the host-side input feed. Off by default; while off nothing is accumulated and
// every input keeps its normal meaning (the right stick moves the Star Pointer).

#include <filesystem>
#include <string>

namespace PetariNative::CameraSettings {

// camera.txt in the user directory: OdysseyCamera=on|off, Speed=1..5,
// InvertX=on|off, InvertY=on|off. A missing file keeps the defaults (off).
// PETARI_ODYSSEY_CAMERA=1|0 overrides on/off for a run. Nothing changes on error.
bool load(const std::filesystem::path& file, std::string* error);
bool save(std::string* error);  // no-op before load
std::string serialize();
bool parse(const std::string& text, std::string* error);

bool enabled();  // any thread
void setEnabled(bool on);
int speed();  // 1..5, default 3
void setSpeed(int value);
bool invertX();
void setInvertX(bool on);
bool invertY();
void setInvertY(bool on);
// What scrolling does: Auto (precise trackpad scroll orbits, wheel clicks zoom),
// Orbit, or Zoom. camera.txt ScrollMode=auto|orbit|zoom.
enum class ScrollMode { Auto, Orbit, Zoom };
ScrollMode scrollMode();
void setScrollMode(ScrollMode mode);

// Host input (wpad_host.cpp calls these only while enabled()).
void stick(float x, float y);         // right stick, x right and y up, -1..1
void mouseDrag(float dx, float dy);   // window points while CameraOrbitHold is held
void scrollOrbit(float x, float y);   // trackpad scroll units (x yaw, y pitch)
void zoomSteps(float steps);          // + = farther
void yawHold(int direction);          // -1/0/+1
void pitchHold(int direction);        // -1/0/+1, + = camera rises
void zoomHold(int direction);         // -1/0/+1, + = farther
void recenter();
void resetInput();

void resetForTesting();

}  // namespace PetariNative::CameraSettings

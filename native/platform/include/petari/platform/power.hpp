#pragma once
// Power and reset on the host.
//
// The console's power and reset buttons become host events the application
// raises: pressPowerButton() for a quit request (window close, Cmd+Q) and
// setResetButton() for a reset key. They reach the game exactly as the STM
// device's events do: one-shot power/reset callbacks run with interrupts
// disabled, and OSGetResetButtonState() reports a press once.
//
// When the game leaves (OSShutdownSystem, OSRestart, OSReturnToMenu,
// OSRebootSystem), registered shutdown functions run as on the console
// (repeated non-final passes until all succeed, then a final pass with
// interrupts disabled), then the application's exit handler receives the
// intent. These OS functions never return: if the handler returns, the call
// aborts. Without a handler:
// - Shutdown, ReturnToMenu, Reboot: the process exits with status 0 (there is
//   no Wii Menu or console to return to);
// - Restart: the process relaunches itself with the reset code, which the
//   new instance reports through OSGetResetCode()/OSIsRestart().

#include <cstdint>

namespace PetariNative::Platform::Power {

enum class Intent {
    Shutdown,      // OSShutdownSystem: power off
    Restart,       // OSRestart: relaunch this title with a reset code
    ReturnToMenu,  // OSReturnToMenu: Wii Menu (unavailable natively)
    Reboot,        // OSRebootSystem: reboot the console (unavailable natively)
};

struct Exit {
    Intent intent;
    std::uint32_t resetCode;      // OSRestart's argument (Restart only)
    std::uint32_t shutdownEvent;  // event given to shutdown functions: 2, 4, 5, 1
};

// Must not return: it should quit, relaunch (see relaunch()), or hand control
// to the host application's exit path.
using ExitHandler = void (*)(const Exit& exit, void* user);
void setExitHandler(ExitHandler handler, void* user);

// Host events. May be called from any thread; the callbacks run on the
// calling thread with interrupts disabled and must not block.
void pressPowerButton();
void setResetButton(bool pressed);

// Replaces this process with a fresh instance of the same executable (same
// arguments), which will report resetCode through OSGetResetCode(). Returns
// only on failure.
void relaunch(std::uint32_t resetCode);

// Environment variable carrying the reset code into a relaunched instance.
inline constexpr const char* kResetCodeEnvironmentVariable = "PETARI_RESET_CODE";

}  // namespace PetariNative::Platform::Power

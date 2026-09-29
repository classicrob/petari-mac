#pragma once
// Native audio DSP device.
//
// Implements the SDK DSP mailbox (<revolution/dsp.h>) and task boot for the
// JAudio2 microcode (jdsp[], HashEctor 0xD643001F), whose behaviour is
// rendered natively (see native/platform/audio/dsp_renderer.hpp for
// provenance). JAudio2's host code (dsptask.cpp, osdsp_task.cpp, dspproc.cpp,
// JASAudioThread) keeps the console's mail protocol, command acks, and
// interrupts. Host pointers cannot fit the protocol's 32-bit mails, so
// addresses travel as handles from addressHandle().
//
// Threads: a DSP worker thread consumes mails, runs commands, and renders
// subframes; a DSP interrupt thread calls __DSPHandler (JAudio2's) with
// interrupts disabled for each interrupting mail, as the DSP interrupt does.

#include <cstdint>

namespace PetariNative::Platform::DSP {

// Returns a stable nonzero 32-bit handle for a host address, for use where the
// console sends a 32-bit main-memory address to the DSP. The same address
// always gets the same handle. Aborts if more than 64 addresses are
// registered (JAudio2 uses about a dozen).
std::uint32_t addressHandle(std::uintptr_t address);

// Microcode identity as Dolphin and this port compute it (HashEctor over the
// big-endian IRAM image).
std::uint32_t microcodeHash(const std::uint16_t* iram, std::uint32_t lengthBytes);

// Stops the DSP threads and forgets all state. For tests and shutdown.
void shutdown();

}  // namespace PetariNative::Platform::DSP

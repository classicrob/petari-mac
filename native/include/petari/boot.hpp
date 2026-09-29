#pragma once

// Initialize host arenas and bind the main thread to the Wii scheduler.
// Call on the macOS main thread before constructing any game heaps or threads.
extern "C" void OSInit();

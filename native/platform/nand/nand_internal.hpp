#pragma once
// Shared between the synchronous NAND port (nand_host.cpp) and the
// asynchronous one (nand_async.cpp). Results are ISFS error codes.

#include <revolution/fs.h>
#include <revolution/types.h>

namespace PetariNative::Platform::NAND {

ISFSError createNode(const char* path, u8 perm, u8 attr, bool privileged, bool directory);
ISFSError deleteNode(const char* path, bool privileged);

// Stops the asynchronous request thread, dropping queued requests (their
// callbacks never run, as when IOS is shut down). Called by shutdown().
void stopAsync();

}  // namespace PetariNative::Platform::NAND

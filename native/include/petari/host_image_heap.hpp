#pragma once
// Where the J3D loaders put host-layout copies of big-endian files.
//
// Natively J3DModelLoaderDataBase and J3DAnmLoaderDataBase convert a big-endian
// model or animation file into a host-layout copy (petari/j3d_model.hpp,
// petari/j3d_animation.hpp) and build the J3D objects from the copy. The source
// resource is never modified. By default the copy is allocated on the current
// JKR heap, next to the objects. A registered resolver may name another heap for
// the copy of a given source (for example a companion of the heap that holds the
// source archive); it returns null to keep the current heap.
//
// Sources that are already host images are used as they are: no copy is made
// and the resolver is not called.

class JKRHeap;

namespace PetariNative::J3D {

using HostImageHeapResolver = JKRHeap* (*)(const void* pSource);

// Registers the resolver (null removes it). Not thread-safe against concurrent loads.
void setHostImageHeapResolver(HostImageHeapResolver resolver);

// The heap for the host copy of pSource: the resolver's answer, or null for the current heap.
JKRHeap* resolveHostImageHeap(const void* pSource);

}  // namespace PetariNative::J3D

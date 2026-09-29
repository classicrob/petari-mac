#include <cstdint>

extern "C" {
void* PetariNativePhysicalToHost(std::uintptr_t address);
std::uintptr_t PetariNativeHostToPhysical(const void* pointer);

// Aurora's SDK declarations call these symbols instead of Petari's macros.
void* OSPhysicalToCached(std::uint32_t address) { return PetariNativePhysicalToHost(address); }
void* OSPhysicalToUncached(std::uint32_t address) { return PetariNativePhysicalToHost(address); }
std::uint32_t OSCachedToPhysical(void* pointer) {
    return static_cast<std::uint32_t>(PetariNativeHostToPhysical(pointer));
}
std::uint32_t OSUncachedToPhysical(void* pointer) { return OSCachedToPhysical(pointer); }
void* OSCachedToUncached(void* pointer) {
    return PetariNativePhysicalToHost(PetariNativeHostToPhysical(pointer));
}
void* OSUncachedToCached(void* pointer) { return OSCachedToUncached(pointer); }
}

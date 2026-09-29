#include <petari/host_allocation.hpp>

namespace PetariNative {
namespace {
thread_local bool sGameAllocationThread;
thread_local unsigned int sHostAllocationDepth;
}

void setGameAllocationThread(bool enabled) {
    sGameAllocationThread = enabled;
}

bool isGameAllocationThread() {
    return sGameAllocationThread;
}

HostAllocationScope::HostAllocationScope() {
    sHostAllocationDepth++;
}

HostAllocationScope::~HostAllocationScope() {
    sHostAllocationDepth--;
}

bool isHostAllocationActive() {
    return !sGameAllocationThread || sHostAllocationDepth != 0;
}
}  // namespace PetariNative
